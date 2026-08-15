// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/08

// loom-desktop is the Wails desktop frontend (docs/DESKTOP_DESIGN.md): it
// assembles the runtime exactly like `loom serve`, then mounts the server's
// fully-middlewared handler into the webview's AssetServer — the SPA talks
// REST+SSE in-process, so by default there is no TCP listener at all. The
// optional LAN share listener (config share.*) exposes only the public
// read-only share surface for session share links.
//
// The bootstrap helpers below intentionally duplicate cmd/loom/main.go's:
// exporting them would couple the CGO-free `loom` binary to this command's
// build graph. Keep them byte-compatible with the originals.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	wails "github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/mac"
	wailsrt "github.com/wailsapp/wails/v2/pkg/runtime"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/server"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/version"
)

const artifactDirectoryName = "artifacts"

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "loom-desktop:", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string) error {
	var printConn bool
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "--print-connection":
			printConn = true
		case args[i] == "version":
			fmt.Println("loom-desktop", version.Version)
			return nil
		default:
			return fmt.Errorf("usage: loom-desktop [--print-connection] | loom-desktop version")
		}
	}

	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig()
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved); err != nil {
		return err
	}

	// A Finder-launched .app gets "/" as cwd. Reopen the most recently active
	// project (derived from session history — the same source of truth the
	// SPA's sidebar focus uses) instead of asking on every launch; the native
	// picker appears only when there is no history to derive from (first
	// launch), and a cancel exits cleanly. Terminal launches keep the project
	// directory, matching `loom` semantics.
	if root == "/" {
		root = lastActiveWorkspaceRoot(ctx, resolved)
		if root == "" {
			picked, perr := chooseFolder("Choose a workspace directory for Loom")
			switch {
			case perr == nil:
				root = picked
			case errors.Is(perr, errPickCancelled):
				return nil
			default:
				// AppleScript unavailable (stripped-down system): fall back to
				// the home directory rather than refuse to start.
				fmt.Fprintf(os.Stderr, "loom-desktop: folder picker unavailable (%v), using home directory\n", perr)
				if home, herr := os.UserHomeDir(); herr == nil {
					root = home
				}
			}
		}
	}

	// Single-instance discipline: same data-dir flock as `loom serve`.
	dataDir := resolved.Storage.SessionsDir()
	lock, err := server.AcquireDataDirLock(dataDir)
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			// A Finder-launched second instance has no visible stderr; tell
			// the user instead of dying silently.
			showAlert("Loom is already running",
				fmt.Sprintf("Another Loom process already owns the data directory:\n%s\n\nStop it first, then relaunch Loom.", dataDir))
			return fmt.Errorf("another loom process already owns %s (stop it first)", dataDir)
		}
		return err
	}
	defer lock.Release()

	logger := newFileLogger(resolved, slog.New(logging.NewGlogHandler(os.Stderr, nil)))
	proc, registry, bootstrap, err := assembleRuntime(ctx, resolved, root, logger)
	if err != nil {
		return err
	}
	defer proc.Close()
	defer registry.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	app.WireSubagentObserver(bootstrap.SubagentFactory, broker, bootstrap.Store, logger)

	// In-process token: random per launch, never persisted or printed.
	token, err := generateToken()
	if err != nil {
		return err
	}

	// The LAN share listener is built lazily by the manager (runtime
	// toggle / config hot-apply); the factory dereferences service only
	// when a listener actually starts, which is always after the
	// assignment below.
	var service *app.SessionService
	shareMgr := server.NewShareManager(func(listen string) (*server.Server, error) {
		return server.New(server.Config{
			Listen: listen, Token: token, Version: version.Version,
			Service: service, Logger: logger, ShareOnly: true,
		})
	}, logger)
	service = app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{
		Logger:        logger,
		ShareEndpoint: shareMgr,
		RulesDir:      resolved.Storage.RulesDir(),
		// The webview surfaces approval requests in-page; a desktop
		// banner on top of that is noise for the user staring at it.
		DisableApprovalNotify: true,
	})

	// UI loopback listener: always on, random port. The webview talks real
	// HTTP here because the Wails AssetServer channel cannot carry our
	// protocol: it reports ContentLength=-1 for every request and its
	// response writer has no http.Flusher, so SSE streaming is impossible
	// (docs/DESKTOP_DESIGN.md §2.3, R-B2 materialized).
	uiSrv, err := server.New(server.Config{
		Listen:     "127.0.0.1:0",
		Token:      token,
		Version:    version.Version,
		Service:    service,
		Logger:     logger,
		ConfigPath: config.ConfigPathForHome(resolved.Storage.BaseDir),
		Share:      shareMgr,
	})
	if err != nil {
		return err
	}
	if err := uiSrv.Listen(); err != nil {
		return err
	}
	uiBase := "http://" + uiSrv.Addr()
	go func() {
		if err := uiSrv.Serve(); err != nil {
			logger.Error("ui listener died", "error", err)
		}
	}()

	// Start the LAN share listener when the config opted in; a bind
	// failure degrades to "sharing off" — the settings toggle retries.
	if err := shareMgr.Apply(resolved.Share.Enabled, resolved.Share.Listen); err != nil {
		logger.Warn("share endpoint start failed", "error", err)
	}

	shutdown := func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if err := uiSrv.Shutdown(shutdownCtx); err != nil {
			logger.Warn("ui http shutdown", "error", err)
		}
		shareMgr.Close()
		if err := service.Shutdown(shutdownCtx); err != nil {
			logger.Warn("service shutdown", "error", err)
		}
		broker.Close()
	}

	// The webview starts on a bootstrap page that immediately redirects to
	// the loopback UI; the token rides in the URL fragment, which never
	// reaches the wire (docs/DESKTOP_DESIGN.md §4.2).
	handler := bootstrapHandler(uiBase + "/#token=" + token)
	if printConn {
		// Debug aid: the full URL (with the in-process token) for driving the
		// loopback UI from a browser or curl.
		fmt.Fprintf(os.Stderr, "loom-desktop: web UI at %s/#token=%s\n", uiBase, token)
	}

	// wailsReady carries the wails runtime context so the OS signal path can
	// ask the GUI loop to quit (which in turn runs OnShutdown).
	wailsReady := make(chan context.Context, 1)
	go func() {
		<-ctx.Done()
		select {
		case wctx := <-wailsReady:
			wailsrt.Quit(wctx)
		case <-time.After(5 * time.Second):
			// The signal arrived before the GUI loop was ready (e.g. SIGTERM
			// during startup): nothing else will stop the process.
			os.Exit(0)
		}
	}()

	// Restore the previous window geometry when it still fits a connected
	// display; otherwise fall back to the defaults.
	width, height := defaultWindowWidth, defaultWindowHeight
	wsPath := windowStatePath(resolved)
	savedWs, hasSavedWs := loadWindowState(wsPath)
	if hasSavedWs {
		width, height = savedWs.Width, savedWs.Height
	}

	logger.Info("loom-desktop starting", "version", version.Version, "share_listen", resolved.Share.Enabled)
	return wails.Run(&options.App{
		Title:     "Loom",
		Width:     width,
		Height:    height,
		MinWidth:  minWindowWidth,
		MinHeight: minWindowHeight,
		AssetServer: &assetserver.Options{
			Handler: handler,
		},
		Mac: &mac.Options{
			// TitleBarHidden: no native title bar — the webview extends to
			// the window top and the traffic lights overlay the 28px top
			// rows (see app.css .is-desktop rules). Note: HiddenInset is
			// NOT used on purpose — it sets UseToolbar=true, which attaches
			// an empty NSToolbar and pushes the lights down into a ~52px
			// unified toolbar band (center ~27px), misaligning them with
			// the web UI's 28px top rows (center 14px).
			TitleBar: mac.TitleBarHidden(),
			About: &mac.AboutInfo{
				Title:   "Loom",
				Message: "Loom Desktop " + version.Version,
			},
		},
		OnStartup: func(wctx context.Context) {
			select {
			case wailsReady <- wctx:
			default:
			}
			if hasSavedWs {
				restoreWindowPosition(wctx, savedWs)
			}
			go persistWindowState(ctx, wctx, wsPath, logger)
		},
		OnShutdown: func(context.Context) {
			shutdown()
		},
	})
}

