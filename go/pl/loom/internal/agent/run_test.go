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

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func newTestRun(limits domain.Limits) *Run {
	clock := domain.NewFakeClock(time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC))
	return NewRun(domain.NewSessionID(), limits, clock)
}

func TestNewRunStartsPreparing(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	if run.State.Lifecycle != domain.LifecycleActive {
		t.Fatalf("expected active, got %s", run.State.Lifecycle)
	}
	if run.State.Phase != domain.PhasePreparing {
		t.Fatalf("expected preparing, got %s", run.State.Phase)
	}
}

func TestContinueRunFromTerminalCheckpoint(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 23, 12, 0, 0, 0, time.UTC))
	sessionID := domain.NewSessionID()
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "first answer"}}, CreatedAt: clock.Now(),
	}
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 7,
		State:    domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeSucceeded},
		Messages: []domain.Message{message}, Usage: domain.Usage{Turns: 2, InputTokens: 10}, CreatedAt: clock.Now(),
	}
	run, err := ContinueRun(checkpoint, checkpoint.Messages, 7, domain.DefaultLimits(), clock)
	if err != nil {
		t.Fatalf("ContinueRun: %v", err)
	}
	if run.SessionID != sessionID || run.State.Lifecycle != domain.LifecycleActive ||
		run.State.Phase != domain.PhasePreparing || run.Version != 8 || run.persistedVersion != 7 ||
		len(run.pendingEvents) != 1 || run.pendingEvents[0].Type != domain.EventRunCreated ||
		len(run.Messages) != 1 || run.Usage.InputTokens != 10 {
		t.Fatalf("unexpected continued run: %+v pending=%+v", run, run.pendingEvents)
	}
	userEvent := run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "continue"}}, CreatedAt: clock.Now(),
	})
	if userEvent.Sequence != 9 || run.Messages[1].Sequence != 2 {
		t.Fatalf("unexpected continuation sequence: event=%d message=%d", userEvent.Sequence, run.Messages[1].Sequence)
	}
}

func TestContinueRunRejectsUnsafeRecovery(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 2,
		State: domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing}, CreatedAt: clock.Now(),
	}
	if _, err := ContinueRun(checkpoint, nil, 2, domain.DefaultLimits(), clock); !hasErrorCode(err, domain.ErrConflict) {
		t.Fatalf("active checkpoint error = %v, want conflict", err)
	}
	checkpoint.State = domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeSucceeded}
	if _, err := ContinueRun(checkpoint, nil, 3, domain.DefaultLimits(), clock); !hasErrorCode(err, domain.ErrConflict) {
		t.Fatalf("stale checkpoint error = %v, want conflict", err)
	}
}

func TestRecoverRunClosesInterruptedReadOnlyTool(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 23, 13, 0, 0, 0, time.UTC))
	sessionID := domain.NewSessionID()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"a"}`)}
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}}, CreatedAt: clock.Now(),
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: message}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventToolExecutionStarted,
			toolCallAuditPayload{CallID: call.ID, Tool: call.Name, Risk: domain.R1, ArgsHash: "hash"}, clock.Now()),
	}
	run, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 2, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun: %v", err)
	}
	if run.Version != 5 || len(run.pendingEvents) != 3 || len(run.Messages) != 2 {
		t.Fatalf("unexpected recovered run: version=%d events=%v messages=%v", run.Version, run.pendingEvents, run.Messages)
	}
	result, ok := findToolResult(run, call.ID)
	if !ok || result.Error == nil || result.Error.Code != "interrupted" || !result.Error.Retryable {
		t.Fatalf("unexpected interrupted result: %+v", result)
	}
}

type fakeFileState map[string]string

func (f fakeFileState) SHA256(path string) (string, error) {
	hash, ok := f[path]
	if !ok {
		return "", errors.New("file not found")
	}
	return hash, nil
}

func TestRecoverRunReconcilesInterruptedFileWrite(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "replace_text", Arguments: json.RawMessage(`{}`)}
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}}, CreatedAt: clock.Now(),
	}
	audit := toolCallAuditPayload{
		CallID: call.ID, Tool: call.Name, Risk: domain.R2,
		Recovery: &domain.RecoverySpec{Kind: "file_replace", Path: "/workspace/a", ExpectedHash: "old", ResultHash: "new"},
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: message}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventToolExecutionStarted, audit, clock.Now()),
	}
	for _, test := range []struct {
		name       string
		hash       string
		wantStatus domain.ToolStatus
		wantCode   string
	}{
		{name: "applied", hash: "new", wantStatus: domain.ToolStatusSuccess},
		{name: "not applied", hash: "old", wantStatus: domain.ToolStatusError, wantCode: "interrupted_not_applied"},
	} {
		t.Run(test.name, func(t *testing.T) {
			run, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 2,
				domain.DefaultLimits(), clock, fakeFileState{"/workspace/a": test.hash})
			if err != nil {
				t.Fatalf("RecoverRun: %v", err)
			}
			result, ok := findToolResult(run, call.ID)
			if !ok || result.Status != test.wantStatus || (test.wantCode != "" && (result.Error == nil || result.Error.Code != test.wantCode)) {
				t.Fatalf("unexpected reconciled result: %+v", result)
			}
		})
	}
	if _, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 2,
		domain.DefaultLimits(), clock, fakeFileState{"/workspace/a": "other"}); !hasErrorCode(err, domain.ErrConflict) {
		t.Fatalf("unexpected hash error = %v, want conflict", err)
	}
}

func TestRecoverRunBlocksUncertainNonIdempotentTool(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd", Arguments: json.RawMessage(`{"command":"make"}`)}
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}}, CreatedAt: clock.Now(),
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: message}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventToolExecutionStarted,
			toolCallAuditPayload{CallID: call.ID, Tool: call.Name, Risk: domain.R2, ArgsHash: "hash"}, clock.Now()),
	}
	if _, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 2,
		domain.DefaultLimits(), clock, nil); !hasErrorCode(err, domain.ErrConflict) ||
		!strings.Contains(err.Error(), "uncertain non-idempotent outcome") {
		t.Fatalf("RecoverRun error = %v, want uncertain conflict", err)
	}
}

func TestRecoverRunRejectsCompletionWithoutTranscriptResult(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{}`)}
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}}, CreatedAt: clock.Now(),
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: message}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventToolExecutionStarted,
			toolCallAuditPayload{CallID: call.ID, Tool: call.Name, Risk: domain.R1}, clock.Now()),
		testAgentEvent(t, sessionID, 3, domain.EventToolExecutionCompleted,
			toolExecutionCompletedPayload{CallID: call.ID, Status: domain.ToolStatusSuccess}, clock.Now()),
	}
	if _, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 3,
		domain.DefaultLimits(), clock, nil); !hasErrorCode(err, domain.ErrConflict) ||
		!strings.Contains(err.Error(), "completed without a persisted result") {
		t.Fatalf("RecoverRun error = %v, want inconsistent completion conflict", err)
	}
}

