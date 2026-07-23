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
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	version           = "0.2.0-dev"
	sessionDBEnv      = "LOOM_SESSION_DB"
	sessionDBFileName = "sessions.db"
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
		return errors.New("usage: loom <run|resume|sessions|inspect|version> [args]")
	}
	switch args[0] {
	case "version":
		fmt.Println("loom", version)
		return nil
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
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
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

func runAgent(ctx context.Context, prompt string, resumeSessionID *domain.SessionID) error {
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
	readFile, err := builtin.NewReadFileTool(validator)
	if err != nil {
		return err
	}
	listDirectory, err := builtin.NewListDirectoryTool(validator)
	if err != nil {
		return err
	}
	searchText, err := builtin.NewSearchTextTool(validator)
	if err != nil {
		return err
	}
	replaceText, err := edit.NewReplaceTextTool(validator)
	if err != nil {
		return err
	}
	applyPatch, err := edit.NewApplyPatchTool(validator)
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
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		return err
	}
	runCommand, err := command.NewRunCommandTool(validator, runner)
	if err != nil {
		return err
	}
	for _, tool := range []domain.Tool{
		readFile, listDirectory, searchText, replaceText, applyPatch, gitStatus, gitDiff, runCommand,
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

	dbPath, err := prepareSessionDBPath()
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
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: prompt}},
		CreatedAt: time.Now().UTC(),
	})
	loop := agent.Loop{
		Run: run, Model: provider, ModelName: modelName, Store: store,
		Approver: consoleApprover{}, Policy: permission.DefaultPolicy(), Registry: registry, Logger: slog.Default(),
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
