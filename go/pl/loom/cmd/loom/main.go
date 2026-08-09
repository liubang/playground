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

package main

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/server"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/ui"
	"github.com/liubang/playground/go/pl/loom/internal/version"
)

const (
	artifactDirectoryName = "artifacts"
	artifactGCGracePeriod = 24 * time.Hour
	// configPathEnv points at an alternative config file. It is a config
	// *locator* (like kubectl's KUBECONFIG), not configuration itself.
	configPathEnv = "LOOM_CONFIG"
)

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
		return collectArtifactGarbage(ctx)
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

// configPath returns the active config file path: LOOM_CONFIG when set,
// otherwise ~/.loom/config.yaml.
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

// loadConfig is the single configuration entry point for every command.
// requireProviders distinguishes agent entries (chat/run/resume: at least
// one provider is mandatory) from offline commands (sessions/inspect/gc/
// rules: they only need storage/rules and work without providers).
func loadConfig(requireProviders bool, logger *slog.Logger) (*config.ResolvedConfig, error) {
	path, err := configPath()
	if err != nil {
		return nil, err
	}
	resolved, err := config.Load(path, config.LoadOptions{RequireProviders: requireProviders, Logger: logger}, os.LookupEnv)
	if err != nil {
		return nil, err
	}
	return resolved, nil
}

// newFileLogger builds loom's unified file logger: glog-style records in
// <base_dir>/logs/loom.YYYY-MM-DD.log, rotated at local midnight. Both
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
	path, err := configPath()
	if err != nil {
		return err
	}
	if err := config.WriteTemplate(path); err != nil {
		return err
	}
	fmt.Printf("created %s\nedit it to configure at least one provider, then run loom again\n", path)
	return nil
}

// modelCatalog flattens the resolved providers into the picker's static
// option list, preserving the config file's declaration order.
func modelCatalog(resolved *config.ResolvedConfig) []ui.ModelOption {
	var models []ui.ModelOption
	for i := range resolved.Providers {
		p := &resolved.Providers[i]
		for _, mo := range p.Models {
			models = append(models, ui.ModelOption{
				Provider:      p.Name,
				Name:          mo.Name,
				ContextWindow: mo.ContextWindow,
				WireAPI:       mo.WireAPI,
			})
		}
	}
	return models
}

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
// and the default workspace (the startup root), then backfills the pre-v5
// session tail onto the default workspace (docs/WORKSPACE_DESIGN.md §7.2).
// The three entry points (chat/run/serve) share it so they assemble
// identically. The returned *Bootstrap is the default workspace's runtime;
// callers that need workspace resolution use the registry.
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
	// Backfill the pre-v5 session tail (workspace_id='') onto the default
	// workspace. Idempotent; safe to run on every boot.
	if store, ok := proc.Store.(domain.WorkspaceStore); ok {
		if n, err := store.BackfillSessionWorkspaces(ctx, defaultWs.WorkspaceID); err != nil {
			logger.Warn("session workspace backfill failed", "error", err)
		} else if n > 0 {
			logger.Info("assigned legacy sessions to default workspace", "count", n, "workspace_id", defaultWs.WorkspaceID.String())
		}
	}
	return proc, registry, defaultWs, nil
}

// --- agent entries ---