func TestRecoverRunClosesPreparedButUnstartedTool(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "replace_text", Arguments: json.RawMessage(`{}`)}
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}}, CreatedAt: clock.Now(),
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: message}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventPermissionRequested,
			toolCallAuditPayload{CallID: call.ID, Tool: call.Name, Risk: domain.R2, ArgsHash: "hash"}, clock.Now()),
	}
	run, err := RecoverRun(sessionID, nil, []domain.Message{message}, events, 2, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun: %v", err)
	}
	result, ok := findToolResult(run, call.ID)
	if !ok || result.Error == nil || result.Error.Retryable {
		t.Fatalf("unexpected unstarted result: %+v", result)
	}
}

func TestRecoverRunRetriesInterruptedModelRequestWithNewUserPrompt(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	user := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "first"}}, CreatedAt: clock.Now(),
	}
	events := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventUserMessageAdded, domain.MessageEventPayload{Message: user}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventRunStateChanged,
			domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhaseCallingModel}, clock.Now()),
	}
	run, err := RecoverRun(sessionID, nil, []domain.Message{user}, events, 2, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun: %v", err)
	}
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "continue"}}, CreatedAt: clock.Now(),
	})
	if run.State.Phase != domain.PhasePreparing || len(run.Messages) != 2 || run.Messages[1].Sequence != 2 {
		t.Fatalf("unexpected model recovery: %+v", run)
	}
}

func testAgentEvent(t *testing.T, sessionID domain.SessionID, sequence int64, typ domain.EventType, payload any, timestamp time.Time) domain.Event {
	t.Helper()
	raw, err := domain.MarshalPayload(payload)
	if err != nil {
		t.Fatalf("MarshalPayload: %v", err)
	}
	return domain.Event{
		ID: domain.NewEventID(), SessionID: sessionID, Sequence: sequence,
		Type: typ, Timestamp: timestamp, Payload: raw,
	}
}

func TestRunTransitionToCallingModel(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	evts, err := run.TransitionTo(domain.PhaseCallingModel)
	if err != nil {
		t.Fatalf("TransitionTo error: %v", err)
	}
	if run.State.Phase != domain.PhaseCallingModel {
		t.Fatalf("expected calling_model, got %s", run.State.Phase)
	}
	if len(evts) != 1 {
		t.Fatalf("expected 1 event, got %d", len(evts))
	}
}

func TestRunTransitionIllegal(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	_, err := run.TransitionTo(domain.PhaseExecutingTools)
	if err == nil {
		t.Fatal("expected error for illegal transition")
	}
}

func TestRunTerminate(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	evts, err := run.Terminate(domain.OutcomeSucceeded)
	if err != nil {
		t.Fatalf("Terminate error: %v", err)
	}
	if run.State.Lifecycle != domain.LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", run.State.Lifecycle)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("expected succeeded, got %s", run.State.Outcome)
	}
	if len(evts) != 1 {
		t.Fatalf("expected 1 event, got %d", len(evts))
	}
}

func TestRunSuspendAndResume(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	_, err := run.Suspend(domain.SuspensionApproval)
	if err != nil {
		t.Fatalf("Suspend error: %v", err)
	}
	if run.State.Lifecycle != domain.LifecycleSuspended {
		t.Fatalf("expected suspended, got %s", run.State.Lifecycle)
	}

	_, err = run.Resume()
	if err != nil {
		t.Fatalf("Resume error: %v", err)
	}
	if run.State.Lifecycle != domain.LifecycleActive {
		t.Fatalf("expected active, got %s", run.State.Lifecycle)
	}
}

func TestRunAddUserMessage(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	msg := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}},
		CreatedAt: time.Now(),
	}
	evt := run.AddUserMessage(msg)
	if evt.Type != domain.EventUserMessageAdded {
		t.Fatalf("expected user message added event, got %s", evt.Type)
	}
	if len(run.Messages) != 1 {
		t.Fatalf("expected 1 message, got %d", len(run.Messages))
	}
	payload, err := domain.UnmarshalMessageEventPayload(evt.Payload)
	if err != nil {
		t.Fatalf("decode message payload: %v", err)
	}
	if payload.Message.ID != msg.ID || payload.Message.Sequence != 1 {
		t.Fatalf("unexpected message payload: %+v", payload.Message)
	}
}

func TestRunCheckBudget(t *testing.T) {
	limits := domain.Limits{MaxTurns: 10}
	run := newTestRun(limits)
	run.Usage.Turns = 8
	check := run.CheckBudget()
	if !check.HasSoft() {
		t.Error("expected soft breach at 80%")
	}

	run.Usage.Turns = 10
	check = run.CheckBudget()
	if !check.HasHard() {
		t.Error("expected hard breach at 100%")
	}
}

func TestToolRegistry(t *testing.T) {
	registry := NewToolRegistry()
	tool := fakes.EchoTool()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}

	found, ok := registry.Lookup("echo")
	if !ok {
		t.Fatal("expected to find echo tool")
	}
	if found.Definition().Name != "echo" {
		t.Fatalf("expected echo, got %s", found.Definition().Name)
	}

	_, ok = registry.Lookup("nonexistent")
	if ok {
		t.Fatal("expected not to find nonexistent tool")
	}

	defs := registry.List()
	if len(defs) != 1 {
		t.Fatalf("expected 1 tool definition, got %d", len(defs))
	}
}

