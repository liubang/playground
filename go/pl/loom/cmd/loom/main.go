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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/ui"
)

const (
	version               = "0.2.0-dev"
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
		return errors.New("usage: loom <run|resume|chat|sessions|inspect|gc|rules|config|version> [args]")
	}
	switch args[0] {
	case "version":
		fmt.Println("loom", version)
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
		return errors.New("usage: loom rules <list|check <program> [args...]>")
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
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("locate home dir: %w", err)
	}
	return filepath.Join(home, ".loom", config.FileName), nil
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

// resolveSessionDB fills ResolvedConfig.Storage.SessionDB: the configured
// path (made absolute) when set, otherwise the platform default. create
// controls whether the private data directory is prepared on disk — agent
// entries create it; offline read commands leave the filesystem untouched.
func resolveSessionDB(resolved *config.ResolvedConfig, create bool) error {
	configured := strings.TrimSpace(resolved.Storage.SessionDB)
	if configured != "" {
		path, err := filepath.Abs(configured)
		if err != nil {
			return fmt.Errorf("resolve storage.session_db: %w", err)
		}
		if create {
			if err := preparePrivateDataDirectory(filepath.Dir(path), false); err != nil {
				return err
			}
		}
		resolved.Storage.SessionDB = path
		return nil
	}
	base, err := defaultStateDirectory()
	if err != nil {
		return err
	}
	directory := filepath.Join(base, "loom")
	if create {
		if err := preparePrivateDataDirectory(directory, true); err != nil {
			return err
		}
	}
	resolved.Storage.SessionDB = filepath.Join(directory, "sessions.db")
	return nil
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
	if err := resolveSessionDB(resolved, true); err != nil {
		return err
	}
	artifactDir := filepath.Join(filepath.Dir(resolved.Storage.SessionDB), artifactDirectoryName)

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	// The questioner is shared: the bootstrap injects it into the ask_user
	// tool, the controller bridges it to the TUI's question overlay.
	questioner := app.NewChannelQuestioner(nil)
	bootstrap, err := app.NewBootstrap(ctx, resolved, app.BootstrapConfig{
		WorkspaceRoot: root,
		ArtifactDir:   artifactDir,
		Version:       version,
		Logger:        discard,
		Questioner:    questioner,
	})
	if err != nil {
		return fmt.Errorf("bootstrap: %w", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker()
	controller := app.NewController(app.ControllerConfig{
		Bootstrap:  bootstrap,
		Broker:     broker,
		Approver:   app.NewChannelApprover(),
		Questioner: questioner,
		Logger:     discard,
	})

	// Start the controller before issuing its serialized commands.
	go controller.Run(ctx)

	if resumeSessionID != nil {
		if err := controller.ResumeSession(ctx, *resumeSessionID); err != nil {
			return fmt.Errorf("resume session: %w", err)
		}
	} else if err := controller.NewSession(ctx); err != nil {
		return fmt.Errorf("new session: %w", err)
	}

	// Dumb terminals usually lack a Nerd Font patched font, so they fall
	// back to plain text icons unless ui.icons says otherwise.
	icons := resolved.UI.Icons
	if icons == "" && os.Getenv("TERM") == "dumb" {
		icons = "plain"
	}
	meta, _ := resolved.ModelMeta(bootstrap.Current)
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
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = controller.Shutdown(shutdownCtx)
		broker.Close()
	}()
	return ui.StartTUI(controller, bootstrap.Current.String(), root, opts)
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
	if err := resolveSessionDB(resolved, true); err != nil {
		return err
	}
	artifactDir := filepath.Join(filepath.Dir(resolved.Storage.SessionDB), artifactDirectoryName)

	bootstrap, err := app.NewBootstrap(ctx, resolved, app.BootstrapConfig{
		WorkspaceRoot: root,
		ArtifactDir:   artifactDir,
		Version:       version,
		Logger:        slog.Default(),
	})
	if err != nil {
		return fmt.Errorf("bootstrap: %w", err)
	}
	defer bootstrap.Close()

	sqliteStore, ok := bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return fmt.Errorf("headless runs require the SQLite session store")
	}

	var run *agent.Run
	if resumeSessionID == nil {
		run = agent.NewRun(domain.NewSessionID(), resolved.Limits, domain.RealClock{})
		if err := sqliteStore.CreateSession(ctx, run.SessionID); err != nil {
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
	bootstrap.SessionEnv.Store(process.LoomSessionEnv(version, run.SessionID.String()))
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: userPrompt}},
		CreatedAt: time.Now().UTC(),
	})

	current := resolved.Default
	provider := resolved.ProviderByName(current.Provider)
	meta, _ := resolved.ModelMeta(current)
	contextCfg := resolved.Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	loop := agent.Loop{
		Run: run, Model: provider.ModelFor(current.Model), ModelName: current.Model, Store: bootstrap.Store,
		Approver: &consoleApprover{}, Policy: bootstrap.Policy, Registry: bootstrap.Registry,
		Logger: slog.Default(), SystemPrompt: bootstrap.PromptBuilder, Artifacts: bootstrap.Artifact,
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
	if err := resolveSessionDB(resolved, true); err != nil {
		return err
	}
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDB)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		return fmt.Errorf("list artifact references: %w", err)
	}
	artifactStore, err := artifact.Open(
		filepath.Join(filepath.Dir(resolved.Storage.SessionDB), artifactDirectoryName),
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
	if err := resolveSessionDB(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDB
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
	summaries, err := store.ListSessions(ctx, 100)
	if err != nil {
		return err
	}
	for _, summary := range summaries {
		fmt.Printf("%s\t%d\t%s\n", summary.ID, summary.Version, summary.UpdatedAt.UTC().Format(time.RFC3339Nano))
	}
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
	if err := resolveSessionDB(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDB
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
	policy := permission.AttachRules(permission.DefaultPolicy(), root, resolved.Rules.LoadOptions(), slog.Default())
	rules := policy.Rules.Rules()
	if len(rules) == 0 {
		fmt.Println("no rules in effect (rules.enabled/rules.builtin may be disabled)")
		return nil
	}
	for _, r := range rules {
		just := ""
		if r.Justification != "" {
			just = " — " + r.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", r.Decision, strings.Join(r.ArgvPrefix, " "), r.Source, just)
	}
	return nil
}

// checkRules is the dry-run inspector for the declarative rule engine: it
// evaluates an argv exactly like the run_cmd policy path and prints the
// decision with the matching rule (if any), mirroring `codex execpolicy
// check`. Usage: loom rules check <program> [args...]
func checkRules(argv []string) error {
	if len(argv) == 0 {
		return errors.New("usage: loom rules check <program> [args...]")
	}
	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	policy := permission.AttachRules(permission.DefaultPolicy(), root, resolved.Rules.LoadOptions(), slog.Default())
	argsJSON, _ := json.Marshal(map[string]any{"program": argv[0], "args": argv[1:]})
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: argsJSON},
		Risk: domain.R2,
	}
	decision := policy.Evaluate(call)
	fmt.Printf("decision: %s\n", decision)
	if process.IsShellProgram(argv[0]) {
		fmt.Println("note: shell interpreters never match prefix rules (always decided per-call at R3)")
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

func defaultStateDirectory() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("resolve user home: %w", err)
	}
	if runtime.GOOS == "darwin" {
		return filepath.Join(home, "Library", "Application Support"), nil
	}
	if stateHome := strings.TrimSpace(os.Getenv("XDG_STATE_HOME")); filepath.IsAbs(stateHome) {
		return filepath.Clean(stateHome), nil
	}
	return filepath.Join(home, ".local", "state"), nil
}

func preparePrivateDataDirectory(directory string, managePermissions bool) error {
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return fmt.Errorf("create session data directory: %w", err)
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return fmt.Errorf("inspect session data directory: %w", err)
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
		return errors.New("session data directory must be a real directory")
	}
	if managePermissions {
		if err := os.Chmod(directory, 0o700); err != nil {
			return fmt.Errorf("secure session data directory: %w", err)
		}
	} else if info.Mode().Perm()&0o077 != 0 {
		return fmt.Errorf("session data directory %q must not be accessible by group or other users", directory)
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
		return domain.DecisionDeny, nil
	}
	a.start(os.Stdin)
	fmt.Fprintf(os.Stderr, "\nApproval required (R%d): %s\nargs hash: %s\nAllow? [y/N] ",
		req.Call.Risk, req.Description, req.Call.ArgsHash)
	return a.awaitAnswer(ctx)
}