// runChat starts the interactive TUI chat session.
// If resumeSessionID is non-nil, the session is resumed; otherwise a new session is created.
func runChat(ctx context.Context, workspaceRoot string, resumeSessionID *domain.SessionID) error {
	root, err := resolveWorkspace(workspaceRoot)
	if err != nil {
		return err
	}
	// Config warnings go to stderr: the TUI has not taken over the screen
	// yet, so a stale-env or key-permission warning is still visible.
	resolved, err := loadConfig(true, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	// 统一文件日志（<base_dir>/logs/loom.YYYY-MM-DD.log，glog 风格）；TUI 占屏，
	// 打不开日志目录时静默降级为丢弃，绝不影响交互。
	logger := newFileLogger(resolved, slog.New(slog.NewTextHandler(io.Discard, nil)))
	proc, registry, bootstrap, err := assembleRuntime(ctx, resolved, root, logger)
	if err != nil {
		return err
	}
	defer proc.Close()
	defer registry.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	// Bridge delegate_task child-run lifecycle onto the event stream so the
	// TUI can show live sub-agent progress and the read-only drill-in view.
	app.WireSubagentObserver(bootstrap.SubagentFactory, broker, bootstrap.Store, logger)
	// The TUI is a peer client of the runtime (docs/SERVE_DESIGN.md §10):
	// same SessionService + in-proc client assembly that `loom serve` uses,
	// so every frontend shares one behavior.
	service := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{Logger: logger})
	sessionClient := client.NewInProc(service)

	if resumeSessionID != nil {
		if err := sessionClient.ResumeSession(ctx, *resumeSessionID); err != nil {
			return fmt.Errorf("resume session: %w", err)
		}
	} else if err := sessionClient.NewSession(ctx); err != nil {
		return fmt.Errorf("new session: %w", err)
	}

	// Dumb terminals usually lack a Nerd Font patched font, so they fall
	// back to plain text icons unless ui.icons says otherwise.
	icons := resolved.UI.Icons
	if icons == "" && os.Getenv("TERM") == "dumb" {
		icons = "plain"
	}
	meta, _ := resolved.ModelMeta(bootstrap.CurrentModel())
	contextCfg := resolved.Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	// The status bar measures context occupancy against the effective
	// window so its warning aligns with the compaction trigger.
	effectiveWindow := agent.NewWindowModel(meta.ContextWindow, resolved.Limits.MaxInputTokens, contextCfg).Effective
	opts := ui.InitOptions{
		NoColor:       os.Getenv("NO_COLOR") != "" || os.Getenv("TERM") == "dumb",
		AltScreen:     resolved.UI.AltScreen,
		Icons:         icons,
		Limits:        resolved.Limits,
		ContextWindow: int(effectiveWindow),
		Models:        modelCatalog(resolved),
		Keymap:        resolved.UI.Keymap,
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = service.Shutdown(shutdownCtx)
		broker.Close()
	}()
	return ui.StartTUI(sessionClient, bootstrap.CurrentModel().String(), root, opts)
}

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
	cfgPath, err := configPath()
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
	service := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{Logger: logger})
	srv, err := server.New(server.Config{
		Listen:      listen,
		Token:       token,
		AllowOrigin: allowOrigin,
		NoWeb:       noWeb,
		Version:     version.Version,
		Service:     service,
		Logger:      logger,
		ConfigPath:  cfgPath,
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

// runAgent executes a single prompt headlessly (loom run / loom resume).
// It shares the TUI's Bootstrap assembly so both paths behave identically;
// only the approver (console vs. channel) and the absence of a broker differ.
func runAgent(ctx context.Context, userPrompt string, resumeSessionID *domain.SessionID) error {
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
	proc, registry, bootstrap, err := assembleRuntime(ctx, resolved, root, slog.Default())
	if err != nil {
		return err
	}
	defer proc.Close()
	defer registry.Close()

	sqliteStore, ok := bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return fmt.Errorf("headless runs require the SQLite session store")
	}

	var run *agent.Run
	if resumeSessionID == nil {
		run = agent.NewRun(domain.NewSessionID(), resolved.Limits, domain.RealClock{})
		if err := sqliteStore.CreateSession(ctx, run.SessionID, bootstrap.WorkspaceID); err != nil {
			return fmt.Errorf("create session: %w", err)
		}
	} else {
		inspection, err := sqliteStore.InspectSession(ctx, *resumeSessionID)
		if err != nil {
			return fmt.Errorf("load session for resume: %w", err)
		}
		run, err = agent.RecoverRun(inspection.Session.ID, inspection.Checkpoint,
			inspection.Transcript.Messages, inspection.Events, inspection.Session.Version,
			resolved.Limits, domain.RealClock{}, bootstrap.Validator)
		if err != nil {
			return fmt.Errorf("resume session: %w", err)
		}
		// A resumed session continues with a new prompt, so it gets a fresh
		// per-prompt budget window (same semantics as agent.ContinueRun).
		run.ResetUsageForNewTurn()
	}
	bootstrap.SessionEnv.Store(process.LoomSessionEnv(version.Version, run.SessionID.String()))
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: userPrompt}},
		CreatedAt: time.Now().UTC(),
	})

	current := resolved.Default
	provider := resolved.ProviderByName(current.Provider)
	if provider == nil {
		// Mirror the controller's runTurn guard: fail loudly instead of
		// panicking on provider.ModelFor (REVIEW M36).
		return fmt.Errorf("provider %q is not configured", current.Provider)
	}
	meta, _ := resolved.ModelMeta(current)
	contextCfg := resolved.Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	// Publish the model selection for delegate_task's child loops — the
	// same mailbox the controller uses on the interactive path.
	app.PublishSubagentSnapshot(bootstrap.SubagentModels, resolved, current, meta.Reasoning.DomainSpec(), run.SessionID)
	loop := agent.Loop{
		Run: run, Model: provider.ModelFor(current.Model), ModelName: current.Model, Store: bootstrap.Store,
		Approver: &consoleApprover{}, Policy: bootstrap.CurrentPolicy(), Registry: bootstrap.Registry,
		Logger: slog.Default(), SystemPrompt: bootstrap.CurrentPrompt(), Artifacts: bootstrap.Artifact,
		Recorder: bootstrap.Recorder, Prompt: userPrompt, Workspace: root,
		Window:  agent.NewWindowModel(meta.ContextWindow, resolved.Limits.MaxInputTokens, contextCfg),
		Runaway: resolved.Runaway, Reasoning: meta.Reasoning.DomainSpec(),
		GoalCell: bootstrap.GoalCell, PlanCell: bootstrap.PlanCell,
		CostInputUSDPerMTok: resolved.Tracing.CostInputPerMTok, CostOutputUSDPerMTok: resolved.Tracing.CostOutputPerMTok,
	}
	fmt.Fprintf(os.Stderr, "loom: session %s\n", run.SessionID)
	executeErr := loop.Execute(ctx)
	var checkpointErr error
	if run.State.Lifecycle == domain.LifecycleTerminal {
		checkpointErr = saveTerminalCheckpoint(ctx, bootstrap.Store, run)
	}
	if executeErr != nil || checkpointErr != nil {
		return errors.Join(executeErr, checkpointErr)
	}
	for i := len(run.Messages) - 1; i >= 0; i-- {
		if run.Messages[i].Role == domain.RoleAssistant {
			for _, text := range run.Messages[i].TextParts() {
				fmt.Print(text)
			}
			fmt.Println()
			return nil
		}
	}
	return errors.New("model produced no final answer")
}