// --- bootstrap page (docs/DESKTOP_DESIGN.md §2.3) ---

// bootstrapHandler serves the single page the webview starts on: an
// immediate meta-refresh redirect to the loopback UI. The token rides in
// the URL fragment (never sent to the server). Meta refresh is used instead
// of a 30x because the Wails AssetServer on darwin does not support
// redirects.
func bootstrapHandler(target string) http.Handler {
	page := `<!doctype html><meta charset="utf-8"><title>Loom</title>` +
		`<meta http-equiv="refresh" content="0;url=` + target + `">` +
		`<style>body{background:#1c2424;color:#9db2ad;font:14px system-ui;display:grid;place-items:center;height:100vh;margin:0}</style>` +
		`<p>Starting Loom&hellip; <a style="color:#7fbbb3" href="` + target + `">continue</a></p>`
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = io.WriteString(w, page)
	})
}

// generateToken returns a fresh 128-bit bearer token (hex). Unlike
// `loom serve`, the desktop token is never written to disk: it is handed to
// the embedded webview in-process (docs/DESKTOP_DESIGN.md §2.1).
func generateToken() (string, error) {
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate token: %w", err)
	}
	return hex.EncodeToString(raw), nil
}

// lastActiveWorkspaceRoot derives the desktop default workspace from the
// most recently updated session, so a Finder-launched Loom reopens in the
// project the user last worked in — the same source of truth the SPA's
// sidebar focus and composer default use. Returns "" when there is no
// usable history (first launch, legacy rows, deleted/moved workspace); the
// caller then falls back to asking once. Read-only: safe to run while
// another loom process owns the data dir (WAL allows concurrent readers).
func lastActiveWorkspaceRoot(ctx context.Context, resolved *config.ResolvedConfig) string {
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); err != nil {
		return ""
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return ""
	}
	defer store.Close()
	summaries, _, err := store.ListSessions(ctx, "", 1, false, domain.WorkspaceID{})
	if err != nil || len(summaries) == 0 {
		return ""
	}
	wsID, err := store.SessionWorkspace(ctx, summaries[0].ID)
	if err != nil {
		return ""
	}
	ws, err := store.GetWorkspace(ctx, wsID)
	if err != nil || ws.RootPath == "" {
		return ""
	}
	// The directory may have moved since the workspace was registered.
	if info, err := os.Stat(ws.RootPath); err != nil || !info.IsDir() {
		return ""
	}
	return ws.RootPath
}

