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
	"errors"
	"fmt"
	"log/slog"
	"os"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/version"
)

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
		// Resuming is an explicit intent to continue the conversation, so
		// an archived (read-only) session is restored to active first —
		// otherwise every event append below would fail with
		// ErrSessionArchived.
		if changed, err := sqliteStore.SetSessionArchived(ctx, *resumeSessionID, false); err != nil {
			return fmt.Errorf("unarchive session: %w", err)
		} else if changed {
			fmt.Fprintf(os.Stderr, "loom: session %s was archived; it has been unarchived\n", *resumeSessionID)
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

// saveTerminalCheckpoint persists the terminal snapshot of a finished run.
// A cancelled ctx must not lose the checkpoint: the persist gets a fresh
// 5s budget detached from the cancellation.
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