func saveTerminalCheckpoint(ctx context.Context, store domain.SessionStore, run *agent.Run) error {
	persistCtx := ctx
	cancel := func() {}
	if ctx == nil {
		persistCtx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	} else if ctx.Err() != nil {
		persistCtx, cancel = context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	}
	defer cancel()
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: run.SessionID, Sequence: run.Version,
		State: run.State, Messages: append([]domain.Message(nil), run.Messages...),
		Plan: run.Plan, Usage: run.Usage, CreatedAt: time.Now().UTC(),
	}
	if err := store.SaveCheckpoint(persistCtx, checkpoint); err != nil {
		return fmt.Errorf("save terminal checkpoint: %w", err)
	}
	return nil
}

// --- offline commands ---

func collectArtifactGarbage(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		return fmt.Errorf("list artifact references: %w", err)
	}
	artifactStore, err := artifact.Open(
		filepath.Join(resolved.Storage.SessionsDir(), artifactDirectoryName),
		resolved.Limits.MaxArtifactBytes,
	)
	if err != nil {
		return fmt.Errorf("open artifact store: %w", err)
	}
	report, err := artifactStore.CollectGarbage(ctx, refs, artifactGCGracePeriod, time.Now())
	if err != nil {
		return fmt.Errorf("collect artifact garbage: %w", err)
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetEscapeHTML(false)
	return encoder.Encode(report)
}

func listSessions(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	summaries, _, err := store.ListSessions(ctx, "", 100, false, domain.WorkspaceID{})
	if err != nil {
		return err
	}
	for _, summary := range summaries {
		fmt.Printf("%s\t%d\t%s\n", summary.ID, summary.Version, summary.UpdatedAt.UTC().Format(time.RFC3339Nano))
	}
	return nil
}

// listWorkspaces prints every registered workspace (loom workspace list).
// Read-only path: works without a running serve process.
func listWorkspaces(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	workspaces, err := store.ListWorkspaces(ctx)
	if err != nil {
		return err
	}
	for _, ws := range workspaces {
		fmt.Printf("%s\t%s\t%s\n", ws.ID, ws.Name, ws.RootPath)
	}
	return nil
}