// --- bootstrap helpers (kept in sync with cmd/loom/main.go) ---

func loadConfig() (*config.ResolvedConfig, error) {
	home, err := config.HomeDir(os.LookupEnv)
	if err != nil {
		return nil, err
	}
	load := func() (*config.ResolvedConfig, error) {
		return config.Load(home, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
	}
	resolved, err := load()
	if err != nil {
		// First run: a missing config in the default loom home is a fresh
		// install. Write the starter template and keep booting — the
		// settings UI collects the API key from there. A missing config
		// in an explicit LOOM_HOME stays a hard error: the user pointed
		// loom at a home that should already be set up.
		if errors.Is(err, config.ErrConfigNotFound) {
			if created, cerr := config.EnsureFirstRunConfig(home); cerr != nil {
				return nil, cerr
			} else if created {
				return load()
			}
		}
		return nil, err
	}
	return resolved, nil
}

func newFileLogger(resolved *config.ResolvedConfig, fallback *slog.Logger) *slog.Logger {
	logger, err := logging.NewFileLogger(resolved.Storage.LogsDir(), nil, logging.Quotas{
		MaxFileBytes:  resolved.Logging.MaxFileBytes,
		MaxTotalBytes: resolved.Logging.MaxTotalBytes,
	})
	if err != nil {
		return fallback
	}
	return logger
}

func prepareStorage(resolved *config.ResolvedConfig) error {
	if err := preparePrivateDataDirectory(resolved.Storage.BaseDir); err != nil {
		return err
	}
	return preparePrivateDataDirectory(resolved.Storage.SessionsDir())
}

func preparePrivateDataDirectory(directory string) error {
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return fmt.Errorf("create loom data directory: %w", err)
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return fmt.Errorf("inspect loom data directory: %w", err)
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
		return errors.New("loom data directory must be a real directory")
	}
	if err := os.Chmod(directory, 0o700); err != nil {
		return fmt.Errorf("secure loom data directory: %w", err)
	}
	return nil
}

func resolveWorkspace(explicit string) (string, error) {
	if root := strings.TrimSpace(explicit); root != "" {
		return root, nil
	}
	if root := strings.TrimSpace(os.Getenv("BUILD_WORKSPACE_DIRECTORY")); root != "" {
		return root, nil
	}
	root, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("get workspace: %w", err)
	}
	return root, nil
}

// assembleRuntime mirrors cmd/loom's shared assembly: the desktop app is a
// peer of chat/serve and must boot identically (docs/DESKTOP_DESIGN.md §3.2).
func assembleRuntime(ctx context.Context, resolved *config.ResolvedConfig, root string, logger *slog.Logger) (*app.ProcessRuntime, *app.WorkspaceRegistry, *app.Bootstrap, error) {
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(resolved.Storage.SessionsDir(), artifactDirectoryName),
		Version:     version.Version,
		Logger:      logger,
	})
	if err != nil {
		return nil, nil, nil, fmt.Errorf("process runtime: %w", err)
	}
	registry, err := app.NewWorkspaceRegistry(proc)
	if err != nil {
		proc.Close()
		return nil, nil, nil, fmt.Errorf("workspace registry: %w", err)
	}
	defaultWs, err := registry.RegisterDefault(ctx, root)
	if err != nil {
		registry.Close()
		proc.Close()
		return nil, nil, nil, fmt.Errorf("default workspace: %w", err)
	}
	for _, wc := range resolved.Workspaces {
		if _, err := registry.Register(ctx, wc.Root, wc.Name); err != nil {
			logger.Warn("workspace pre-register skipped", "root", wc.Root, "error", err)
		}
	}
	return proc, registry, defaultWs, nil
}
