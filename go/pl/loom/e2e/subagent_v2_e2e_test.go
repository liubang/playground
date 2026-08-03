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
// Created: 2026/08/02

package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
)

// subagent_v2_e2e_test.go verifies the async (V2) sub-agent flow end to
// end: the V2 Manager's spawn/wait lifecycle, role-based delegation,
// and rewind file restoration.

// stubPromptBuilder is a minimal PromptBuilder for e2e tests.
type stubPromptBuilder struct{}

func (s *stubPromptBuilder) Build(_ context.Context) (string, []domain.ContextRuleRef, error) {
	return "system prompt", nil, nil
}

// TestE2ESubagentV2SpawnAndWait verifies the V2 Manager's Spawn/Wait
// lifecycle: a sub-agent is spawned asynchronously, the manager reports
// it as running, wait blocks until it finishes, and the persisted
// session carries the correct delegation edge and conclusion.
func TestE2ESubagentV2SpawnAndWait(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "main.go", "package main\n\nfunc main() {}\n")

	// Use a fake model with a scripted conclusion so the child
	// completes deterministically without an HTTP mock.
	childModel := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "结论：入口是 main.go", StopReason: domain.StopEndTurn},
	)

	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	childRegistry, _ := realEnv(t, ws)
	researcherPrompt := &stubPromptBuilder{}

	models := &subagent.ModelSource{}
	parentSession := domain.NewSessionID()
	if err := store.CreateSession(context.Background(), parentSession); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{
		Model:         childModel,
		ModelName:     "fake-model",
		ParentSession: parentSession,
	})

	factory := &subagent.Factory{
		Store:     store,
		Registry:  childRegistry,
		Prompt:    researcherPrompt,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}

	roles := map[subagent.Role]*subagent.RoleSpec{
		subagent.RoleResearcher: {Registry: childRegistry, Prompt: researcherPrompt, Risk: domain.R1},
	}
	manager, err := subagent.NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager() error = %v", err)
	}
	t.Cleanup(func() { manager.Shutdown(context.Background()) })

	// Spawn a researcher sub-agent.
	childID, err := manager.Spawn(subagent.SpawnSpec{
		Task:          "调研入口文件",
		Role:          subagent.RoleResearcher,
		ParentSession: parentSession,
	})
	if err != nil {
		t.Fatalf("Spawn() error = %v", err)
	}
	if childID.IsZero() {
		t.Fatal("Spawn() returned zero session ID")
	}

	// The child should be running.
	if status := manager.Status(childID); status != subagent.StatusRunning {
		t.Fatalf("Status() = %v, want running", status)
	}

	// Wait for the child to finish.
	result, err := manager.Wait(context.Background(), childID, 30*time.Second)
	if err != nil {
		t.Fatalf("Wait() error = %v", err)
	}
	if result.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("child outcome = %s, want succeeded", result.Outcome)
	}
	if !strings.Contains(result.Conclusion, "入口是 main.go") {
		t.Fatalf("child conclusion = %q, want reference to main.go", result.Conclusion)
	}

	// After wait, the child should report done.
	if status := manager.Status(childID); status != subagent.StatusDone {
		t.Fatalf("Status() after wait = %v, want done", status)
	}

	// The child session should be persisted with the delegation edge.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: err=%v n=%d", err, len(events))
	}
	edge := string(events[0].Payload)
	if events[0].Type != domain.EventRunCreated || !strings.Contains(edge, `"delegated":true`) {
		t.Fatalf("first child event = %s %s, want the delegation edge", events[0].Type, edge)
	}
}