// addWorkspace registers a workspace entity (loom workspace add <path>).
// It writes through the local store under the data-dir flock — mutually
// exclusive with a running serve, so a live serve means "use the Web UI or
// POST /v1/workspaces instead". The workspace's runtime is assembled lazily
// on the next Resolve (chat/serve startup), not in this short-lived process.
func addWorkspace(ctx context.Context, root, name string) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	// Canonical validation mirrors the registry's (docs/WORKSPACE_DESIGN.md W2).
	abs, err := filepath.Abs(strings.TrimSpace(root))
	if err != nil {
		return fmt.Errorf("workspace root: %w", err)
	}
	canonical, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return fmt.Errorf("workspace root: %w", err)
	}
	if info, err := os.Stat(canonical); err != nil || !info.IsDir() {
		return fmt.Errorf("workspace root %q is not an existing directory", canonical)
	}
	lock, err := server.AcquireDataDirLock(resolved.Storage.SessionsDir())
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			return errors.New("a loom serve process owns the data directory; add the workspace via the Web UI or POST /v1/workspaces instead")
		}
		return err
	}
	defer lock.Release()
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	if name == "" {
		name = filepath.Base(canonical)
	}
	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID:       domain.NewWorkspaceID(),
		Name:     name,
		RootPath: canonical,
	})
	if err != nil {
		return fmt.Errorf("register workspace: %w", err)
	}
	fmt.Printf("%s\t%s\t%s\n", ws.ID, ws.Name, ws.RootPath)
	return nil
}

// removeWorkspace deletes a workspace entity (loom workspace rm <id>). Only
// the metadata row is removed — the on-disk root directory is never touched
// and the workspace's sessions survive as read-only history
// (docs/WORKSPACE_DESIGN.md §7.1). Like `add`, it writes through the local
// store under the data-dir flock: a running serve owns the directory, so
// deletion then goes through the Web UI or DELETE /v1/workspaces/{id}
// (which additionally guards the default workspace and live sessions).
func removeWorkspace(ctx context.Context, rawID string) error {
	id, err := domain.ParseWorkspaceID(rawID)
	if err != nil || !domain.HasPrefix(id, "ws_") {
		return fmt.Errorf("invalid workspace id %q", rawID)
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	lock, err := server.AcquireDataDirLock(resolved.Storage.SessionsDir())
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			return errors.New("a loom serve process owns the data directory; delete the workspace via the Web UI or DELETE /v1/workspaces/{id} instead")
		}
		return err
	}
	defer lock.Release()
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	ws, err := store.GetWorkspace(ctx, id)
	if err != nil {
		return fmt.Errorf("workspace not found: %s", id)
	}
	if err := store.DeleteWorkspace(ctx, id); err != nil {
		return fmt.Errorf("delete workspace: %w", err)
	}
	fmt.Printf("deleted\t%s\t%s\t%s\n", ws.ID, ws.Name, ws.RootPath)
	return nil
}

func inspectSession(ctx context.Context, rawSessionID string) error {
	sessionID, err := parseSessionID(rawSessionID)
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return errors.New("session store does not exist")
		}
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		return fmt.Errorf("inspect session: %w", err)
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(inspection); err != nil {
		return fmt.Errorf("encode session inspection: %w", err)
	}
	return nil
}