func TestLoopExecuteTextOnly(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			Text:       "Hello! I can help with that.",
			StopReason: domain.StopEndTurn,
			UsageIn:    100,
			UsageOut:   20,
		},
	)

	registry := NewToolRegistry()
	approver := fakes.NewFakeApprover(domain.DecisionAllow)
	logger := slog.Default()

	run := newTestRun(domain.DefaultLimits())
	// Add initial user message
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}},
		CreatedAt: time.Now(),
	})

	loop := &Loop{
		Run:      run,
		Model:    model,
		Approver: approver,
		Registry: registry,
		Logger:   logger,
	}

	err := loop.Execute(context.Background())
	if err != nil {
		t.Fatalf("Execute error: %v", err)
	}

	if run.State.Lifecycle != domain.LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", run.State.Lifecycle)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("expected succeeded, got %s", run.State.Outcome)
	}
}

func TestLoopExecuteWithToolCalls(t *testing.T) {
	readTool := fakes.ReadFileTool()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{
			Text:       "I found the issue.",
			StopReason: domain.StopEndTurn,
			UsageIn:    200,
			UsageOut:   15,
		},
	)

	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}

	approver := fakes.NewFakeApprover(domain.DecisionAllow)
	logger := slog.Default()

	run := newTestRun(domain.Limits{
		MaxTurns:         10,
		MaxToolCalls:     10,
		MaxParallelTools: 4,
		MaxOutputTokens:  4096,
	})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read test.go"}},
		CreatedAt: time.Now(),
	})

	loop := &Loop{
		Run:      run,
		Model:    model,
		Approver: approver,
		Registry: registry,
		Logger:   logger,
	}

	err := loop.Execute(context.Background())
	if err != nil {
		t.Fatalf("Execute error: %v", err)
	}

	if run.State.Lifecycle != domain.LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", run.State.Lifecycle)
	}
}

