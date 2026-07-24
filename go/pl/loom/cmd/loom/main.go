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
	"syscall"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/ui"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	version               = "0.2.0-dev"
	sessionDBEnv          = "LOOM_SESSION_DB"
	sessionDBFileName     = "sessions.db"
	artifactDirectoryName = "artifacts"
	artifactGCGracePeriod = 24 * time.Hour
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
		return errors.New("usage: loom <run|resume|chat|sessions|inspect|gc|version> [args]")
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
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func collectArtifactGarbage(ctx context.Context) error {
	dbPath, err := prepareSessionDBPath()
	if err != nil {
		return err
	}
	store, err := session.OpenSQLiteStore(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		return fmt.Errorf("list artifact references: %w", err)
	}
	artifactStore, err := artifact.Open(
		filepath.Join(filepath.Dir(dbPath), artifactDirectoryName),
		domain.DefaultLimits().MaxArtifactBytes,
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
	dbPath, err := sessionDBPath(false)
	if err != nil {
		return err
	}
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

func inspectSession(ctx context.Context, rawSessionID string) error {
	sessionID, err := parseSessionID(rawSessionID)
	if err != nil {
		return err
	}
	dbPath, err := sessionDBPath(false)
	if err != nil {
		return err
	}
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

func runAgent(ctx context.Context, userPrompt string, resumeSessionID *domain.SessionID) error {
	root := strings.TrimSpace(os.Getenv("BUILD_WORKSPACE_DIRECTORY"))
	if root == "" {
		var err error
		root, err = os.Getwd()
		if err != nil {
			return fmt.Errorf("get workspace: %w", err)
		}
	}
	validator, err := workspace.NewPathValidator(root)
	if err != nil {
		return fmt.Errorf("validate workspace: %w", err)
	}
	registry := agent.NewToolRegistry()
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		return err
	}
	book := workspace.NewFileStateBook()
	readFile, err := builtin.NewReadFileTool(validator, book)
	if err != nil {
		return err
	}
	listDir, err := builtin.NewListDirTool(validator)
	if err != nil {
		return err
	}
	searchTool, err := builtin.NewSearchTool(validator, runner)
	if err != nil {
		return err
	}
	globTool, err := builtin.NewGlobTool(validator, runner)
	if err != nil {
		return err
	}
	editTool, err := edit.NewEditTool(validator, book)
	if err != nil {
		return err
	}
	writeFile, err := edit.NewWriteTool(validator)
	if err != nil {
		return err
	}
	gitStatus, err := gittools.NewGitStatusTool(validator)
	if err != nil {
		return err
	}
	gitDiff, err := gittools.NewGitDiffTool(validator)
	if err != nil {
		return err
	}
	gitLog, err := gittools.NewGitLogTool(validator)
	if err != nil {
		return err
	}
	dbPath, err := prepareSessionDBPath()
	if err != nil {
		return err
	}
	artifactStore, err := artifact.Open(
		filepath.Join(filepath.Dir(dbPath), artifactDirectoryName),
		domain.DefaultLimits().MaxArtifactBytes,
	)
	if err != nil {
		return fmt.Errorf("open artifact store: %w", err)
	}
	runCmd, err := command.NewRunCmdToolWithArtifacts(
		validator, runner, artifactStore, int(domain.DefaultLimits().MaxToolOutputBytes),
	)
	if err != nil {
		return err
	}
	for _, tool := range []domain.Tool{
		readFile, listDir, searchTool, globTool, editTool, writeFile, gitStatus, gitDiff, gitLog, runCmd,
	} {
		if err := registry.Register(tool); err != nil {
			return err
		}
	}

	modelName := strings.TrimSpace(os.Getenv("LOOM_MODEL"))
	if modelName == "" {
		return errors.New("LOOM_MODEL is required")
	}
	provider, err := openai.New(openai.Config{
		BaseURL:    os.Getenv("LOOM_BASE_URL"),
		APIKey:     os.Getenv("LOOM_API_KEY"),
		WireAPI:    openai.WireAPI(os.Getenv("LOOM_WIRE_API")),
		MaxRetries: 2,
	})
	if err != nil {
		return err
	}

	store, err := session.OpenSQLiteStore(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()

	var run *agent.Run
	if resumeSessionID == nil {
		run = agent.NewRun(domain.NewSessionID(), domain.DefaultLimits(), domain.RealClock{})
		if err := store.CreateSession(ctx, run.SessionID); err != nil {
			return fmt.Errorf("create session: %w", err)
		}
	} else {
		inspection, err := store.InspectSession(ctx, *resumeSessionID)
		if err != nil {
			return fmt.Errorf("load session for resume: %w", err)
		}
		run, err = agent.RecoverRun(inspection.Session.ID, inspection.Checkpoint,
			inspection.Transcript.Messages, inspection.Events, inspection.Session.Version,
			domain.DefaultLimits(), domain.RealClock{}, validator)
		if err != nil {
			return fmt.Errorf("resume session: %w", err)
		}
	}
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: userPrompt}},
		CreatedAt: time.Now().UTC(),
	})
	var promptBuilder agent.PromptBuilder
	if os.Getenv("LOOM_DISABLE_SYSTEM_PROMPT") != "1" {
		promptBuilder = prompt.NewBuilder(root,
			prompt.WithExtraInstructions(os.Getenv("LOOM_SYSTEM_PROMPT_EXTRA")))
	}
	loop := agent.Loop{
		Run: run, Model: provider, ModelName: modelName, Store: store,
		Approver: consoleApprover{}, Policy: permission.DefaultPolicy(), Registry: registry, Logger: slog.Default(),
		SystemPrompt: promptBuilder,
	}
	fmt.Fprintf(os.Stderr, "loom: session %s\n", run.SessionID)
	executeErr := loop.Execute(ctx)
	var checkpointErr error
	if run.State.Lifecycle == domain.LifecycleTerminal {
		checkpointErr = saveTerminalCheckpoint(ctx, store, run)
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

func prepareSessionDBPath() (string, error) {
	return sessionDBPath(true)
}

func sessionDBPath(create bool) (string, error) {
	configured := strings.TrimSpace(os.Getenv(sessionDBEnv))
	if configured != "" {
		path, err := filepath.Abs(configured)
		if err != nil {
			return "", fmt.Errorf("resolve %s: %w", sessionDBEnv, err)
		}
		if create {
			if err := preparePrivateDataDirectory(filepath.Dir(path), false); err != nil {
				return "", err
			}
		}
		return path, nil
	}

	base, err := defaultStateDirectory()
	if err != nil {
		return "", err
	}
	directory := filepath.Join(base, "loom")
	if create {
		if err := preparePrivateDataDirectory(directory, true); err != nil {
			return "", err
		}
	}
	return filepath.Join(directory, sessionDBFileName), nil
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

// runChat starts the interactive TUI chat session.
// If resumeSessionID is non-nil, the session is resumed; otherwise a new session is created.
func runChat(ctx context.Context, workspaceRoot string, resumeSessionID *domain.SessionID) error {
	if workspaceRoot == "" {
		workspaceRoot = strings.TrimSpace(os.Getenv("BUILD_WORKSPACE_DIRECTORY"))
		if workspaceRoot == "" {
			var err error
			workspaceRoot, err = os.Getwd()
			if err != nil {
				return fmt.Errorf("get workspace: %w", err)
			}
		}
	}

	dbPath, err := prepareSessionDBPath()
	if err != nil {
		return err
	}
	artifactDir := filepath.Join(filepath.Dir(dbPath), artifactDirectoryName)

	modelName := strings.TrimSpace(os.Getenv("LOOM_MODEL"))
	if modelName == "" {
		modelName = "gpt-4o"
	}

	bootstrap, err := app.NewBootstrap(ctx, app.BootstrapConfig{
		WorkspaceRoot:    workspaceRoot,
		SessionDBPath:    dbPath,
		ArtifactDir:      artifactDir,
		ArtifactMaxBytes: domain.DefaultLimits().MaxArtifactBytes,
		ModelName:        modelName,
		BaseURL:          os.Getenv("LOOM_BASE_URL"),
		APIKey:           os.Getenv("LOOM_API_KEY"),
		WireAPI:          openai.WireAPI(os.Getenv("LOOM_WIRE_API")),
		Limits:           domain.DefaultLimits(),
		Policy:           permission.DefaultPolicy(),
		Logger:           slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	if err != nil {
		return fmt.Errorf("bootstrap: %w", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker()
	approver := app.NewChannelApprover()

	controller := app.NewController(app.ControllerConfig{
		Bootstrap: bootstrap,
		Broker:    broker,
		Approver:  approver,
		Logger:    slog.New(slog.NewTextHandler(io.Discard, nil)),
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

	// Start the TUI. Dumb terminals usually lack a Nerd Font patched font,
	// so they fall back to plain text icons unless LOOM_ICONS says otherwise.
	icons := os.Getenv("LOOM_ICONS")
	if icons == "" && os.Getenv("TERM") == "dumb" {
		icons = "plain"
	}
	opts := ui.InitOptions{
		NoColor:   os.Getenv("NO_COLOR") != "" || os.Getenv("TERM") == "dumb",
		AltScreen: os.Getenv("LOOM_ALT_SCREEN") == "1",
		Icons:     icons,
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = controller.Shutdown(shutdownCtx)
		broker.Close()
	}()
	return ui.StartTUI(controller, modelName, workspaceRoot, opts)
}

// isTTY checks whether the given file descriptor is a terminal.
func isTTY(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}

type consoleApprover struct{}

func (consoleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	info, err := os.Stdin.Stat()
	if err != nil {
		return domain.DecisionDeny, fmt.Errorf("inspect stdin: %w", err)
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		return domain.DecisionDeny, nil
	}
	fmt.Fprintf(os.Stderr, "\nApproval required (R%d): %s\nargs hash: %s\nAllow? [y/N] ",
		req.Call.Risk, req.Description, req.Call.ArgsHash)
	answer := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
		answer <- strings.TrimSpace(strings.ToLower(line))
	}()
	select {
	case <-ctx.Done():
		return domain.DecisionDeny, ctx.Err()
	case value := <-answer:
		if value == "y" || value == "yes" {
			return domain.DecisionAllow, nil
		}
		return domain.DecisionDeny, nil
	}
}