// listRules prints every effective rule with its layer, so users can audit
// what the policy engine will do without running a command.
func listRules() error {
	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	policy := permission.AttachRules(context.Background(), permission.DefaultPolicy(), root, resolved.Storage.RulesDir(), resolved.Rules.LoadOptions(), slog.Default())
	rules := policy.Rules.Rules()
	domains := policy.Rules.Domains()
	tools := policy.Rules.Tools()
	if len(rules) == 0 && len(domains) == 0 && len(tools) == 0 {
		fmt.Println("no rules in effect (rules.enabled/rules.builtin may be disabled)")
		return nil
	}
	for _, r := range rules {
		just := ""
		if r.Justification != "" {
			just = " — " + r.Justification
		}
		grant := ""
		if g := r.Grant.ExecGrant(); !g.IsZero() {
			grant = " (" + g.Summary() + ")"
		}
		fmt.Printf("[%s] %-40s %s%s\n", r.Decision, strings.Join(r.ArgvPrefix, " ")+grant, r.Source, just)
	}
	for _, d := range domains {
		just := ""
		if d.Justification != "" {
			just = " — " + d.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", d.Decision, "host:"+d.Host, d.Source, just)
	}
	for _, t := range tools {
		just := ""
		if t.Justification != "" {
			just = " — " + t.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", t.Decision, "tool:"+t.Name, t.Source, just)
	}
	return nil
}

// checkRules is the dry-run inspector for the declarative rule engine: it
// evaluates an argv exactly like the run_cmd policy path and prints the
// verdict with the matching rule (if any), mirroring `codex execpolicy
// check`. Usage:
//
//	loom rules check [--escalated] [--needs-network] <program> [args...]
//	loom rules check --url https://example.com/x   (web_fetch evaluation)
func checkRules(argv []string) error {
	var (
		escalated    bool
		needsNetwork bool
		fetchURL     string
		args         []string
	)
	for i := 0; i < len(argv); i++ {
		switch argv[i] {
		case "--escalated":
			escalated = true
		case "--needs-network":
			needsNetwork = true
		case "--url":
			if i+1 >= len(argv) {
				return errors.New("--url requires a value")
			}
			i++
			fetchURL = argv[i]
		default:
			args = append(args, argv[i])
		}
	}
	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	policy := permission.AttachRules(context.Background(), permission.DefaultPolicy(), root, resolved.Storage.RulesDir(), resolved.Rules.LoadOptions(), slog.Default())
	if fetchURL != "" {
		return checkFetchURL(policy, resolved.Approval.Mode, fetchURL)
	}
	if len(args) == 0 {
		return errors.New("usage: loom rules check [--escalated] [--needs-network] [--url URL] <program> [args...]")
	}
	argv = args
	callArgs := map[string]any{"program": argv[0], "args": argv[1:]}
	risk := domain.R2
	if escalated {
		callArgs["sandbox_permissions"] = "require_escalated"
		callArgs["justification"] = "dry run"
		risk = domain.R3
	}
	if needsNetwork {
		callArgs["needs_network"] = true
	}
	argsJSON, _ := json.Marshal(callArgs)
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: argsJSON},
		Risk: risk,
	}
	decider := policy.Decider(resolved.Approval.Mode)
	verdict := decider.Evaluate(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if !verdict.Grant.IsZero() {
		fmt.Printf("grant: %s\n", verdict.Grant.Summary())
	}
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	if process.IsShellProgram(argv[0]) {
		fmt.Println("note: shell scripts are evaluated per subcommand via AST analysis (pipes/redirects included)")
	}
	if ruleArgv, ok := permission.RunCmdArgv(argsJSON); ok {
		rule := permission.MatchRule(policy.Rules, ruleArgv)
		via := ""
		if rule.Source == "" {
			if norm, ok := permission.NormalizeTrustedPath(ruleArgv); ok {
				rule = permission.MatchRule(policy.Rules, norm)
				via = " (via trusted basename " + norm[0] + ")"
			}
		}
		if rule.Source != "" {
			source := rule.Source
			if source == "builtin" {
				source = "builtin (embedded read-only set)"
			}
			fmt.Printf("matched rule: %v -> %s (%s)%s\n", rule.ArgvPrefix, rule.Decision, source, via)
			if rule.Justification != "" {
				fmt.Printf("justification: %s\n", rule.Justification)
			}
		}
	}
	return nil
}

// checkFetchURL evaluates a web_fetch call against the domain rules and
// prints the verdict — the web_fetch counterpart of checkRules.
func checkFetchURL(policy permission.Policy, mode permission.ApprovalMode, rawURL string) error {
	argsJSON, _ := json.Marshal(map[string]string{"url": rawURL})
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_fetch", Arguments: argsJSON},
		Risk: domain.R3,
	}
	verdict := policy.Decider(mode).Evaluate(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	if host, ok := permission.ParseWebFetchHost(argsJSON); ok {
		if _, rule := policy.Rules.EvaluateDomain(host); rule.Source != "" {
			fmt.Printf("matched rule: host:%s -> %s (%s)\n", rule.Host, rule.Decision, rule.Source)
			if rule.Justification != "" {
				fmt.Printf("justification: %s\n", rule.Justification)
			}
		}
	}
	return nil
}

// forgetRules removes a remembered approval from the SQLite store.
// Usage: loom rules forget [--domain host | --tool name] <program> [args...]
func forgetRules(argv []string) error {
	if len(argv) == 0 {
		return errors.New("usage: loom rules forget [--domain host | --tool name] <program> [args...]")
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	store, err := permission.OpenRememberedStore(context.Background(), permission.RememberedDBPath(resolved.Storage.RulesDir()))
	if err != nil {
		return fmt.Errorf("open remembered store: %w", err)
	}
	defer store.Close()
	if argv[0] == "--domain" || argv[0] == "--tool" {
		if len(argv) < 2 || strings.HasPrefix(argv[1], "--") {
			return fmt.Errorf("%s requires a value\nusage: loom rules forget [--domain host | --tool name] <program> [args...]", argv[0])
		}
		if argv[0] == "--domain" {
			host := argv[1]
			ok, err := store.ForgetDomain(context.Background(), host)
			if err != nil {
				return err
			}
			if !ok {
				fmt.Printf("no remembered domain for %q\n", host)
			} else {
				fmt.Printf("forgot domain %q\n", host)
			}
			return nil
		}
		name := argv[1]
		ok, err := store.ForgetTool(context.Background(), name)
		if err != nil {
			return err
		}
		if !ok {
			fmt.Printf("no remembered tool rule for %q\n", name)
		} else {
			fmt.Printf("forgot tool rule %q\n", name)
		}
		return nil
	}
	// argv prefix rule
	ok, err := store.ForgetRule(context.Background(), argv)
	if err != nil {
		return err
	}
	if !ok {
		fmt.Printf("no remembered rule for %v\n", argv)
	} else {
		fmt.Printf("forgot rule %v\n", argv)
	}
	return nil
}

// importRules imports a declarative rule file (the user-layer JSON schema)
// into the remembered store. Existing store entries win; the file itself is
// left untouched (rename it aside manually to complete a migration).
// Usage: loom rules import <file.json>
func importRules(path string) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	ctx := context.Background()
	store, err := permission.OpenRememberedStore(ctx, permission.RememberedDBPath(resolved.Storage.RulesDir()))
	if err != nil {
		return fmt.Errorf("open remembered store: %w", err)
	}
	defer store.Close()
	if err := store.ImportRuleFile(ctx, path); err != nil {
		return fmt.Errorf("import %s: %w", path, err)
	}
	fmt.Printf("imported allow rules from %s into %s (existing entries kept)\n", path, store.Path())
	return nil
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

// consoleApprover prompts on the terminal. A single background reader feeds
// a line channel for the process lifetime: recreating bufio.Reader per
// approval used to drop bytes it had already buffered, and each cancelled
// approval leaked a goroutine that then raced the next one on stdin.
type consoleApprover struct {
	once  sync.Once
	lines chan string
}

// start launches the one stdin reader goroutine. It exits on stdin EOF.
func (a *consoleApprover) start(r io.Reader) {
	a.once.Do(func() {
		a.lines = make(chan string)
		go func() {
			defer close(a.lines)
			reader := bufio.NewReader(r)
			for {
				line, err := reader.ReadString('\n')
				if err != nil {
					return
				}
				a.lines <- strings.TrimSpace(strings.ToLower(line))
			}
		}()
	})
}

// awaitAnswer waits for the next input line or ctx cancellation.
func (a *consoleApprover) awaitAnswer(ctx context.Context) (domain.Decision, error) {
	select {
	case <-ctx.Done():
		return domain.DecisionDeny, ctx.Err()
	case value, ok := <-a.lines:
		if !ok {
			return domain.DecisionDeny, nil
		}
		if value == "y" || value == "yes" {
			return domain.DecisionAllow, nil
		}
		return domain.DecisionDeny, nil
	}
}

func (a *consoleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	info, err := os.Stdin.Stat()
	if err != nil {
		return domain.DecisionDeny, fmt.Errorf("inspect stdin: %w", err)
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		// Unattended (piped stdin): the request can never be answered, so
		// it is denied — but the desktop notification still goes out: the
		// whole point of a headless long run is that the user is elsewhere.
		app.NotifyApproval(req.Call.Call.Name, req.Description+" (无人值守，已自动拒绝)")
		return domain.DecisionDeny, nil
	}
	a.start(os.Stdin)
	app.NotifyApproval(req.Call.Call.Name, req.Description)
	fmt.Fprintf(os.Stderr, "\nApproval required (R%d): %s\nargs hash: %s\nAllow? [y/N] ",
		req.Call.Risk, req.Description, req.Call.ArgsHash)
	return a.awaitAnswer(ctx)
}