// Regression: a tool call whose raw arguments differ from the canonical form
// produced by Prepare (e.g. "./sub" vs "sub", or an absolute path vs the
// workspace-relative display form) must still execute. Previously the
// execution-time validation compared raw against canonical bytes and rejected
// every legitimately normalized call with a "security" error.
func TestLoopExecuteToolCallWithNonCanonicalPath(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "sub"), 0o755); err != nil {
		t.Fatal(err)
	}
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	listDir, err := builtin.NewListDirTool(validator)
	if err != nil {
		t.Fatalf("NewListDirTool: %v", err)
	}

	registry := NewToolRegistry()
	if err := registry.Register(listDir); err != nil {
		t.Fatalf("Register error: %v", err)
	}

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "list_dir", Arguments: json.RawMessage(`{"path":"./sub"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{
			Text:       "listed",
			StopReason: domain.StopEndTurn,
			UsageIn:    50,
			UsageOut:   10,
		},
	)

	run := newTestRun(domain.Limits{
		MaxTurns:         10,
		MaxToolCalls:     10,
		MaxParallelTools: 4,
		MaxOutputTokens:  4096,
	})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "list ./sub"}},
		CreatedAt: time.Now(),
	})

	loop := &Loop{
		Run:      run,
		Model:    model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry,
		Logger:   slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}

	var result *domain.ToolResult
	for i := len(run.Messages) - 1; i >= 0 && result == nil; i-- {
		for _, part := range run.Messages[i].Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				result = part.ToolResult
			}
		}
	}
	if result == nil {
		t.Fatal("no tool result recorded")
	}
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("tool result status = %s, error = %+v", result.Status, result.Error)
	}
}

// The freshness re-check must fail closed when the environment changed after
// the call was prepared: the prepared canonical form no longer matches what
// Prepare produces now.
func TestExecuteToolsFailsClosedWhenEnvironmentDrifts(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "sub"), 0o755); err != nil {
		t.Fatal(err)
	}
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	listDir, err := builtin.NewListDirTool(validator)
	if err != nil {
		t.Fatalf("NewListDirTool: %v", err)
	}

	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "list_dir", Arguments: json.RawMessage(`{"path":"./sub"}`)}
	prepared, err := listDir.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	// The directory disappears after preparation: freshness re-check fails.
	if err := os.RemoveAll(filepath.Join(root, "sub")); err != nil {
		t.Fatal(err)
	}
	if err := verifyPreparedFreshness(context.Background(), listDir, call, prepared); err == nil {
		t.Fatal("verifyPreparedFreshness succeeded after the directory vanished")
	}
}

func TestLoopExecuteCancelled(t *testing.T) {
	model := fakes.NewFakeModel()

	registry := NewToolRegistry()
	logger := slog.Default()

	run := newTestRun(domain.DefaultLimits())
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // cancel immediately

	loop := &Loop{
		Run:      run,
		Model:    model,
		Registry: registry,
		Logger:   logger,
	}

	err := loop.Execute(ctx)
	if err == nil {
		t.Fatal("expected error from cancelled context")
	}

	if run.State.Outcome != domain.OutcomeCancelled {
		t.Fatalf("expected cancelled outcome, got %s", run.State.Outcome)
	}
}

func TestLoopExecuteCancelledPersistsTerminalEvent(t *testing.T) {
	store := &contextCheckingStore{base: fakes.NewFakeStore()}
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	loop := &Loop{Run: run, Model: fakes.NewFakeModel(), Store: store, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("Execute() error = %v, want context.Canceled", err)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if eventIndex(events, domain.EventRunCancelled) < 0 {
		t.Fatalf("cancelled terminal event was not persisted: %v", collectEventTypes(events))
	}
}

func TestLoopExecuteBudgetExhausted(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			Text:       "hi",
			StopReason: domain.StopEndTurn,
		},
	)

	registry := NewToolRegistry()
	logger := slog.Default()

	limits := domain.Limits{MaxTurns: 1} // very tight budget
	run := newTestRun(limits)
	run.Usage.Turns = 1 // already at limit

	loop := &Loop{
		Run:      run,
		Model:    model,
		Registry: registry,
		Logger:   logger,
	}

	err := loop.Execute(context.Background())
	if err == nil {
		t.Fatal("expected budget exhausted error")
	}

	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("expected budget_exhausted, got %s", run.State.Outcome)
	}
}

func TestToolRegistryRejectsDuplicateAndSorts(t *testing.T) {
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register read_file: %v", err)
	}
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register echo: %v", err)
	}
	if err := registry.Register(fakes.EchoTool()); err == nil {
		t.Fatal("expected duplicate registration error")
	}
	defs := registry.List()
	if len(defs) != 2 || defs[0].Name != "echo" || defs[1].Name != "read_file" {
		t.Fatalf("definitions not deterministically sorted: %+v", defs)
	}
}

func TestLoopTracksUsageManifestAndPersistsEvents(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text: "done", StopReason: domain.StopEndTurn, UsageIn: 12, UsageOut: 7,
	})
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	if err := store.CreateSession(context.Background(), run.SessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "question"}},
		CreatedAt: run.Clock.Now(),
	})
	loop := &Loop{
		Run: run, Model: model, ModelName: "test-model", Store: store,
		Registry: NewToolRegistry(), Logger: slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.Usage.InputTokens != 12 || run.Usage.OutputTokens != 7 {
		t.Fatalf("unexpected usage: %+v", run.Usage)
	}
	calls := model.Calls()
	if len(calls) != 1 || calls[0].ModelName != "test-model" {
		t.Fatalf("unexpected model calls: %+v", calls)
	}
	if err := calls[0].ContextManifest.Validate(); err != nil {
		t.Fatalf("invalid context manifest: %v", err)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(events) != 7 {
		t.Fatalf("expected 7 persisted events, got %d", len(events))
	}
	if _, err := domain.UnmarshalMessageEventPayload(events[0].Payload); err != nil {
		t.Fatalf("invalid persisted user message: %v", err)
	}
	if events[2].Type != domain.EventBudgetUpdated || events[3].Type != domain.EventModelRequestStarted {
		t.Fatalf("missing turn budget or model request audit event: %v", collectEventTypes(events))
	}
	if _, err := domain.UnmarshalMessageEventPayload(events[4].Payload); err != nil {
		t.Fatalf("invalid persisted assistant message: %v", err)
	}
	if events[5].Type != domain.EventBudgetUpdated {
		t.Fatalf("missing budget update event: %v", collectEventTypes(events))
	}
}

type stubPromptBuilder struct {
	text  string
	rules []domain.ContextRuleRef
	err   error
}

func (s stubPromptBuilder) Build(context.Context) (string, []domain.ContextRuleRef, error) {
	return s.text, s.rules, s.err
}

func TestLoopPrependsSystemPromptToModelRequest(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn})
	run := newTestRun(domain.DefaultLimits())
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "question"}},
		CreatedAt: run.Clock.Now(),
	})
	loop := &Loop{
		Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default(),
		SystemPrompt: stubPromptBuilder{
			text:  "SYSTEM MARKER",
			rules: []domain.ContextRuleRef{{Source: "loom://builtin/test", Hash: "sha256:abc"}},
		},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	calls := model.Calls()
	if len(calls) != 1 {
		t.Fatalf("expected 1 model call, got %d", len(calls))
	}
	msgs := calls[0].Messages
	if len(msgs) != 2 || msgs[0].Role != domain.RoleSystem || msgs[1].Role != domain.RoleUser {
		t.Fatalf("expected [system, user] request messages, got %+v", msgs)
	}
	if got := msgs[0].Parts[0].Text; got != "SYSTEM MARKER" {
		t.Fatalf("unexpected system prompt text: %q", got)
	}

	// The system prompt is request-scoped: it must not leak into the transcript.
	for _, m := range run.Messages {
		if m.Role == domain.RoleSystem {
			t.Fatal("system prompt leaked into the persisted transcript")
		}
	}

	// Rule sources are audited through the context manifest.
	manifest := calls[0].ContextManifest
	if len(manifest.Rules) != 1 || manifest.Rules[0].Source != "loom://builtin/test" {
		t.Fatalf("manifest rules not populated: %+v", manifest.Rules)
	}
	if err := manifest.Validate(); err != nil {
		t.Fatalf("invalid context manifest: %v", err)
	}
}

func TestLoopContinuesWithoutSystemPromptWhenBuildFails(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn})
	run := newTestRun(domain.DefaultLimits())
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "question"}},
		CreatedAt: run.Clock.Now(),
	})
	loop := &Loop{
		Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default(),
		SystemPrompt: stubPromptBuilder{err: errors.New("boom")},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute should tolerate prompt build failure: %v", err)
	}
	calls := model.Calls()
	if len(calls) != 1 {
		t.Fatalf("expected 1 model call, got %d", len(calls))
	}
	if len(calls[0].Messages) != 1 || calls[0].Messages[0].Role != domain.RoleUser {
		t.Fatalf("expected bare transcript on prompt failure, got %+v", calls[0].Messages)
	}
	if len(calls[0].ContextManifest.Rules) != 0 {
		t.Fatalf("expected no rule refs on prompt failure, got %+v", calls[0].ContextManifest.Rules)
	}
}

func TestAggregateStreamRejectsInterruptedAndMalformedToolCalls(t *testing.T) {
	clock := domain.NewFakeClock(time.Unix(0, 0).UTC())
	tests := []struct {
		name   string
		stream domain.ModelStream
	}{
		{
			name: "transport error",
			stream: &scriptedStream{events: []domain.ModelEvent{
				{Kind: domain.ModelEventTextDelta, TextDelta: "partial"},
			}, terminalErr: errors.New("connection reset")},
		},
		{
			name: "incomplete tool call",
			stream: &scriptedStream{events: []domain.ModelEvent{
				{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: domain.NewToolCallID().String(), ToolName: "read_file"},
				{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
			}},
		},
		{
			name: "invalid arguments",
			stream: &scriptedStream{events: []domain.ModelEvent{
				{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: domain.NewToolCallID().String(), ToolName: "read_file"},
				{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: "{"},
				{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0},
				{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
			}},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			response, err := aggregateStream(tt.stream, clock)
			if err == nil {
				t.Fatal("expected protocol error")
			}
			if tt.name == "transport error" {
				if response.Message.Status != domain.MessageStatusInterrupted || len(response.Message.TextParts()) != 1 {
					t.Fatalf("partial response not retained as interrupted: %+v", response.Message)
				}
			}
		})
	}
}

type scriptedStream struct {
	events      []domain.ModelEvent
	pos         int
	terminalErr error
}

func (s *scriptedStream) Recv() (domain.ModelEvent, error) {
	if s.pos < len(s.events) {
		evt := s.events[s.pos]
		s.pos++
		return evt, nil
	}
	if s.terminalErr != nil {
		return domain.ModelEvent{}, s.terminalErr
	}
	return domain.ModelEvent{}, io.EOF
}

func (*scriptedStream) Close() error { return nil }

func TestRestoreRun(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC))
	id := domain.NewRunID()
	sid := domain.NewSessionID()
	state := domain.RunState{Lifecycle: domain.LifecycleSuspended, Phase: domain.PhaseAwaitingApproval, SuspensionReason: domain.SuspensionApproval}

	run := RestoreRun(id, sid, state, domain.Plan{}, domain.Usage{}, domain.DefaultLimits(), nil, 5, clock)

	if run.ID != id {
		t.Fatalf("ID mismatch")
	}
	if run.State.Lifecycle != domain.LifecycleSuspended {
		t.Fatalf("expected suspended, got %s", run.State.Lifecycle)
	}
	if run.Version != 5 {
		t.Fatalf("expected version 5, got %d", run.Version)
	}
}

func TestLoopEmitsApprovalAndSideEffectEventsSafely(t *testing.T) {
	callID := domain.NewToolCallID()
	tool := newMutableTool(mutableToolConfig{
		definition:    newTestToolDefinition("write_note", []domain.Capability{domain.CapFSWrite}),
		canonicalArgs: json.RawMessage(`{"nested":{"a":1,"b":2},"path":"notes.txt","token":"s3cr3t"}`),
		writePaths:    []string{"/workspace/notes.txt"},
		approvalDesc:  "Write notes.txt",
		argsHash:      "args-hash-123",
		result: domain.ToolResult{
			Status:     domain.ToolStatusSuccess,
			StartedAt:  time.Date(2025, 1, 1, 0, 1, 0, 0, time.UTC),
			FinishedAt: time.Date(2025, 1, 1, 0, 1, 2, 0, time.UTC),
			Metadata:   map[string]string{"mode": "replace"},
			Content: []domain.ContentPart{{
				Kind: domain.PartText,
				Text: `{"path":"/workspace/notes.txt","old_hash":"old123","new_hash":"new456","size":42}`,
			}},
		},
	})
	approver := &callbackApprover{decision: domain.DecisionAllow}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID:        callID,
				Name:      "write_note",
				Arguments: json.RawMessage(`{"token":"s3cr3t","path":"notes.txt","nested":{"b":2,"a":1}}`),
			}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "write notes")

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register tool: %v", err)
	}
	loop := &Loop{Run: run, Model: model, Store: store, Approver: approver, Registry: registry, Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	preparedIdx := eventIndex(events, domain.EventToolCallPrepared)
	requestedIdx := eventIndex(events, domain.EventPermissionRequested)
	resolvedIdx := eventIndex(events, domain.EventPermissionResolved)
	startedIdx := eventIndex(events, domain.EventToolExecutionStarted)
	completedIdx := eventIndex(events, domain.EventToolExecutionCompleted)
	fileChangedIdx := eventIndex(events, domain.EventFileChanged)
	if !(preparedIdx >= 0 && requestedIdx > preparedIdx && resolvedIdx > requestedIdx && startedIdx > resolvedIdx && completedIdx > startedIdx && fileChangedIdx > completedIdx) {
		t.Fatalf("unexpected tool event order: %v", collectEventTypes(events))
	}

	preparedPayload := decodeToolAuditPayload(t, events[preparedIdx].Payload)
	requestedPayload := decodeToolAuditPayload(t, events[requestedIdx].Payload)
	startedPayload := decodeToolAuditPayload(t, events[startedIdx].Payload)
	resolvedPayload := decodePermissionResolvedPayload(t, events[resolvedIdx].Payload)
	completedPayload := decodeToolExecutionCompletedPayload(t, events[completedIdx].Payload)
	changedPayload := decodeFileChangedPayload(t, events[fileChangedIdx].Payload)

	for _, payload := range []toolCallAuditPayload{preparedPayload, requestedPayload, startedPayload} {
		if payload.CallID != callID || payload.Tool != "write_note" || payload.ArgsHash != "args-hash-123" {
			t.Fatalf("unexpected safe payload: %+v", payload)
		}
	}
	if len(preparedPayload.WritePaths) != 1 || preparedPayload.WritePaths[0] != "/workspace/notes.txt" {
		t.Fatalf("unexpected write paths: %+v", preparedPayload.WritePaths)
	}
	if preparedPayload.ApprovalDesc != "Write notes.txt" {
		t.Fatalf("unexpected approval desc: %q", preparedPayload.ApprovalDesc)
	}
	if resolvedPayload.CallID != callID || resolvedPayload.ArgsHash != "args-hash-123" || resolvedPayload.Decision != domain.DecisionAllow {
		t.Fatalf("unexpected permission resolution payload: %+v", resolvedPayload)
	}
	if completedPayload.CallID != callID || completedPayload.Status != domain.ToolStatusSuccess || completedPayload.ErrorCode != "" {
		t.Fatalf("unexpected completion payload: %+v", completedPayload)
	}
	if completedPayload.Metadata["mode"] != "replace" {
		t.Fatalf("unexpected completion metadata: %+v", completedPayload.Metadata)
	}
	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay persisted events: %v", err)
	}
	if len(transcript.Messages) != len(run.Messages) {
		t.Fatalf("replayed %d messages, want %d", len(transcript.Messages), len(run.Messages))
	}
	if _, ok := findToolResultInMessages(transcript.Messages, callID); !ok {
		t.Fatalf("replayed transcript is missing tool result for %s", callID)
	}
	if changedPayload.CallID != callID || changedPayload.Path != "/workspace/notes.txt" || changedPayload.OldHash != "old123" || changedPayload.NewHash != "new456" || changedPayload.Size != 42 {
		t.Fatalf("unexpected file changed payload: %+v", changedPayload)
	}

	requests := approver.Requests()
	if len(requests) != 1 || requests[0].Call.ArgsHash != "args-hash-123" {
		t.Fatalf("approval request did not preserve args hash: %+v", requests)
	}
	for _, idx := range []int{preparedIdx, requestedIdx, startedIdx, completedIdx} {
		payload := string(events[idx].Payload)
		if strings.Contains(payload, "s3cr3t") || strings.Contains(payload, "\"arguments\"") || strings.Contains(payload, "\"token\"") {
			t.Fatalf("payload leaked secret or arguments: %s", payload)
		}
	}
	if strings.Contains(string(events[completedIdx].Payload), "\"content\"") {
		t.Fatalf("completion payload must not contain tool content: %s", string(events[completedIdx].Payload))
	}
}

func TestLoopUsesInjectedPolicyDecisions(t *testing.T) {
	for _, tt := range []struct {
		name        string
		decision    domain.Decision
		wantExecute int
		wantDenied  bool
	}{
		{name: "allow bypasses approval", decision: domain.DecisionAllow, wantExecute: 1},
		{name: "deny blocks execution", decision: domain.DecisionDeny, wantDenied: true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			callID := domain.NewToolCallID()
			tool := newMutableTool(mutableToolConfig{
				definition:    newTestToolDefinition("write_note", []domain.Capability{domain.CapFSWrite}),
				canonicalArgs: json.RawMessage(`{"path":"notes.txt"}`),
				writePaths:    []string{"/workspace/notes.txt"},
				approvalDesc:  "Write notes.txt",
				argsHash:      "policy-hash",
				result:        domain.ToolResult{Status: domain.ToolStatusSuccess},
			})
			model := fakes.NewFakeModel(
				fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{ID: callID, Name: "write_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)}}, StopReason: domain.StopToolUse},
				fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
			)
			run := newTestRun(domain.DefaultLimits())
			addUserTextMessage(run, "write notes")
			registry := NewToolRegistry()
			if err := registry.Register(tool); err != nil {
				t.Fatalf("Register tool: %v", err)
			}
			loop := &Loop{Run: run, Model: model, Policy: fixedPolicy(tt.decision), Registry: registry, Logger: slog.Default()}
			if err := loop.Execute(context.Background()); err != nil {
				t.Fatalf("Execute: %v", err)
			}
			if got := tool.ExecuteCount(); got != tt.wantExecute {
				t.Fatalf("ExecuteCount() = %d, want %d", got, tt.wantExecute)
			}
			result, ok := findToolResult(run, callID)
			if !ok {
				t.Fatal("missing tool result")
			}
			if tt.wantDenied && (result.Error == nil || result.Error.Code != "permission_denied") {
				t.Fatalf("expected permission_denied result, got %+v", result)
			}
		})
	}
}

func TestLoopApprovalErrorPropagatesWithoutDenyResult(t *testing.T) {
	callID := domain.NewToolCallID()
	tool := newMutableTool(mutableToolConfig{
		definition:    newTestToolDefinition("write_note", []domain.Capability{domain.CapFSWrite}),
		canonicalArgs: json.RawMessage(`{"path":"notes.txt"}`),
		writePaths:    []string{"/workspace/notes.txt"},
		approvalDesc:  "Write notes.txt",
		argsHash:      "approval-hash",
		result:        domain.ToolResult{Status: domain.ToolStatusSuccess},
	})
	approver := &callbackApprover{err: errors.New("approval backend unavailable")}
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		ToolCalls:  []domain.ToolCall{{ID: callID, Name: "write_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)}},
		StopReason: domain.StopToolUse,
	})
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "write notes")

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register tool: %v", err)
	}
	loop := &Loop{Run: run, Model: model, Store: store, Approver: approver, Registry: registry, Logger: slog.Default()}
	err := loop.Execute(context.Background())
	if err == nil || !strings.Contains(err.Error(), "approval backend unavailable") {
		t.Fatalf("expected approver error, got %v", err)
	}
	if tool.ExecuteCount() != 0 {
		t.Fatalf("tool executed despite approver error: %d", tool.ExecuteCount())
	}
	if _, ok := findToolResult(run, callID); ok {
		t.Fatalf("approver error must not be converted into deny result")
	}

	events, loadErr := store.LoadEvents(context.Background(), run.SessionID, 0)
	if loadErr != nil {
		t.Fatalf("LoadEvents: %v", loadErr)
	}
	if eventIndex(events, domain.EventPermissionRequested) < 0 {
		t.Fatalf("expected permission requested event, got %v", collectEventTypes(events))
	}
	if eventIndex(events, domain.EventPermissionResolved) >= 0 {
		t.Fatalf("permission should not resolve on approver error: %v", collectEventTypes(events))
	}
}

func TestLoopCommitIntentFailurePreventsExecute(t *testing.T) {
	callID := domain.NewToolCallID()
	tool := newMutableTool(mutableToolConfig{
		definition:    newTestToolDefinition("inspect_note", []domain.Capability{domain.CapFSRead}),
		canonicalArgs: json.RawMessage(`{"path":"notes.txt"}`),
		readPaths:     []string{"/workspace/notes.txt"},
		approvalDesc:  "Inspect notes.txt",
		argsHash:      "intent-hash",
		result:        domain.ToolResult{Status: domain.ToolStatusSuccess},
	})
	store := &failingStore{base: fakes.NewFakeStore(), failOnType: domain.EventToolExecutionStarted, err: errors.New("commit intent failed")}
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "inspect notes")
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: callID, Name: "inspect_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register tool: %v", err)
	}

	loop := &Loop{Run: run, Model: model, Store: store, Registry: registry, Logger: slog.Default()}
	err := loop.Execute(context.Background())
	if err == nil || !strings.Contains(err.Error(), "append events and checkpoint") {
		t.Fatalf("expected atomic intent persistence failure, got %v", err)
	}
	if tool.ExecuteCount() != 0 {
		t.Fatalf("tool executed after intent append failure: %d", tool.ExecuteCount())
	}
}

func TestLoopBlocksRegistryDefinitionDriftWithSecurityError(t *testing.T) {
	callID := domain.NewToolCallID()
	tool := newMutableTool(mutableToolConfig{
		definition:    newTestToolDefinition("write_note", []domain.Capability{domain.CapFSWrite}),
		canonicalArgs: json.RawMessage(`{"path":"notes.txt"}`),
		writePaths:    []string{"/workspace/notes.txt"},
		approvalDesc:  "Write notes.txt",
		argsHash:      "drift-hash",
		result:        domain.ToolResult{Status: domain.ToolStatusSuccess},
	})
	approver := &callbackApprover{
		decision: domain.DecisionAllow,
		after: func(domain.ApprovalRequest) {
			tool.SetDefinition(domain.ToolDefinition{
				Name:         "write_note",
				Description:  "mutated",
				InputSchema:  json.RawMessage(`{"type":"object"}`),
				Capabilities: []domain.Capability{domain.CapFSWrite},
				Source:       domain.ToolSourceMCP,
			})
		},
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: callID, Name: "write_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "write notes")
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register tool: %v", err)
	}

	loop := &Loop{Run: run, Model: model, Store: store, Approver: approver, Registry: registry, Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if tool.ExecuteCount() != 0 {
		t.Fatalf("tool executed despite registry drift: %d", tool.ExecuteCount())
	}
	result, ok := findToolResult(run, callID)
	if !ok || result.Error == nil || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("expected security tool result, got %+v", result)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	completedIdx := eventIndex(events, domain.EventToolExecutionCompleted)
	if completedIdx < 0 {
		t.Fatalf("missing tool execution completed event: %v", collectEventTypes(events))
	}
	completedPayload := decodeToolExecutionCompletedPayload(t, events[completedIdx].Payload)
	if completedPayload.ErrorCode != string(domain.ErrSecurity) {
		t.Fatalf("expected security error code in completion payload, got %+v", completedPayload)
	}
}

type callbackApprover struct {
	mu       sync.Mutex
	decision domain.Decision
	err      error
	after    func(domain.ApprovalRequest)
	requests []domain.ApprovalRequest
}

func (a *callbackApprover) RequestApproval(_ context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	a.mu.Lock()
	a.requests = append(a.requests, req)
	after := a.after
	decision := a.decision
	err := a.err
	a.mu.Unlock()
	if after != nil {
		after(req)
	}
	if err != nil {
		return "", err
	}
	return decision, nil
}

func (a *callbackApprover) Requests() []domain.ApprovalRequest {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := make([]domain.ApprovalRequest, len(a.requests))
	copy(out, a.requests)
	return out
}

type mutableToolConfig struct {
	definition    domain.ToolDefinition
	canonicalArgs json.RawMessage
	readPaths     []string
	writePaths    []string
	approvalDesc  string
	argsHash      string
	result        domain.ToolResult
}

type mutableTool struct {
	mu            sync.Mutex
	definition    domain.ToolDefinition
	canonicalArgs json.RawMessage
	readPaths     []string
	writePaths    []string
	approvalDesc  string
	argsHash      string
	result        domain.ToolResult
	executeCalls  int
}

func newMutableTool(cfg mutableToolConfig) *mutableTool {
	return &mutableTool{
		definition:    cfg.definition,
		canonicalArgs: append(json.RawMessage(nil), cfg.canonicalArgs...),
		readPaths:     append([]string(nil), cfg.readPaths...),
		writePaths:    append([]string(nil), cfg.writePaths...),
		approvalDesc:  cfg.approvalDesc,
		argsHash:      cfg.argsHash,
		result:        cfg.result,
	}
}

func (t *mutableTool) Definition() domain.ToolDefinition {
	t.mu.Lock()
	defer t.mu.Unlock()
	return cloneToolDefinition(t.definition)
}

func (t *mutableTool) SetDefinition(def domain.ToolDefinition) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.definition = cloneToolDefinition(def)
}

func (t *mutableTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	t.mu.Lock()
	def := cloneToolDefinition(t.definition)
	canonicalArgs := append(json.RawMessage(nil), t.canonicalArgs...)
	readPaths := append([]string(nil), t.readPaths...)
	writePaths := append([]string(nil), t.writePaths...)
	approvalDesc := t.approvalDesc
	argsHash := t.argsHash
	t.mu.Unlock()
	if len(canonicalArgs) == 0 {
		canonicalArgs = append(json.RawMessage(nil), call.Arguments...)
	}
	if approvalDesc == "" {
		approvalDesc = "Execute " + def.Name
	}
	if argsHash == "" {
		argsHash = "prepared-hash"
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      def.Name,
			Arguments: canonicalArgs,
		},
		Definition:   def,
		Risk:         def.Risk(),
		ApprovalDesc: approvalDesc,
		ReadPaths:    readPaths,
		WritePaths:   writePaths,
		ArgsHash:     argsHash,
	}, nil
}

func (t *mutableTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.executeCalls++
	result := t.result
	if result.CallID.IsZero() {
		result.CallID = prepared.Call.ID
	}
	if result.StartedAt.IsZero() {
		result.StartedAt = time.Date(2025, 1, 1, 0, 2, 0, 0, time.UTC)
	}
	if result.FinishedAt.IsZero() {
		result.FinishedAt = result.StartedAt.Add(time.Second)
	}
	result.Content = append([]domain.ContentPart(nil), result.Content...)
	result.Metadata = cloneStringMap(result.Metadata)
	return result
}

func (t *mutableTool) ExecuteCount() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.executeCalls
}

type fixedPolicy domain.Decision

func (p fixedPolicy) Evaluate(domain.RiskLevel) domain.Decision { return domain.Decision(p) }

type contextCheckingStore struct {
	base *fakes.FakeStore
}

func (s *contextCheckingStore) CreateSession(ctx context.Context, id domain.SessionID) error {
	return s.base.CreateSession(ctx, id)
}

func (s *contextCheckingStore) AppendEvents(ctx context.Context, id domain.SessionID, expectedVersion int64, events []domain.Event) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	return s.base.AppendEvents(ctx, id, expectedVersion, events)
}

func (s *contextCheckingStore) AppendEventsAndCheckpoint(ctx context.Context, id domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	return s.base.AppendEventsAndCheckpoint(ctx, id, expectedVersion, events, checkpoint)
}

func (s *contextCheckingStore) LoadEvents(ctx context.Context, id domain.SessionID, after int64) ([]domain.Event, error) {
	return s.base.LoadEvents(ctx, id, after)
}

func (s *contextCheckingStore) SaveCheckpoint(ctx context.Context, ckpt domain.Checkpoint) error {
	return s.base.SaveCheckpoint(ctx, ckpt)
}

func (s *contextCheckingStore) LoadLatestCheckpoint(ctx context.Context, id domain.SessionID) (domain.Checkpoint, error) {
	return s.base.LoadLatestCheckpoint(ctx, id)
}

type failingStore struct {
	base       *fakes.FakeStore
	failOnType domain.EventType
	err        error
}

func (s *failingStore) CreateSession(ctx context.Context, sessionID domain.SessionID) error {
	return s.base.CreateSession(ctx, sessionID)
}

func (s *failingStore) AppendEvents(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event) error {
	for _, evt := range events {
		if evt.Type == s.failOnType {
			return s.err
		}
	}
	return s.base.AppendEvents(ctx, sessionID, expectedVersion, events)
}

func (s *failingStore) AppendEventsAndCheckpoint(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	for _, evt := range events {
		if evt.Type == s.failOnType {
			return errors.New("injected persistence failure")
		}
	}
	return s.base.AppendEventsAndCheckpoint(ctx, sessionID, expectedVersion, events, checkpoint)
}

func (s *failingStore) LoadEvents(ctx context.Context, sessionID domain.SessionID, after int64) ([]domain.Event, error) {
	return s.base.LoadEvents(ctx, sessionID, after)
}

func (s *failingStore) SaveCheckpoint(ctx context.Context, ckpt domain.Checkpoint) error {
	return s.base.SaveCheckpoint(ctx, ckpt)
}

func (s *failingStore) LoadLatestCheckpoint(ctx context.Context, sessionID domain.SessionID) (domain.Checkpoint, error) {
	return s.base.LoadLatestCheckpoint(ctx, sessionID)
}

func newTestToolDefinition(name string, capabilities []domain.Capability) domain.ToolDefinition {
	return domain.ToolDefinition{
		Name:         name,
		Description:  "test tool",
		InputSchema:  json.RawMessage(`{"type":"object"}`),
		Capabilities: append([]domain.Capability(nil), capabilities...),
		Source:       domain.ToolSourceBuiltin,
	}
}

func cloneToolDefinition(def domain.ToolDefinition) domain.ToolDefinition {
	def.Capabilities = append([]domain.Capability(nil), def.Capabilities...)
	def.InputSchema = append(json.RawMessage(nil), def.InputSchema...)
	def.OutputSchema = append(json.RawMessage(nil), def.OutputSchema...)
	return def
}

func cloneStringMap(values map[string]string) map[string]string {
	if len(values) == 0 {
		return nil
	}
	out := make(map[string]string, len(values))
	for key, value := range values {
		out[key] = value
	}
	return out
}

func addUserTextMessage(run *Run, text string) {
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		CreatedAt: run.Clock.Now(),
	})
}

func mustCreateSession(t *testing.T, store domain.SessionStore, sessionID domain.SessionID) {
	t.Helper()
	if err := store.CreateSession(context.Background(), sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
}

func hasErrorCode(err error, code domain.ErrorCode) bool {
	var agentErr *domain.AgentError
	return errors.As(err, &agentErr) && agentErr.Code == code
}

func eventIndex(events []domain.Event, typ domain.EventType) int {
	for i, evt := range events {
		if evt.Type == typ {
			return i
		}
	}
	return -1
}

func collectEventTypes(events []domain.Event) []domain.EventType {
	out := make([]domain.EventType, 0, len(events))
	for _, evt := range events {
		out = append(out, evt.Type)
	}
	return out
}

func findToolResult(run *Run, callID domain.ToolCallID) (domain.ToolResult, bool) {
	return findToolResultInMessages(run.Messages, callID)
}

func findToolResultInMessages(messages []domain.Message, callID domain.ToolCallID) (domain.ToolResult, bool) {
	for _, msg := range messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.CallID == callID {
				return *part.ToolResult, true
			}
		}
	}
	return domain.ToolResult{}, false
}

func decodeToolAuditPayload(t *testing.T, payload json.RawMessage) toolCallAuditPayload {
	t.Helper()
	var decoded toolCallAuditPayload
	if err := json.Unmarshal(payload, &decoded); err != nil {
		t.Fatalf("decode tool audit payload: %v", err)
	}
	return decoded
}

func decodePermissionResolvedPayload(t *testing.T, payload json.RawMessage) permissionResolvedPayload {
	t.Helper()
	var decoded permissionResolvedPayload
	if err := json.Unmarshal(payload, &decoded); err != nil {
		t.Fatalf("decode permission resolved payload: %v", err)
	}
	return decoded
}

func decodeToolExecutionCompletedPayload(t *testing.T, payload json.RawMessage) toolExecutionCompletedPayload {
	t.Helper()
	var decoded toolExecutionCompletedPayload
	if err := json.Unmarshal(payload, &decoded); err != nil {
		t.Fatalf("decode tool execution completed payload: %v", err)
	}
	return decoded
}

func decodeFileChangedPayload(t *testing.T, payload json.RawMessage) fileChangedPayload {
	t.Helper()
	var decoded fileChangedPayload
	if err := json.Unmarshal(payload, &decoded); err != nil {
		t.Fatalf("decode file changed payload: %v", err)
	}
	return decoded
}
