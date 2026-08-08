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
// REST+SSE in-process, so by default there is no TCP listener at all. An
// optional --listen address exposes the server on the LAN for session share
// links.
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
	"net"
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

const (
	artifactDirectoryName = "artifacts"
	// configPathEnv points at an alternative config file (same locator
	// semantics as cmd/loom).
	configPathEnv = "LOOM_CONFIG"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "loom-desktop:", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string) error {
	var listen, advertise string
	var printConn bool
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "--listen" && i+1 < len(args):
			i++
			listen = args[i]
		case args[i] == "--advertise" && i+1 < len(args):
			i++
			advertise = args[i]
		case args[i] == "--print-connection":
			printConn = true
		case args[i] == "version":
			fmt.Println("loom-desktop", version.Version)
			return nil
		default:
			return fmt.Errorf("usage: loom-desktop [--listen <addr>] [--advertise <base-url>] [--print-connection] | loom-desktop version")
		}
	}

	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	cfgPath, err := configPath()
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
	service := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{Logger: logger})

	// Mirror attention-worthy agent milestones to Notification Center.
	go watchNotifications(ctx, broker, logger)

	// In-process token: random per launch, never persisted or printed.
	token, err := generateToken()
	if err != nil {
		return err
	}

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
		ConfigPath: cfgPath,
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

	// Optional second listener for LAN session sharing
	// (docs/DESKTOP_DESIGN.md §5). Resolve :0 to a concrete port up front so
	// PublicBaseURL (which embeds the port) is known before server.New
	// validates it.
	var shareSrv *server.Server
	if listen != "" {
		if listen, err = resolveListenPort(listen); err != nil {
			return err
		}
		publicBase, derr := derivePublicBase(advertise, listen)
		if derr != nil {
			return derr
		}
		if shareSrv, err = server.New(server.Config{
			Listen:        listen,
			Token:         token,
			Version:       version.Version,
			Service:       service,
			Logger:        logger,
			PublicBaseURL: publicBase,
			ConfigPath:    cfgPath,
		}); err != nil {
			return err
		}
		if err := shareSrv.Listen(); err != nil {
			return err
		}
		logger.Info("loom-desktop share endpoint", "base", publicBase)
		go func() {
			if err := shareSrv.Serve(); err != nil {
				logger.Error("share listener died", "error", err)
			}
		}()
	} else if advertise != "" {
		return errors.New("--advertise requires --listen (no listener, nothing to advertise)")
	}

	shutdown := func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if err := uiSrv.Shutdown(shutdownCtx); err != nil {
			logger.Warn("ui http shutdown", "error", err)
		}
		if shareSrv != nil {
			if err := shareSrv.Shutdown(shutdownCtx); err != nil {
				logger.Warn("share http shutdown", "error", err)
			}
		}
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

	logger.Info("loom-desktop starting", "version", version.Version, "share_listen", listen != "")
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
			TitleBar: mac.TitleBarDefault(),
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

// --- advertise address (docs/DESKTOP_DESIGN.md §5.2) ---

// resolveListenPort replaces a :0 port with a concrete one by probing a
// listener, so the advertised share URL is computable before server start.
// The probe closes immediately; the brief race window is acceptable for a
// loopback/LAN convenience flag.
func resolveListenPort(addr string) (string, error) {
	_, port, err := net.SplitHostPort(addr)
	if err != nil {
		return "", fmt.Errorf("parse --listen %q: %w", addr, err)
	}
	if port != "0" {
		return addr, nil
	}
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return "", fmt.Errorf("probe --listen %q: %w", addr, err)
	}
	defer ln.Close()
	return ln.Addr().String(), nil
}

// derivePublicBase resolves the externally reachable base URL for share
// links. Explicit --advertise wins; otherwise it is derived from the bound
// address: loopback → the loopback URL (localhost-only sharing), unspecified
// (0.0.0.0/::) → the outbound interface address, a specific address → used
// as-is. Returns "" when no usable address exists.
func derivePublicBase(advertise, boundAddr string) (string, error) {
	if advertise != "" {
		return advertise, nil
	}
	host, port, err := net.SplitHostPort(boundAddr)
	if err != nil {
		return "", fmt.Errorf("parse bound address %q: %w", boundAddr, err)
	}
	if ip := net.ParseIP(host); ip != nil {
		switch {
		case ip.IsLoopback():
			return "http://127.0.0.1:" + port, nil
		case ip.IsUnspecified():
			out := outboundIP()
			if out == "" {
				return "", errors.New("cannot determine the LAN address of this machine; pass --advertise explicitly")
			}
			return "http://" + out + ":" + port, nil
		default:
			return "http://" + host + ":" + port, nil
		}
	}
	if host == "localhost" {
		return "http://127.0.0.1:" + port, nil
	}
	return "http://" + host + ":" + port, nil
}

// outboundIP finds the preferred outbound IPv4 without sending traffic: a
// UDP "dial" to a documentation prefix only performs routing-table lookup.
func outboundIP() string {
	conn, err := net.Dial("udp4", "192.0.2.1:80")
	if err != nil {
		return ""
	}
	defer conn.Close()
	addr, ok := conn.LocalAddr().(*net.UDPAddr)
	if !ok {
		return ""
	}
	return addr.IP.String()
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

func configPath() (string, error) {
	if p := strings.TrimSpace(os.Getenv(configPathEnv)); p != "" {
		return filepath.Abs(p)
	}
	base, err := config.DefaultBaseDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(base, config.FileName), nil
}

func loadConfig() (*config.ResolvedConfig, error) {
	path, err := configPath()
	if err != nil {
		return nil, err
	}
	return config.Load(path, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
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
	if store, ok := proc.Store.(domain.WorkspaceStore); ok {
		if n, err := store.BackfillSessionWorkspaces(ctx, defaultWs.WorkspaceID); err != nil {
			logger.Warn("session workspace backfill failed", "error", err)
		} else if n > 0 {
			logger.Info("assigned legacy sessions to default workspace", "count", n, "workspace_id", defaultWs.WorkspaceID.String())
		}
	}
	return proc, registry, defaultWs, nil
}