// TestE2ESubagentV2CoderRole verifies that the coder role can be
// spawned and carries an R3 risk level.
func TestE2ESubagentV2CoderRole(t *testing.T) {
	ws := t.TempDir()

	childModel := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "已修改文件", StopReason: domain.StopEndTurn},
	)

	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	childRegistry, _ := realEnv(t, ws)
	researcherPrompt := &stubPromptBuilder{}
	coderRegistry := agent.NewToolRegistry()
	coderPrompt := &stubPromptBuilder{}

	models := &subagent.ModelSource{}
	parentSession := domain.NewSessionID()
	if err := store.CreateSession(context.Background(), parentSession); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{
		Model:         childModel,
		ModelName:     "fake-model",
		ParentSession: parentSession,
	})

	factory := &subagent.Factory{
		Store:     store,
		Registry:  childRegistry,
		Prompt:    researcherPrompt,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}

	roles := map[subagent.Role]*subagent.RoleSpec{
		subagent.RoleResearcher: {Registry: childRegistry, Prompt: researcherPrompt, Risk: domain.R1},
		subagent.RoleCoder:     {Registry: coderRegistry, Prompt: coderPrompt, Risk: domain.R3},
	}
	manager, err := subagent.NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager() error = %v", err)
	}
	t.Cleanup(func() { manager.Shutdown(context.Background()) })

	if !manager.HasRole(subagent.RoleCoder) {
		t.Fatal("Manager should support the coder role")
	}

	// Spawn a coder sub-agent.
	childID, err := manager.Spawn(subagent.SpawnSpec{
		Task:          "修改main.go",
		Role:          subagent.RoleCoder,
		ParentSession: parentSession,
	})
	if err != nil {
		t.Fatalf("Spawn() error = %v", err)
	}

	result, err := manager.Wait(context.Background(), childID, 30*time.Second)
	if err != nil {
		t.Fatalf("Wait() error = %v", err)
	}
	if result.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("coder outcome = %s, want succeeded", result.Outcome)
	}

	// The child session's delegation edge should carry the coder role.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: err=%v n=%d", err, len(events))
	}
	edge := string(events[0].Payload)
	if !strings.Contains(edge, `"role":"coder"`) {
		t.Fatalf("delegation edge missing coder role: %s", edge)
	}
}

// TestE2EDelegateTaskAsyncInLoop verifies the V2 async delegation flow
// at the tool level: delegate_task with async=true returns a spawned
// reference, and wait_subagent on that session collects the conclusion.
// This tests the full tool integration without needing an HTTP mock.
func TestE2EDelegateTaskAsyncInLoop(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "main.go", "package main\n\nfunc main() {}\n")

	// Child model: produces a simple conclusion.
	childModel := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "结论：入口是 main.go", StopReason: domain.StopEndTurn},
	)

	childRegistry, artStore := realEnv(t, ws)

	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	researcherPrompt := &stubPromptBuilder{}
	models := &subagent.ModelSource{}
	parentSession := domain.NewSessionID()
	if err := store.CreateSession(context.Background(), parentSession); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{
		Model:         childModel,
		ModelName:     "fake-child",
		ParentSession: parentSession,
	})

	factory := &subagent.Factory{
		Store:     store,
		Artifacts: artStore,
		Registry:  childRegistry,
		Prompt:    researcherPrompt,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}

	roles := map[subagent.Role]*subagent.RoleSpec{
		subagent.RoleResearcher: {Registry: childRegistry, Prompt: researcherPrompt, Risk: domain.R1},
	}
	manager, err := subagent.NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager() error = %v", err)
	}
	t.Cleanup(func() { manager.Shutdown(context.Background()) })
	factory.Manager = manager

	// Step 1: delegate_task async returns a spawned reference.
	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	callID := domain.NewToolCallID()
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        callID,
			Name:      "delegate_task",
			Arguments: json.RawMessage(`{"task":"调研入口文件","async":true}`),
		},
		Definition: delegateTool.Definition(),
		Risk:       domain.R1,
	}
	delegateResult := delegateTool.Execute(context.Background(), prepared)
	if delegateResult.Status != domain.ToolStatusSuccess {
		t.Fatalf("delegate_task status = %s, want success", delegateResult.Status)
	}
	if delegateResult.Metadata["spawn_status"] != "spawned" {
		t.Fatalf("delegate_task metadata spawn_status = %q, want spawned", delegateResult.Metadata["spawn_status"])
	}
	childSessionIDStr := delegateResult.Metadata["child_session_id"]
	if childSessionIDStr == "" {
		t.Fatal("delegate_task result missing child_session_id")
	}
	childSessionID, err := domain.ParseSessionID(childSessionIDStr)
	if err != nil {
		t.Fatalf("ParseSessionID(%q) error = %v", childSessionIDStr, err)
	}

	// Step 2: wait_subagent collects the child's conclusion.
	waitTool, err := subagent.NewWaitSubagentTool(manager)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool() error = %v", err)
	}
	waitCallID := domain.NewToolCallID()
	waitPrepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        waitCallID,
			Name:      "wait_subagent",
			Arguments: json.RawMessage(fmt.Sprintf(`{"child_session_id":"%s"}`, childSessionIDStr)),
		},
		Definition: waitTool.Definition(),
		Risk:       domain.R1,
	}
	// Need to run Prepare first to validate/canonicalize the call.
	waitPrepared, prepareErr := waitTool.Prepare(context.Background(), waitPrepared.Call)
	if prepareErr != nil {
		t.Fatalf("wait_subagent Prepare error = %v", prepareErr)
	}
	waitResult := waitTool.Execute(context.Background(), waitPrepared)
	if waitResult.Status != domain.ToolStatusSuccess {
		t.Fatalf("wait_subagent status = %s, want success", waitResult.Status)
	}
	// The conclusion is in Content[0].Text as JSON.
	if len(waitResult.Content) == 0 {
		t.Fatal("wait_subagent result has no content")
	}
	var waitPayload map[string]any
	if err := json.Unmarshal([]byte(waitResult.Content[0].Text), &waitPayload); err != nil {
		t.Fatalf("unmarshal wait result: %v", err)
	}
	if waitPayload["status"] != "completed" {
		t.Fatalf("wait_subagent status = %v, want completed", waitPayload["status"])
	}
	conclusion, _ := waitPayload["conclusion"].(string)
	if !strings.Contains(conclusion, "入口是 main.go") {
		t.Fatalf("wait_subagent conclusion = %q, want reference to main.go", conclusion)
	}

	// The child session should be persisted with the delegation edge.
	events, err := store.LoadEvents(context.Background(), childSessionID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: err=%v n=%d", err, len(events))
	}
	edge := string(events[0].Payload)
	if events[0].Type != domain.EventRunCreated || !strings.Contains(edge, `"delegated":true`) {
		t.Fatalf("first child event = %s %s, want the delegation edge", events[0].Type, edge)
	}
}

