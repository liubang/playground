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
// Created: 2026/07/22 21:10

// Command loom is the single binary for every entry point: the interactive
// TUI (chat), headless single-prompt runs (run/resume), the REST+SSE
// daemon (serve), and the offline maintenance commands (sessions/inspect/
// gc/rules/workspace/config). The entry points live in their own files:
// chat.go, agent.go, serve.go, offline.go, rules.go, approver.go.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"github.com/liubang/playground/go/pl/loom/internal/version"
)

// artifactDirectoryName is the sessions subdirectory holding the
// content-addressed artifact store.
const artifactDirectoryName = "artifacts"

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "loom:", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string) error {
	if len(args) == 0 {
		// No args: if TTY, enter interactive chat; otherwise show usage.
		if isTTY(os.Stdout) && isTTY(os.Stdin) {
			return runChat(ctx, "", nil)
		}
		return errors.New("usage: loom <run|resume|chat|serve|sessions|inspect|gc|rules|config|version> [args]")
	}
	switch args[0] {
	case "version":
		fmt.Println("loom", version.Version)
		return nil
	case "chat":
		if len(args) == 1 {
			return runChat(ctx, "", nil)
		}
		if len(args) == 3 && args[1] == "--resume" {
			sessionID, err := parseSessionID(args[2])
			if err != nil {
				return err
			}
			return runChat(ctx, "", &sessionID)
		}
		return errors.New("usage: loom chat [--resume <session-id>]")
	case "run":
		if len(args) < 2 || strings.TrimSpace(strings.Join(args[1:], " ")) == "" {
			return errors.New("usage: loom run <prompt>")
		}
		return runAgent(ctx, strings.Join(args[1:], " "), nil)
	case "resume":
		if len(args) < 3 || strings.TrimSpace(strings.Join(args[2:], " ")) == "" {
			return errors.New("usage: loom resume <session-id> <prompt>")
		}
		sessionID, err := parseSessionID(args[1])
		if err != nil {
			return err
		}
		return runAgent(ctx, strings.Join(args[2:], " "), &sessionID)
	case "sessions":
		if len(args) != 1 {
			return errors.New("usage: loom sessions")
		}
		return listSessions(ctx)
	case "workspace":
		if len(args) == 2 && args[1] == "list" {
			return listWorkspaces(ctx)
		}
		if len(args) >= 3 && args[1] == "add" {
			name := ""
			for i := 3; i+1 < len(args); i++ {
				if args[i] == "--name" {
					name = args[i+1]
				}
			}
			return addWorkspace(ctx, args[2], name)
		}
		if len(args) == 3 && args[1] == "rm" {
			return removeWorkspace(ctx, args[2])
		}
		return errors.New("usage: loom workspace <list|add <path> [--name N]|rm <id>>")
	case "inspect":
		if len(args) != 2 {
			return errors.New("usage: loom inspect <session-id>")
		}
		return inspectSession(ctx, args[1])
	case "gc":
		if len(args) != 1 {
			return errors.New("usage: loom gc")
		}
		return collectGarbage(ctx)
	case "rules":
		if len(args) == 2 && args[1] == "list" {
			return listRules()
		}
		if len(args) >= 2 && args[1] == "check" {
			return checkRules(args[2:])
		}
		if len(args) >= 2 && args[1] == "forget" {
			return forgetRules(args[2:])
		}
		if len(args) == 3 && args[1] == "import" {
			return importRules(args[2])
		}
		return errors.New("usage: loom rules <list|check <program> [args...]|forget [--domain host] <program> [args...]|import <file.json>>")
	case "serve":
		return runServe(ctx, args[1:])
	case "config":
		if len(args) == 2 && args[1] == "init" {
			return initConfig()
		}
		return errors.New("usage: loom config init")
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
}

// --- configuration ---

// loadConfig is the single configuration entry point for every command.
// requireProviders distinguishes agent entries (chat/run/resume: at least
// one provider is mandatory) from offline commands (sessions/inspect/gc/
// rules: they only need storage/rules and work without providers).
func loadConfig(requireProviders bool, logger *slog.Logger) (*config.ResolvedConfig, error) {
	home, err := config.HomeDir(os.LookupEnv)
	if err != nil {
		return nil, err
	}
	resolved, err := config.Load(home, config.LoadOptions{RequireProviders: requireProviders, Logger: logger}, os.LookupEnv)
	if err != nil {
		// First-run bootstrap: a missing config in the default loom home
		// is a fresh install, not an error. Write the starter template and
		// hand back a directed error (exit non-zero: the user still has to
		// set an LLM API key before the agent can run). A missing config
		// in an explicit LOOM_HOME stays the original hard error — the
		// user pointed loom at a home that should already be set up.
		if requireProviders && errors.Is(err, config.ErrConfigNotFound) {
			if created, cerr := config.EnsureFirstRunConfig(home); cerr != nil {
				return nil, cerr
			} else if created {
				return nil, fmt.Errorf("first run: created starter config at %s\nset your LLM API key (api_key or api_key_env) in it, then run loom again", config.ConfigPathForHome(home))
			}
		}
		return nil, err
	}
	return resolved, nil
}

// newFileLogger builds loom's unified file logger: glog-style records in
// <loom home>/logs/loom.YYYY-MM-DD.log, rotated at local midnight. Both
// the TUI and serve modes share it (the TUI previously discarded all
// logs). fallback applies when the log directory cannot be opened — the
// TUI passes a discard logger, serve passes a stderr glog handler.
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

// prepareStorage creates loom's private data directories (the base dir
// and its sessions subdirectory) when create is set — agent entries
// create them; offline read commands leave the filesystem untouched.
// Both directories are loom-owned, so they are tightened to 0700.
func prepareStorage(resolved *config.ResolvedConfig, create bool) error {
	if !create {
		return nil
	}
	if err := preparePrivateDataDirectory(resolved.Storage.BaseDir); err != nil {
		return err
	}
	return preparePrivateDataDirectory(resolved.Storage.SessionsDir())
}

// initConfig writes the annotated starter config (loom config init).
func initConfig() error {
	home, err := config.HomeDir(os.LookupEnv)
	if err != nil {
		return err
	}
	path := config.ConfigPathForHome(home)
	if err := config.WriteTemplate(path); err != nil {
		return err
	}
	fmt.Printf("created %s\nedit it to configure at least one provider, then run loom again\n", path)
	return nil
}

// --- runtime assembly ---

// resolveWorkspace picks the workspace root: the explicit value, then the
// Bazel runfiles hint, then the current directory.
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

// assembleRuntime wires the shared ProcessRuntime, the workspace registry,
// and the default workspace (the startup root). The three entry points
// (chat/run/serve) share it so they assemble identically. The returned
// *Bootstrap is the default workspace's runtime; callers that need
// workspace resolution use the registry.
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
	// Pre-register configured workspaces (docs/WORKSPACE_DESIGN.md §10).
	// Best-effort: an unreachable root is logged and skipped, never fatal.
	for _, wc := range resolved.Workspaces {
		if _, err := registry.Register(ctx, wc.Root, wc.Name); err != nil {
			logger.Warn("workspace pre-register skipped", "root", wc.Root, "error", err)
		}
	}
	return proc, registry, defaultWs, nil
}

// --- helpers ---

func parseSessionID(rawSessionID string) (domain.SessionID, error) {
	rawSessionID = strings.TrimSpace(rawSessionID)
	sessionID, err := domain.ParseSessionID(rawSessionID)
	if err != nil || !domain.HasPrefix(sessionID, "sess_") || len(rawSessionID) != len("sess_")+32 {
		return domain.SessionID{}, errors.New("parse session ID: expected sess_ followed by 32 hexadecimal characters")
	}
	for _, ch := range rawSessionID[len("sess_"):] {
		if !strings.ContainsRune("0123456789abcdef", ch) {
			return domain.SessionID{}, errors.New("parse session ID: expected sess_ followed by 32 hexadecimal characters")
		}
	}
	return sessionID, nil
}

// preparePrivateDataDirectory creates one loom-owned data directory,
// rejecting symlinks and tightening permissions to 0700.
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

// isTTY checks whether the given file descriptor is a terminal.
func isTTY(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}
