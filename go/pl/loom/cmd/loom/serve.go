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
// Created: 2026/08/16

package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/server"
	"github.com/liubang/playground/go/pl/loom/internal/version"
)

// runServe starts the headless server mode (loom serve): a single-instance
// daemon exposing the REST+SSE protocol (docs/SERVE_DESIGN.md §5).
func runServe(ctx context.Context, args []string) error {
	var listen, token, allowOrigin string
	var noWeb bool
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "--listen" && i+1 < len(args):
			i++
			listen = args[i]
		case args[i] == "--token" && i+1 < len(args):
			i++
			token = args[i]
		case args[i] == "--allow-origin" && i+1 < len(args):
			i++
			allowOrigin = args[i]
		case args[i] == "--no-web":
			noWeb = true
		default:
			return fmt.Errorf("usage: loom serve [--listen <addr|unix:path>] [--token <token>] [--allow-origin <origin>] [--no-web]")
		}
	}
	if listen == "" {
		listen = "127.0.0.1:7680"
	}

	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(true, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	dataDir := resolved.Storage.SessionsDir()

	// Single-instance discipline (docs/SERVE_DESIGN.md §3.2): the data
	// directory flock must be taken BEFORE anything touches the store.
	lock, err := server.AcquireDataDirLock(dataDir)
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			return fmt.Errorf("another loom process already owns %s (stop it or use a different data dir)", dataDir)
		}
		return err
	}
	defer lock.Release()

	// Resolve the bearer token: explicit flag, else the persisted token
	// file, else generate one (printed ONCE to stderr, never logged).
	tokenFile := filepath.Join(dataDir, "serve.token")
	generated := false
	if token == "" {
		if raw, err := os.ReadFile(tokenFile); err == nil {
			token = strings.TrimSpace(string(raw))
		} else if errors.Is(err, os.ErrNotExist) {
			token, err = generateServeToken(tokenFile)
			if err != nil {
				return err
			}
			generated = true
		} else {
			return fmt.Errorf("read serve token: %w", err)
		}
	}

	logger := newFileLogger(resolved, slog.New(logging.NewGlogHandler(os.Stderr, nil)))
	proc, registry, bootstrap, err := assembleRuntime(ctx, resolved, root, logger)
	if err != nil {
		return err
	}
	defer proc.Close()
	defer registry.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	app.WireSubagentObserver(bootstrap.SubagentFactory, broker, bootstrap.Store, logger)
	service := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{
		Logger:   logger,
		RulesDir: resolved.Storage.RulesDir(),
		// The WebUI surfaces approval requests in-page; a desktop banner on
		// top of that is noise for the user staring at the browser.
		DisableApprovalNotify: true,
	})
	srv, err := server.New(server.Config{
		Listen:      listen,
		Token:       token,
		AllowOrigin: allowOrigin,
		NoWeb:       noWeb,
		Version:     version.Version,
		Service:     service,
		Logger:      logger,
		ConfigPath:  config.ConfigPathForHome(resolved.Storage.BaseDir),
	})
	if err != nil {
		return err
	}
	if err := srv.Listen(); err != nil {
		return err
	}

	if generated {
		// One-time convenience hint (never repeated, never in logs).
		scheme := "http"
		fmt.Fprintf(os.Stderr, "loom: serve token written to %s\n", tokenFile)
		fmt.Fprintf(os.Stderr, "loom: connect with: curl -H 'Authorization: Bearer %s' %s://%s/v1/meta/version\n", token, scheme, srv.Addr())
	}
	logger.Info("loom serve ready", "addr", srv.Addr(), "instance", srv.Instance())

	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve() }()

	select {
	case <-ctx.Done():
	case err := <-serveErr:
		return err
	}
	// Graceful stop (docs/SERVE_DESIGN.md §7.3): stop accepting HTTP, then
	// drain the session service (turns finish or get cancelled at the
	// deadline), then close the broker and store.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		logger.Warn("http shutdown", "error", err)
	}
	if err := service.Shutdown(shutdownCtx); err != nil {
		logger.Warn("service shutdown", "error", err)
	}
	broker.Close()
	return nil
}

// generateServeToken creates a random bearer token and persists it
// owner-only (docs/SERVE_DESIGN.md §5.2).
func generateServeToken(path string) (string, error) {
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate token: %w", err)
	}
	token := hex.EncodeToString(raw)
	if err := os.WriteFile(path, []byte(token+"\n"), 0o600); err != nil {
		return "", fmt.Errorf("write serve token: %w", err)
	}
	return token, nil
}