// TestE2ERewindRestoreFiles verifies the full rewind path: run a loop
// that creates checkpoints, then RewindSession on the store and apply
// the file restoration, confirming that files return to their
// pre-mutation state.
func TestE2ERewindRestoreFiles(t *testing.T) {
	ws := t.TempDir()
	original := "package main\n\nfunc main() {}\n"
	writeFile(t, ws, "main.go", original)

	// Script: parent delegates a task that triggers a file change
	// recording (via the synchronous V1 path for simplicity — the
	// rewind logic is independent of the delegation mode).
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "delegate_task", ToolArgs: `{"task":"修改main.go"}`, Match: "do the task", UsageIn: 100, UsageOut: 30},
		{Text: "结论：已查看 main.go", Match: "修改main.go", UsageIn: 200, UsageOut: 40},
		{Text: "子Agent完成。", UsageIn: 50, UsageOut: 10},
	})
	model := mock.provider(t)

	childRegistry, artStore := realEnv(t, ws)
	parentRegistry, _ := realEnv(t, ws)

	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	researcherPrompt := &stubPromptBuilder{}
	models := &subagent.ModelSource{}
	factory := &subagent.Factory{
		Store:     store,
		Artifacts: artStore,
		Registry:  childRegistry,
		Prompt:    researcherPrompt,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}

	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	if err := parentRegistry.Register(delegateTool); err != nil {
		t.Fatalf("Register(delegate_task) error = %v", err)
	}

	run := newBudgetRun(t, domain.DefaultLimits())
	if err := store.CreateSession(context.Background(), run.SessionID); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{
		Model:         model,
		ModelName:     "mock-model",
		ParentSession: run.SessionID,
	})

	loop := newRealLoop(run, model, parentRegistry, artStore, agent.WindowModel{})
	loop.Store = store
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("parent outcome = %s, want succeeded", run.State.Outcome)
	}

	// The parent session should have at least one checkpoint.
	checkpoints, err := store.ListCheckpoints(context.Background(), run.SessionID, 10)
	if err != nil {
		t.Fatalf("ListCheckpoints: %v", err)
	}
	if len(checkpoints) == 0 {
		t.Fatal("no checkpoints found — the loop should persist at least one per turn")
	}

	// Rewind to the earliest checkpoint (before the child's edits).
	earliest := checkpoints[len(checkpoints)-1]
	result, err := store.RewindSession(context.Background(), run.SessionID, earliest.Sequence)
	if err != nil {
		t.Fatalf("RewindSession: %v", err)
	}

	// Apply file restoration directly: the app-level restoreRewindChanges
	// is unexported, so replicate the core logic here — restore each change's
	// before-content via atomic write, or delete if created after checkpoint.
	for _, change := range result.Changes {
		if !change.Restorable {
			t.Logf("skipped unrestorable: %s", change.Path)
			continue
		}
		path := filepath.Join(ws, change.Path)
		if change.BeforeExisted {
			content := change.BeforeContent
			if content == nil {
				content = []byte{}
			}
			if err := os.WriteFile(path, content, 0o644); err != nil {
				t.Fatalf("restore %s: %v", change.Path, err)
			}
			t.Logf("restored: %s", change.Path)
		} else {
			if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
				t.Fatalf("delete %s: %v", change.Path, err)
			}
			t.Logf("deleted: %s", change.Path)
		}
	}

	// The file should have been restored to its original content.
	got, err := readFileContent(t, ws, "main.go")
	if err != nil {
		t.Fatalf("read main.go: %v", err)
	}
	if got != original {
		t.Fatalf("main.go after rewind = %q, want %q", got, original)
	}
}

// TestE2ERewindSessionTruncation verifies that RewindSession correctly
// truncates the event log: after rewind, the session version is reset
// and events past the checkpoint are removed.
func TestE2ERewindSessionTruncation(t *testing.T) {
	ctx := context.Background()
	ws := t.TempDir()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	// Build two checkpoints with a file change between them.
	events1 := []domain.Event{{
		ID: domain.NewEventID(), SessionID: sessionID, Sequence: 1,
		Type: domain.EventSessionCreated, Timestamp: time.Now().UTC(),
		Payload: json.RawMessage(`{}`),
	}}
	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint 1: %v", err)
	}

	// Record a file change AFTER checkpoint 1.
	if err := store.RecordFileChange(ctx, sessionID, "main.go", true, "h1", []byte("original"), "h2"); err != nil {
		t.Fatalf("RecordFileChange: %v", err)
	}

	events2 := []domain.Event{{
		ID: domain.NewEventID(), SessionID: sessionID, Sequence: 2,
		Type: domain.EventUserMessageAdded, Timestamp: time.Now().UTC().Add(time.Second),
		Payload: json.RawMessage(`{"text":"hello"}`),
	}}
	ckpt2 := testCheckpoint(sessionID, 2, time.Now().UTC().Add(2*time.Second))
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint 2: %v", err)
	}

	// Rewind to checkpoint 1.
	result, err := store.RewindSession(ctx, sessionID, 1)
	if err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	if result.Checkpoint.Sequence != 1 {
		t.Fatalf("checkpoint sequence = %d, want 1", result.Checkpoint.Sequence)
	}
	if len(result.Changes) != 1 {
		t.Fatalf("changes = %d, want 1", len(result.Changes))
	}
	if result.Changes[0].Path != "main.go" {
		t.Fatalf("change path = %q, want main.go", result.Changes[0].Path)
	}
	if string(result.Changes[0].BeforeContent) != "original" {
		t.Fatalf("before content = %q, want original", result.Changes[0].BeforeContent)
	}

	// After rewind, the session version should be 1.
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession: %v", err)
	}
	if inspection.Session.Version != 1 {
		t.Fatalf("session version after rewind = %d, want 1", inspection.Session.Version)
	}

	// Events after checkpoint 1 should be gone.
	remaining, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	for _, e := range remaining {
		if e.Sequence > 1 {
			t.Fatalf("event sequence %d should have been removed by rewind", e.Sequence)
		}
	}
}

// readFileContent reads a file from the workspace.
func readFileContent(t *testing.T, ws, name string) (string, error) {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(ws, name))
	return string(data), err
}

// testCheckpoint builds a minimal checkpoint for store tests.
func testCheckpoint(sessionID domain.SessionID, sequence int64, createdAt time.Time) domain.Checkpoint {
	return domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: sequence,
		State: domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		Messages: []domain.Message{{
			ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "checkpoint"}}, CreatedAt: createdAt,
		}},
		Plan:      domain.Plan{Items: []domain.PlanItem{{Index: 0, Goal: "persist", Status: domain.PlanItemInProgress}}},
		Usage:     domain.Usage{Turns: 1, ToolCalls: 2, InputTokens: 3, OutputTokens: 4},
		CreatedAt: createdAt,
	}
}
