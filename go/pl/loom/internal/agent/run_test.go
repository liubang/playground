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
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
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

func TestAddAssistantMessageStampsRunAndTrace(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	run.TraceID = "trace-abc123"
	run.AddAssistantMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}},
		CreatedAt: run.Clock.Now(),
	})
	got := run.Messages[len(run.Messages)-1]
	if got.Metadata["run_id"] != run.ID.String() {
		t.Fatalf("run_id metadata = %q, want %q", got.Metadata["run_id"], run.ID.String())
	}
	if got.Metadata["trace_id"] != "trace-abc123" {
		t.Fatalf("trace_id metadata = %q, want trace-abc123", got.Metadata["trace_id"])
	}
}

func TestAddAssistantMessageOmitsTraceIDWhenTracingOff(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	run.AddAssistantMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}},
		CreatedAt: run.Clock.Now(),
	})
	got := run.Messages[len(run.Messages)-1]
	if got.Metadata["run_id"] != run.ID.String() {
		t.Fatalf("run_id metadata = %q, want %q", got.Metadata["run_id"], run.ID.String())
	}
	if _, ok := got.Metadata["trace_id"]; ok {
		t.Fatalf("trace_id must be omitted when the run has no trace, got %q", got.Metadata["trace_id"])
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
		len(run.Messages) != 1 {
		t.Fatalf("unexpected continued run: %+v pending=%+v", run, run.pendingEvents)
	}
	// v3: a continuation inherits the checkpoint's session-cumulative
	// budget counters (tokens, cost) while the per-prompt observability
	// counters (turns, tool calls, wall time) reset
	// (docs/CONTEXT_DESIGN.md §4.4.3).
	want := domain.Usage{InputTokens: 10}
	if run.Usage != want {
		t.Fatalf("continued run usage = %+v, want %+v (session tokens inherited, per-prompt counters reset)", run.Usage, want)
	}
	userEvent := run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "continue"}}, CreatedAt: clock.Now(),
	})
	if userEvent.Sequence != 9 || run.Messages[1].Sequence != 2 {
		t.Fatalf("unexpected continuation sequence: event=%d message=%d", userEvent.Sequence, run.Messages[1].Sequence)
	}
}

// A completed plan is inert for the next turn — never re-injected into
// model context, archived by frontends at the turn boundary — so the
// continuation drops it. Dropping it also keeps drainPlanUpdates' title
// fallback from leaking the finished plan's title onto the next plan. An
// unfinished plan is live state and survives the prompt boundary.
func TestContinueRunPlanCarryOver(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	newCheckpoint := func(plan domain.Plan) domain.Checkpoint {
		return domain.Checkpoint{
			ID: domain.NewCheckpointID(), SessionID: domain.NewSessionID(), Sequence: 7,
			State:     domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeSucceeded},
			Plan:      plan,
			CreatedAt: clock.Now(),
		}
	}

	completed := domain.Plan{Title: "old task", Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemDone},
		{Index: 1, Goal: "step two", Status: domain.PlanItemDone},
	}}
	run, err := ContinueRun(newCheckpoint(completed), nil, 7, domain.DefaultLimits(), clock)
	if err != nil {
		t.Fatalf("ContinueRun(completed plan): %v", err)
	}
	if len(run.Plan.Items) != 0 || run.Plan.Title != "" {
		t.Fatalf("completed plan carried into continuation: %+v", run.Plan)
	}

	unfinished := domain.Plan{Title: "ongoing task", Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemDone},
		{Index: 1, Goal: "step two", Status: domain.PlanItemInProgress},
	}}
	run, err = ContinueRun(newCheckpoint(unfinished), nil, 7, domain.DefaultLimits(), clock)
	if err != nil {
		t.Fatalf("ContinueRun(unfinished plan): %v", err)
	}
	if run.Plan.Title != "ongoing task" || len(run.Plan.Items) != 2 {
		t.Fatalf("unfinished plan not preserved: %+v", run.Plan)
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
	limits := domain.Limits{MaxTokens: 100}
	clock := domain.NewFakeClock(time.Now().UTC())
	run := NewRun(domain.NewSessionID(), limits, clock)
	run.Usage.InputTokens = 90
	if check := run.CheckBudget(); !check.HasSoft() || check.HasHard() {
		t.Errorf("expected soft-only breach at 90%%, got %+v", check)
	}
	run.Usage.OutputTokens = 10
	if check := run.CheckBudget(); !check.HasHard() {
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
		MaxOutputTokens: 4096,
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

func TestLoopReportsContextUsageAfterResponseAndToolBatch(t *testing.T) {
	readTool := fakes.ReadFileTool()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)}},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 200, UsageOut: 15},
	)
	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read test.go"}},
		CreatedAt: time.Now(),
	})

	var samples []int64
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		StreamHooks: StreamHooks{
			OnContextUsage: func(occupancyTokens int64) {
				samples = append(samples, occupancyTokens)
			},
		},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}

	// Expect at least three reports: after the tool-call response, after the
	// tool batch, and after the final response. Occupancy is the calibrated
	// next-request size: the metered footprint of the last call (fake model
	// reports UsageIn as its context tokens) plus everything appended since.
	if len(samples) < 3 {
		t.Fatalf("context usage reports = %d, want ≥ 3: %+v", len(samples), samples)
	}
	if samples[0] < 100 {
		t.Fatalf("first report = %d, want ≥ metered footprint 100", samples[0])
	}
	if samples[1] <= samples[0] {
		t.Fatalf("tool-batch report = %d, want growth over %d (tool results appended)", samples[1], samples[0])
	}
	last := samples[len(samples)-1]
	if last < 200 {
		t.Fatalf("final report = %d, want ≥ metered footprint 200", last)
	}
}

// Regression: when the 80% budget notice fired right after a tool-call
// response, the notice became the new tail message and the routing readers
// (which read only the tail) silently dropped the pending call; the next
// provider request then died with "unresolved tool_call ids ...". The notice
// is now injected in prepare, and readers scan for the most recent message
// carrying tool calls.
func TestLoopRoutesToolCallsWhenBudgetNoticeFires(t *testing.T) {
	readTool := fakes.ReadFileTool()
	callID := domain.NewToolCallID()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: callID, Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)}},
			StopReason: domain.StopToolUse,
			// Crosses the 80% notice/compaction line for the 100-token window.
			UsageIn:  85,
			UsageOut: 30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 90, UsageOut: 15},
	)
	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read test.go"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		Window: WindowModel{Effective: 100, CompactTrigger: 80, CompactTarget: 50},
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}
	if run.State.Lifecycle != domain.LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", run.State.Lifecycle)
	}

	// The pending call must have been routed and executed despite the notice.
	if run.Usage.ToolCalls != 1 {
		t.Fatalf("tool calls executed = %d, want 1", run.Usage.ToolCalls)
	}
	resultIndex := -1
	for i, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.CallID == callID {
				resultIndex = i
			}
		}
	}
	if resultIndex < 0 {
		t.Fatalf("no tool result recorded for %s", callID)
	}
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("transcript still has unresolved tool calls: %+v", dangling)
	}

	// Any injected budget notice must sit after the tool result, never
	// between the call and its result.
	for i, msg := range run.Messages {
		if msg.Metadata["kind"] != "system_note" {
			continue
		}
		if i < resultIndex {
			t.Fatalf("budget notice at index %d sits before the tool result at %d", i, resultIndex)
		}
	}
}

// Regression: kimi-style providers reset their tool-call id counters every
// turn ("run_cmd_0", "run_cmd_1", ...). The second turn's "run_cmd_0"
// collided with the first turn's recorded result, the replay guard skipped
// its execution, and the next provider request died with "invalid replay
// history: unresolved tool_call ids". Colliding ids are now rewritten to
// fresh unique ones at stream aggregation time.
func TestLoopRewritesCollidingProviderToolCallIDs(t *testing.T) {
	readTool := fakes.ReadFileTool()
	id0, _ := domain.ParseToolCallID("run_cmd_0")
	id1, _ := domain.ParseToolCallID("run_cmd_1")
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: id0, Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}},
			StopReason: domain.StopToolUse,
			UsageIn:    10, UsageOut: 5,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: id0, Name: "read_file", Arguments: json.RawMessage(`{"path":"b.go"}`)},
				{ID: id1, Name: "read_file", Arguments: json.RawMessage(`{"path":"c.go"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    20, UsageOut: 10,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 30, UsageOut: 5},
	)
	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := newTestRun(domain.DefaultLimits())
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read files"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}
	if run.State.Lifecycle != domain.LifecycleTerminal || run.State.Outcome == domain.OutcomeFailed {
		t.Fatalf("run ended as %s/%s, want a successful terminal state", run.State.Lifecycle, run.State.Outcome)
	}
	if run.Usage.ToolCalls != 3 {
		t.Fatalf("tool calls executed = %d, want 3 (turn 2's colliding call must not be skipped)", run.Usage.ToolCalls)
	}
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("transcript still has unresolved tool calls: %+v", dangling)
	}
	// The colliding second-turn "run_cmd_0" must have been rewritten: the
	// transcript carries exactly one call with the original id.
	count := 0
	for _, msg := range run.Messages {
		for _, tc := range msg.ToolCalls() {
			if tc.ID == id0 {
				count++
			}
		}
	}
	if count != 1 {
		t.Fatalf("transcript carries %d calls with id run_cmd_0, want 1 (the colliding one rewritten)", count)
	}
}

// Regression: a provider that streams malformed tool-call arguments
// (pretty-printed JSON with literal newlines, truncated payloads, empty
// arguments) used to kill the whole run at stream finalization
// ("invalid tool call at index N: invalid arguments JSON"). The run must
// survive: the aggregator preserves the raw payload as valid placeholder
// JSON, the tool layer rejects the call with a recoverable prepare error,
// and the transcript stays valid for checkpointing/recovery.
func TestLoopSurvivesMalformedToolCallArguments(t *testing.T) {
	// Emulate the real read_file's strict argument decoding: unknown fields
	// are rejected, so the malformed-arguments placeholder never executes.
	readTool := fakes.ReadFileTool()
	readTool.WithPrepareFn(func(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
		var args struct {
			Path string `json:"path"`
		}
		dec := json.NewDecoder(strings.NewReader(string(call.Arguments)))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&args); err != nil {
			return domain.PreparedCall{}, fmt.Errorf("invalid read_file arguments: %w", err)
		}
		return domain.PreparedCall{
			Call: call, Definition: readTool.Definition(), Risk: domain.R1,
			ApprovalDesc: "Read " + args.Path, ArgsHash: "deadbeef",
		}, nil
	})
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				// Pretty-printed JSON carrying a literal newline inside a string
				// is invalid per encoding/json — as observed from glm-5.2.
				{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage("{\n  \"path\": \"test.go\n\"}")},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)
	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read test.go"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v (run must survive malformed arguments)", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	// The malformed call was routed and rejected by the tool layer with a
	// recoverable error result (never executed).
	if len(readTool.ExecutedCalls()) != 0 {
		t.Fatalf("malformed call must not execute: %+v", readTool.ExecutedCalls())
	}
	errorResults := 0
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			switch part.Kind {
			case domain.PartToolCall:
				if !json.Valid(part.ToolCall.Arguments) {
					t.Fatalf("persisted call args are not valid JSON: %q", part.ToolCall.Arguments)
				}
				if !strings.Contains(string(part.ToolCall.Arguments), "__malformed_arguments") {
					t.Fatalf("malformed payload not preserved as evidence: %q", part.ToolCall.Arguments)
				}
			case domain.PartToolResult:
				if part.ToolResult != nil && part.ToolResult.Status == domain.ToolStatusError {
					errorResults++
				}
			}
		}
	}
	if errorResults != 1 {
		t.Fatalf("recoverable tool error results = %d, want 1", errorResults)
	}
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("transcript has unresolved tool calls: %+v", dangling)
	}
}

// Regression: a run that revises its plan and then ends with it unfinished
// (the model delivered the final answer but forgot the closing bookkeeping)
// must get exactly one extra turn to reconcile — otherwise sessions stick at
// e.g. 2/3 with an in_progress step after a successful run.
func TestLoopReconcilesUnfinishedPlanOnce(t *testing.T) {
	planTool, planCell := newPlanTool(t)
	registry := NewToolRegistry()
	if err := registry.Register(planTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	openSnapshot := `{"plan":[` +
		`{"goal":"step one","status":"done","evidence":["done earlier"]},` +
		`{"goal":"step two","status":"in_progress"}]}`
	closeSnapshot := `{"plan":[` +
		`{"goal":"step one","status":"done","evidence":["done earlier"]},` +
		`{"goal":"step two","status":"done","evidence":["produced"]}]}`
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(openSnapshot)}},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   20,
		},
		fakes.ScriptEntry{Text: "final answer", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 30},
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(closeSnapshot)}},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   20,
		},
		fakes.ScriptEntry{Text: "closed", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 10},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "do it"}}, CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		PlanCell: planCell,
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	// The nudge produced exactly one extra turn: open plan → answer →
	// reconcile prompt → closing update_plan → closing text.
	if calls := len(model.Calls()); calls != 4 {
		t.Fatalf("model calls = %d, want 4 (plan, answer, closing call, closing text)", calls)
	}
	if !run.Plan.IsComplete() {
		t.Fatalf("plan not closed by the reconcile turn: %+v", run.Plan.Items)
	}
	nudges := 0
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && strings.Contains(strings.Join(msg.TextParts(), ""), "still has unfinished steps") {
			nudges++
		}
	}
	if nudges != 1 {
		t.Fatalf("reconcile nudges = %d, want exactly 1", nudges)
	}
}

// The reconcile nudge is one-shot: a model that ends again with an open plan
// is accepted, not looped forever.
func TestLoopReconcileNudgeIsOneShot(t *testing.T) {
	planTool, planCell := newPlanTool(t)
	registry := NewToolRegistry()
	if err := registry.Register(planTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	openSnapshot := `{"plan":[` +
		`{"goal":"step one","status":"done"},` +
		`{"goal":"step two","status":"in_progress"}]}`
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(openSnapshot)}},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   20,
		},
		fakes.ScriptEntry{Text: "answer one", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 30},
		fakes.ScriptEntry{Text: "answer two", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 20},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "do it"}}, CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		PlanCell: planCell,
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded (open plan accepted after one nudge)", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want 3 (plan, nudged answer, accepted answer)", calls)
	}
}

// A plan inherited from an earlier turn (not revised this run) must not
// hijack an unrelated prompt with a reconcile turn.
func TestLoopDoesNotNudgeForStalePlan(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "quick answer", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "fix the typo"}}, CreatedAt: time.Now(),
	})
	// Inherited plan state from a previous turn: never revised this run.
	run.Plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemDone},
		{Index: 1, Goal: "step two", Status: domain.PlanItemInProgress},
	}}
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: NewToolRegistry(), Logger: slog.Default(),
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls = %d, want 1 (stale plan must not nudge)", calls)
	}
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && strings.Contains(strings.Join(msg.TextParts(), ""), "still has unfinished steps") {
			t.Fatal("stale plan triggered a reconcile nudge")
		}
	}
}

func TestLastToolCallsScansPastBookkeepingMessages(t *testing.T) {
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{}`)}
	withCall := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant, Status: domain.MessageStatusFinal, Revision: 1, Sequence: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &call}},
	}
	note := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleSystem, Status: domain.MessageStatusFinal, Revision: 1, Sequence: 2,
		Parts:    []domain.ContentPart{{Kind: domain.PartText, Text: "[budget notice]"}},
		Metadata: map[string]string{"kind": "system_note"},
	}
	if got := lastToolCalls(nil); len(got) != 0 {
		t.Fatalf("lastToolCalls(nil) = %v, want none", got)
	}
	got := lastToolCalls([]domain.Message{withCall, note})
	if len(got) != 1 || got[0].ID != call.ID {
		t.Fatalf("lastToolCalls = %+v, want the call behind the note", got)
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
		MaxOutputTokens: 4096,
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

// midStreamModel emits one text delta, then blocks until the context is
// done (or fails with failErr): a provider stream interrupted mid-flight.
type midStreamModel struct {
	onFirst func()
	failErr error
}

func (m *midStreamModel) Stream(ctx context.Context, _ domain.ModelRequest) (domain.ModelStream, error) {
	return &midStream{ctx: ctx, onFirst: m.onFirst, failErr: m.failErr}, nil
}

type midStream struct {
	ctx     context.Context
	onFirst func()
	failErr error
	sent    bool
}

func (s *midStream) Recv() (domain.ModelEvent, error) {
	if !s.sent {
		s.sent = true
		if s.onFirst != nil {
			s.onFirst()
		}
		return domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "partial"}, nil
	}
	if s.failErr != nil {
		return domain.ModelEvent{}, s.failErr
	}
	<-s.ctx.Done()
	return domain.ModelEvent{}, s.ctx.Err()
}

func (s *midStream) Close() error { return nil }

// TestLoopExecuteCancelledMidStream locks the cancellation routing (the
// reported bug): cancelling while the model stream is in flight must
// terminate the run as cancelled — not emit model.request_failed with a
// misleading "internal" code and a failed outcome.
func TestLoopExecuteCancelledMidStream(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	started := make(chan struct{})
	model := &midStreamModel{onFirst: func() { close(started) }}

	loop := &Loop{Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default()}
	go func() {
		<-started
		cancel()
	}()
	err := loop.Execute(ctx)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Execute() error = %v, want context.Canceled", err)
	}
	if run.State.Outcome != domain.OutcomeCancelled {
		t.Fatalf("outcome = %s, want cancelled", run.State.Outcome)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if eventIndex(events, domain.EventRunCancelled) < 0 {
		t.Fatalf("run.cancelled not persisted: %v", collectEventTypes(events))
	}
	if eventIndex(events, domain.EventModelRequestFailed) >= 0 {
		t.Fatalf("cancellation must not emit model.request_failed: %v", collectEventTypes(events))
	}
}

// TestLoopExecuteModelFailureCarriesMessage locks the failure-audit
// contract: a genuine mid-stream provider error terminates as failed and
// the persisted request_failed event carries the underlying message, so
// the failure is diagnosable without server logs.
func TestLoopExecuteModelFailureCarriesMessage(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	model := &midStreamModel{failErr: errors.New("provider exploded: quota exhausted")}
	loop := &Loop{Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected error from failing model stream")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	idx := eventIndex(events, domain.EventModelRequestFailed)
	if idx < 0 {
		t.Fatalf("model.request_failed not persisted: %v", collectEventTypes(events))
	}
	var payload struct {
		Stage   string `json:"stage"`
		Code    string `json:"code"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(events[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload.Stage != "stream" || payload.Code != "internal" || !strings.Contains(payload.Message, "quota exhausted") {
		t.Fatalf("payload = %+v, want stage=stream code=internal with the provider message", payload)
	}
}

// fastStartRetry keeps retry waits in the millisecond range so retry tests
// stay fast.
var fastStartRetry = StartRetryPolicy{MaxAttempts: 5, InitialWait: time.Millisecond, MaxWait: 2 * time.Millisecond, MaxHintWait: 5 * time.Millisecond}

func rateLimitedStartError() error {
	return domain.NewError(domain.ErrRateLimited, "provider: HTTP 429 too many requests", domain.WithRetryable(true))
}

func countEvents(events []domain.Event, typ domain.EventType) int {
	n := 0
	for _, ev := range events {
		if ev.Type == typ {
			n++
		}
	}
	return n
}

// TestLoopExecuteRetriesRetryableStartFailure locks the rate-limit
// recovery path: a start-stage retryable failure (HTTP 429 class) is
// waited out and retried — audited via model.request_retrying — instead
// of killing the run on the first failure.
func TestLoopExecuteRetriesRetryableStartFailure(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Text: "recovered", StopReason: domain.StopEndTurn},
	)
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: fastStartRetry,
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want nil (the retry should recover)", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if got := len(model.Calls()); got != 3 {
		t.Fatalf("model calls = %d, want 3 (2 failed attempts + 1 success)", got)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if got := countEvents(events, domain.EventModelRequestRetrying); got != 2 {
		t.Fatalf("model.request_retrying count = %d, want 2: %v", got, collectEventTypes(events))
	}
	if eventIndex(events, domain.EventModelRequestFailed) >= 0 {
		t.Fatalf("retried attempts must not emit model.request_failed: %v", collectEventTypes(events))
	}
	idx := eventIndex(events, domain.EventModelRequestRetrying)
	var payload struct {
		Stage       string `json:"stage"`
		Code        string `json:"code"`
		Message     string `json:"message"`
		Attempt     int    `json:"attempt"`
		MaxAttempts int    `json:"max_attempts"`
	}
	if err := json.Unmarshal(events[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal retrying payload: %v", err)
	}
	if payload.Stage != "start" || payload.Code != string(domain.ErrRateLimited) ||
		!strings.Contains(payload.Message, "429") || payload.Attempt != 1 ||
		payload.MaxAttempts != fastStartRetry.MaxAttempts {
		t.Fatalf("retrying payload = %+v, want stage=start code=rate_limited attempt=1/%d with the provider message",
			payload, fastStartRetry.MaxAttempts)
	}
}

// TestLoopExecuteStartRetryGivesUp locks the retry bound: a persistently
// failing provider exhausts MaxAttempts and the run fails with the
// terminal model.request_failed audit — it never hangs the turn forever.
func TestLoopExecuteStartRetryGivesUp(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Err: rateLimitedStartError()},
	)
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: StartRetryPolicy{MaxAttempts: 3, InitialWait: time.Millisecond, MaxWait: 2 * time.Millisecond},
	}
	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected error from a persistently rate-limited model")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if got := len(model.Calls()); got != 3 {
		t.Fatalf("model calls = %d, want 3 (MaxAttempts bound)", got)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if got := countEvents(events, domain.EventModelRequestRetrying); got != 2 {
		t.Fatalf("model.request_retrying count = %d, want 2 (attempts 1-2 retried): %v", got, collectEventTypes(events))
	}
	if countEvents(events, domain.EventModelRequestFailed) != 1 {
		t.Fatalf("model.request_failed count = %d, want 1 (terminal): %v",
			countEvents(events, domain.EventModelRequestFailed), collectEventTypes(events))
	}
	if eventIndex(events, domain.EventRunFailed) < 0 {
		t.Fatalf("run.failed not persisted: %v", collectEventTypes(events))
	}
}

// failStartModel fails every start attempt with a fixed error; onCall
// fires per attempt (used to cancel the context mid-retry).
type failStartModel struct {
	err    error
	onCall func()
	calls  int
}

func (m *failStartModel) Stream(_ context.Context, _ domain.ModelRequest) (domain.ModelStream, error) {
	m.calls++
	if m.onCall != nil {
		m.onCall()
	}
	return nil, m.err
}

// replayStream replays a fixed event sequence; an empty sequence means
// the stream dies silently (immediate EOF, no terminal event) — a
// truncated response.
type replayStream struct {
	events []domain.ModelEvent
	pos    int
}

func (s *replayStream) Recv() (domain.ModelEvent, error) {
	if s.pos >= len(s.events) {
		return domain.ModelEvent{}, io.EOF
	}
	evt := s.events[s.pos]
	s.pos++
	return evt, nil
}

func (s *replayStream) Close() error { return nil }

// streamStepModel returns scripted outcomes in order: a start error, or a
// stream replaying the given events.
type streamStepModel struct {
	calls int
	steps []streamStep
}

type streamStep struct {
	err    error
	events []domain.ModelEvent
}

func (m *streamStepModel) Stream(_ context.Context, _ domain.ModelRequest) (domain.ModelStream, error) {
	step := m.steps[min(m.calls, len(m.steps)-1)]
	m.calls++
	if step.err != nil {
		return nil, step.err
	}
	return &replayStream{events: step.events}, nil
}

// TestStreamAggregatorStreamErrorClassification pins the typed contract:
// a retryable-marked stream error keeps its classification through the
// aggregator so the loop can wait-and-retry; an unmarked one stays a
// plain terminal failure.
// Reasoning blocks seal with their wall-clock span, so reloaded transcripts
// can show it (the WebUI's "thought for Xs" on reopened sessions).
func TestStreamAggregatorReasoningDuration(t *testing.T) {
	base := time.Date(2026, 8, 22, 12, 0, 0, 0, time.UTC)
	clock := domain.NewFakeClock(base)
	agg := NewStreamAggregator(clock, StreamHooks{})

	mustApply := func(evt domain.ModelEvent) {
		t.Helper()
		if err := agg.Apply(evt); err != nil {
			t.Fatalf("Apply(%s): %v", evt.Kind, err)
		}
	}
	mustApply(domain.ModelEvent{Kind: domain.ModelEventReasoningStart})
	clock.Advance(1500 * time.Millisecond)
	mustApply(domain.ModelEvent{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "deep"})
	mustApply(domain.ModelEvent{Kind: domain.ModelEventReasoningEnd})
	mustApply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "answer"})
	mustApply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})

	msg, _, _, _, err := agg.Finalize()
	if err != nil {
		t.Fatalf("Finalize: %v", err)
	}
	if len(msg.Parts) != 2 || msg.Parts[0].Reasoning == nil {
		t.Fatalf("parts = %+v, want reasoning + text", msg.Parts)
	}
	if got := msg.Parts[0].Reasoning.DurationMs; got != 1500 {
		t.Fatalf("DurationMs = %d, want 1500", got)
	}

	// Providers that skip reasoning_start still get the span anchored at the
	// first delta.
	clock2 := domain.NewFakeClock(base)
	agg2 := NewStreamAggregator(clock2, StreamHooks{})
	if err := agg2.Apply(domain.ModelEvent{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "x"}); err != nil {
		t.Fatalf("Apply(delta): %v", err)
	}
	clock2.Advance(800 * time.Millisecond)
	if err := agg2.Apply(domain.ModelEvent{Kind: domain.ModelEventReasoningEnd}); err != nil {
		t.Fatalf("Apply(end): %v", err)
	}
	if err := agg2.Apply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "ok"}); err != nil {
		t.Fatalf("Apply(text): %v", err)
	}
	if err := agg2.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn}); err != nil {
		t.Fatalf("Apply(response_end): %v", err)
	}
	msg2, _, _, _, err := agg2.Finalize()
	if err != nil {
		t.Fatalf("Finalize: %v", err)
	}
	if got := msg2.Parts[0].Reasoning.DurationMs; got != 800 {
		t.Fatalf("DurationMs without reasoning_start = %d, want 800", got)
	}
}

func TestStreamAggregatorStreamErrorClassification(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, StreamHooks{})
	err := agg.Apply(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: "stream read failed: connection reset", Retryable: true})
	if !domain.IsRetryable(err) {
		t.Fatalf("retryable stream error = %v, want retryable", err)
	}
	agg = NewStreamAggregator(domain.RealClock{}, StreamHooks{})
	err = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: "malformed chunk JSON"})
	if domain.IsRetryable(err) {
		t.Fatalf("unmarked stream error = %v, want non-retryable", err)
	}
}

// TestLoopExecuteRetriesSilentStreamTruncation locks the mid-stream
// recovery path: a stream that dies before delivering ANY content
// (silent EOF here; a retryable-marked provider stream error behaves the
// same) is waited out and retried — audited with stage=stream — instead
// of killing the run.
func TestLoopExecuteRetriesSilentStreamTruncation(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	model := &streamStepModel{steps: []streamStep{
		{events: nil}, // truncated: immediate EOF, no terminal event
		{events: []domain.ModelEvent{
			{Kind: domain.ModelEventTextDelta, TextDelta: "recovered"},
			{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
		}},
	}}
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: fastStartRetry,
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want the truncated stream retried to success", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if model.calls != 2 {
		t.Fatalf("model calls = %d, want 2 (truncation + retry)", model.calls)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	idx := eventIndex(events, domain.EventModelRequestRetrying)
	if idx < 0 {
		t.Fatalf("model.request_retrying not persisted: %v", collectEventTypes(events))
	}
	var payload struct {
		Stage string `json:"stage"`
	}
	if err := json.Unmarshal(events[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal retrying payload: %v", err)
	}
	if payload.Stage != "stream" {
		t.Fatalf("retrying stage = %q, want stream", payload.Stage)
	}
	if eventIndex(events, domain.EventModelRequestFailed) >= 0 {
		t.Fatalf("a recovered retry must not emit model.request_failed: %v", collectEventTypes(events))
	}
}

// TestLoopExecuteDoesNotRetryStreamWithPartialContent locks the safety
// boundary: once ANY content streamed (text here; reasoning/tool
// fragments count the same), a failure — even a retryable-classified
// one — preserves the partial draft as an interrupted message and fails,
// never silently re-issues the request.
func TestLoopExecuteDoesNotRetryStreamWithPartialContent(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	model := &streamStepModel{steps: []streamStep{
		{events: []domain.ModelEvent{
			{Kind: domain.ModelEventTextDelta, TextDelta: "partial draft"},
			{Kind: domain.ModelEventStreamError, Error: "openai provider: stream read failed: connection reset", Retryable: true},
		}},
		{events: []domain.ModelEvent{
			{Kind: domain.ModelEventTextDelta, TextDelta: "unreached"},
			{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
		}},
	}}
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: fastStartRetry,
	}
	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected the mid-stream failure to fail the run")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if model.calls != 1 {
		t.Fatalf("model calls = %d, want 1 (no retry with delivered content)", model.calls)
	}
	if eventIndex(run.PendingEvents(), domain.EventModelRequestRetrying) >= 0 {
		t.Fatal("a content-carrying stream failure must not emit model.request_retrying")
	}
	// The partial draft survives as an interrupted assistant message.
	var interrupted string
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleAssistant && msg.Status == domain.MessageStatusInterrupted {
			interrupted = strings.Join(msg.TextParts(), "")
		}
	}
	if interrupted != "partial draft" {
		t.Fatalf("interrupted draft = %q, want %q", interrupted, "partial draft")
	}
}

// TestLoopExecuteStartRetryWaitCancelled: cancelling while the loop
// sleeps out a retryable failure terminates as cancelled — not as a
// failed run with a misleading request_failed audit.
func TestLoopExecuteStartRetryWaitCancelled(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	model := &failStartModel{err: rateLimitedStartError(), onCall: cancel}
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		// The wait itself must not matter: cancellation cuts it short.
		StartRetry: StartRetryPolicy{MaxAttempts: 5, InitialWait: time.Hour, MaxWait: time.Hour},
	}
	if err := loop.Execute(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("Execute() error = %v, want context.Canceled", err)
	}
	if run.State.Outcome != domain.OutcomeCancelled {
		t.Fatalf("outcome = %s, want cancelled", run.State.Outcome)
	}
	if model.calls != 1 {
		t.Fatalf("model calls = %d, want 1 (no retry after cancellation)", model.calls)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if eventIndex(events, domain.EventRunCancelled) < 0 {
		t.Fatalf("run.cancelled not persisted: %v", collectEventTypes(events))
	}
	if eventIndex(events, domain.EventModelRequestFailed) >= 0 {
		t.Fatalf("cancellation during the retry wait must not emit model.request_failed: %v", collectEventTypes(events))
	}
}

func TestStartRetryWait(t *testing.T) {
	loop := &Loop{StartRetry: StartRetryPolicy{
		MaxAttempts: 3, InitialWait: 2 * time.Second, MaxWait: 5 * time.Second, MaxHintWait: time.Minute,
	}}

	if _, retry := loop.startRetryWait(errors.New("plain failure"), 1); retry {
		t.Fatal("non-retryable error must not be retried")
	}
	if _, retry := loop.startRetryWait(rateLimitedStartError(), 3); retry {
		t.Fatal("reaching MaxAttempts must give up")
	}
	// attempt 2 backoff: base 4s, half-jittered into [2s, 4s].
	wait, retry := loop.startRetryWait(rateLimitedStartError(), 2)
	if !retry || wait < 2*time.Second || wait > 4*time.Second {
		t.Fatalf("startRetryWait(attempt 2) = %s, %v; want [2s, 4s], true", wait, retry)
	}
	// A Retry-After hint above the backoff but within the cap wins.
	hinted := domain.NewError(domain.ErrRateLimited, "slow down", domain.WithRetryable(true),
		domain.WithCause(&httpc.StatusError{Code: 429, Status: "429", RetryAfter: 30 * time.Second}))
	if wait, retry := loop.startRetryWait(hinted, 1); !retry || wait != 30*time.Second {
		t.Fatalf("startRetryWait(hint 30s) = %s, %v; want 30s, true", wait, retry)
	}
	// A hint beyond the cap is ignored.
	bigHint := domain.NewError(domain.ErrRateLimited, "slow down", domain.WithRetryable(true),
		domain.WithCause(&httpc.StatusError{Code: 429, Status: "429", RetryAfter: time.Hour}))
	if wait, retry := loop.startRetryWait(bigHint, 1); !retry || wait > 2*time.Second {
		t.Fatalf("startRetryWait(hint 1h) = %s, %v; want <= 2s (hint ignored), true", wait, retry)
	}
}

// TestTrailingRetryStreak pins the crash signature: only an UNRESOLVED
// trailing run of request_retrying events counts.
func TestTrailingRetryStreak(t *testing.T) {
	sessionID := domain.NewSessionID()
	ev := func(seq int64, typ domain.EventType) domain.Event {
		return testAgentEvent(t, sessionID, seq, typ, modelRequestRetryingPayload{}, time.Now().UTC())
	}
	cases := []struct {
		name  string
		types []domain.EventType
		want  int
	}{
		{"empty log", nil, 0},
		{"crash mid-retry", []domain.EventType{
			domain.EventRunCreated, domain.EventModelRequestStarted,
			domain.EventModelRequestRetrying, domain.EventModelRequestRetrying,
		}, 2},
		{"resolved by success", []domain.EventType{
			domain.EventModelRequestStarted, domain.EventModelRequestRetrying,
			domain.EventModelResponseCompleted,
		}, 0},
		{"resolved by terminal failure", []domain.EventType{
			domain.EventModelRequestStarted, domain.EventModelRequestRetrying,
			domain.EventModelRequestFailed, domain.EventRunFailed,
		}, 0},
		{"cancelled mid-wait", []domain.EventType{
			domain.EventModelRequestStarted, domain.EventModelRequestRetrying,
			domain.EventRunCancelled,
		}, 0},
		{"an earlier streak does not leak past a request boundary", []domain.EventType{
			domain.EventModelRequestStarted, domain.EventModelRequestRetrying,
			domain.EventModelRequestStarted, domain.EventModelRequestRetrying,
		}, 1},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var events []domain.Event
			for i, typ := range tc.types {
				events = append(events, ev(int64(i+1), typ))
			}
			if got := trailingRetryStreak(events); got != tc.want {
				t.Fatalf("trailingRetryStreak = %d, want %d", got, tc.want)
			}
		})
	}
}

// TestRecoverRunSeedsStartRetryStreak locks the durable half of retry
// counting: recovery after a crash mid-retry carries the streak into the
// continuation run; a cleanly resolved log seeds nothing.
func TestRecoverRunSeedsStartRetryStreak(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}, CreatedAt: clock.Now(),
	}
	crashed := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventRunCreated, struct{}{}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventModelRequestStarted, modelRequestAuditPayload{}, clock.Now()),
		testAgentEvent(t, sessionID, 3, domain.EventModelRequestRetrying, modelRequestRetryingPayload{Attempt: 1}, clock.Now()),
		testAgentEvent(t, sessionID, 4, domain.EventModelRequestRetrying, modelRequestRetryingPayload{Attempt: 2}, clock.Now()),
	}
	run, err := RecoverRun(sessionID, nil, []domain.Message{message}, crashed, 4, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun: %v", err)
	}
	if run.StartRetryStreak != 2 {
		t.Fatalf("StartRetryStreak = %d, want 2 (crash mid-retry)", run.StartRetryStreak)
	}

	resolved := append(crashed, testAgentEvent(t, sessionID, 5, domain.EventModelRequestFailed, modelRequestFailedPayload{}, clock.Now()))
	run, err = RecoverRun(sessionID, nil, []domain.Message{message}, resolved, 5, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun(resolved): %v", err)
	}
	if run.StartRetryStreak != 0 {
		t.Fatalf("StartRetryStreak = %d, want 0 (the retry loop resolved)", run.StartRetryStreak)
	}
}

// TestLoopExecuteStartRetryHonorsRecoveredStreak: the recovered streak
// counts against the retry budget — a provider failing across crash
// cycles exhausts MaxAttempts instead of getting a fresh budget per
// restart. The streak is consumed by exactly one call.
func TestLoopExecuteStartRetryHonorsRecoveredStreak(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	run.StartRetryStreak = 3

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Err: rateLimitedStartError()},
		fakes.ScriptEntry{Text: "unreached", StopReason: domain.StopEndTurn},
	)
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: fastStartRetry, // MaxAttempts 5; streak 3 → one retry, then give up
	}
	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected the budget-exhausted run to fail")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if got := len(model.Calls()); got != 2 {
		t.Fatalf("model calls = %d, want 2 (streak 3 + attempts 4,5)", got)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if got := countEvents(events, domain.EventModelRequestRetrying); got != 1 {
		t.Fatalf("model.request_retrying count = %d, want 1", got)
	}
	idx := eventIndex(events, domain.EventModelRequestRetrying)
	var payload struct {
		Attempt     int `json:"attempt"`
		MaxAttempts int `json:"max_attempts"`
	}
	if err := json.Unmarshal(events[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal retrying payload: %v", err)
	}
	if payload.Attempt != 4 || payload.MaxAttempts != fastStartRetry.MaxAttempts {
		t.Fatalf("retrying attempt = %d/%d, want 4/%d (streak-aware)",
			payload.Attempt, payload.MaxAttempts, fastStartRetry.MaxAttempts)
	}
	if run.StartRetryStreak != 0 {
		t.Fatalf("StartRetryStreak = %d, want 0 (consumed by the call)", run.StartRetryStreak)
	}
}

// TestOrphanedRunID pins the crash signature: only a tail run.created
// with NO terminal event after it is an orphan.
func TestOrphanedRunID(t *testing.T) {
	sessionID := domain.NewSessionID()
	runID := domain.NewRunID()
	created := testAgentEvent(t, sessionID, 1, domain.EventRunCreated, struct {
		RunID domain.RunID `json:"run_id"`
	}{RunID: runID}, time.Now().UTC())
	seq := int64(1)
	ev := func(typ domain.EventType) domain.Event {
		seq++
		return testAgentEvent(t, sessionID, seq, typ, struct{}{}, time.Now().UTC())
	}

	if got := OrphanedRunID(nil); !got.IsZero() {
		t.Fatalf("OrphanedRunID(nil) = %s, want zero", got)
	}
	// Crash mid-turn: created, activity, no terminal.
	crashed := []domain.Event{created, ev(domain.EventModelRequestStarted), ev(domain.EventModelRequestRetrying)}
	if got := OrphanedRunID(crashed); got != runID {
		t.Fatalf("OrphanedRunID(crashed) = %s, want %s", got, runID)
	}
	// Resolved tails: each terminal outcome closes the run.
	for _, terminal := range []domain.EventType{domain.EventRunCompleted, domain.EventRunFailed, domain.EventRunCancelled} {
		resolved := append([]domain.Event{created, ev(domain.EventModelRequestStarted)}, ev(terminal))
		if got := OrphanedRunID(resolved); !got.IsZero() {
			t.Fatalf("OrphanedRunID(%s) = %s, want zero", terminal, got)
		}
	}
	// A run.created with an undecodable payload degrades to zero rather
	// than a false orphan.
	broken := testAgentEvent(t, sessionID, 1, domain.EventRunCreated, nil, time.Now().UTC())
	broken.Payload = json.RawMessage(`{`)
	if got := OrphanedRunID([]domain.Event{broken}); !got.IsZero() {
		t.Fatalf("OrphanedRunID(broken payload) = %s, want zero", got)
	}
}

// TestRecoverRunMarksOrphanedRun: recovery after a crash writes the
// interrupted marker for the dead run BEFORE opening the continuation;
// a cleanly terminated tail writes none.
func TestRecoverRunMarksOrphanedRun(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	sessionID := domain.NewSessionID()
	message := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}, CreatedAt: clock.Now(),
	}
	deadRunID := domain.NewRunID()
	crashed := []domain.Event{
		testAgentEvent(t, sessionID, 1, domain.EventRunCreated, struct {
			RunID domain.RunID `json:"run_id"`
		}{RunID: deadRunID}, clock.Now()),
		testAgentEvent(t, sessionID, 2, domain.EventModelRequestStarted, modelRequestAuditPayload{}, clock.Now()),
	}
	run, err := RecoverRun(sessionID, nil, []domain.Message{message}, crashed, 2, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun: %v", err)
	}
	idx := eventIndex(run.PendingEvents(), domain.EventRunInterrupted)
	if idx < 0 {
		t.Fatal("no run.interrupted marker for the crash-orphaned run")
	}
	var payload struct {
		RunID  domain.RunID `json:"run_id"`
		Reason string       `json:"reason"`
	}
	if err := json.Unmarshal(run.PendingEvents()[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal marker: %v", err)
	}
	if payload.RunID != deadRunID || payload.Reason == "" {
		t.Fatalf("marker = %+v, want the dead run's ID with a reason", payload)
	}
	// The marker precedes the continuation's run.created.
	if created := eventIndex(run.PendingEvents(), domain.EventRunCreated); created < idx {
		t.Fatalf("marker at %d must precede run.created at %d", idx, created)
	}
	if !run.PendingEvents()[idx].Ignorable {
		t.Fatal("run.interrupted is pure audit and must be ignorable")
	}

	// A cleanly terminated tail marks nothing.
	resolved := append(crashed, testAgentEvent(t, sessionID, 3, domain.EventRunCompleted, struct{}{}, clock.Now()))
	run, err = RecoverRun(sessionID, nil, []domain.Message{message}, resolved, 3, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun(resolved): %v", err)
	}
	if eventIndex(run.PendingEvents(), domain.EventRunInterrupted) >= 0 {
		t.Fatal("run.interrupted written for a cleanly terminated run")
	}
}

// TestLoopExecuteDoesNotRetryQuotaExhaustion locks the quota contract:
// quota exhaustion rides a 429 but is not a transient rate limit — the
// loop fails fast with a quota_exhausted audit instead of burning its
// retry budget on waits that cannot help.
func TestLoopExecuteDoesNotRetryQuotaExhaustion(t *testing.T) {
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)

	quotaErr := domain.NewError(domain.ErrQuotaExhausted, "openai provider: HTTP 429: insufficient_quota")
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Err: quotaErr},
		fakes.ScriptEntry{Text: "unreached", StopReason: domain.StopEndTurn},
	)
	loop := &Loop{
		Run: run, Model: model, Store: store, Registry: NewToolRegistry(), Logger: slog.Default(),
		StartRetry: fastStartRetry,
	}
	if err := loop.Execute(context.Background()); err == nil {
		t.Fatal("expected quota exhaustion to fail the run")
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if got := len(model.Calls()); got != 1 {
		t.Fatalf("model calls = %d, want 1 (quota errors are never retried)", got)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if got := countEvents(events, domain.EventModelRequestRetrying); got != 0 {
		t.Fatalf("model.request_retrying count = %d, want 0", got)
	}
	idx := eventIndex(events, domain.EventModelRequestFailed)
	if idx < 0 {
		t.Fatalf("model.request_failed not persisted: %v", collectEventTypes(events))
	}
	var payload struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(events[idx].Payload, &payload); err != nil {
		t.Fatalf("unmarshal failed payload: %v", err)
	}
	if payload.Code != string(domain.ErrQuotaExhausted) {
		t.Fatalf("failed code = %s, want quota_exhausted", payload.Code)
	}
}

// v3 (CONTEXT_DESIGN §4.4.3): the token budget is session-cumulative and
// must survive the prompt boundary, while per-prompt observability
// counters (turns, tool calls, wall time) reset.
func TestRunSessionTokenBudgetSurvivesPromptBoundary(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 28, 12, 0, 0, 0, time.UTC))
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxTokens: 100}, clock)
	run.Usage.InputTokens = 80
	run.Usage.OutputTokens = 30 // 110 ≥ 100 → hard breach
	run.Usage.Turns = 7

	clock.Advance(2 * time.Minute)
	check := run.CheckBudget()
	if !check.HasHard() {
		t.Fatalf("session tokens must breach the hard limit, got %+v", check)
	}
	found := false
	for _, b := range check.HardBreaches {
		if b == "tokens" {
			found = true
		}
	}
	if !found {
		t.Fatalf("hard breaches = %v, want tokens", check.HardBreaches)
	}
	if run.Usage.WallTime != 2*time.Minute {
		t.Fatalf("Usage.WallTime = %v, want 2m (display counter)", run.Usage.WallTime)
	}

	// A new prompt does NOT clear the session-cumulative budget...
	run.ResetUsageForNewTurn()
	if check := run.CheckBudget(); !check.HasHard() {
		t.Fatalf("session token budget must survive the prompt boundary, got %+v", check)
	}
	// ...but the per-prompt observability counters reset.
	if run.Usage.Turns != 0 || run.Usage.WallTime != 0 {
		t.Fatalf("per-prompt counters must reset: %+v", run.Usage)
	}
	if run.Usage.InputTokens != 80 || run.Usage.OutputTokens != 30 {
		t.Fatalf("session token counters must be preserved: %+v", run.Usage)
	}
}

// Regression (REVIEW H2): Usage.CostUSD was never updated, so the
// MaxEstimatedCostUSD runaway limit could never fire. With cost rates
// configured, token usage must price into the budget.
func TestLoopExecuteCostBudgetExhausted(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    2_000_000,
		},
		fakes.ScriptEntry{Text: "unreachable", StopReason: domain.StopEndTurn},
	)
	limits := domain.Limits{MaxEstimatedCostUSD: 1.0}
	run := newTestRun(limits)

	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: model, Registry: registry, Logger: slog.Default(),
		CostInputUSDPerMTok: 1.0, // $1 per million input tokens → first call costs $2
	}
	// The hard breach enters the soft-landing wrap-up: the model gets one
	// final turn (the second script entry) and the run terminates cleanly.
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want clean soft-landing termination", err)
	}
	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("expected budget_exhausted, got %s", run.State.Outcome)
	}
	if run.Usage.CostUSD != 2.0 {
		t.Fatalf("Usage.CostUSD = %v, want 2.0", run.Usage.CostUSD)
	}
	// The wrap-up delivered a final answer and recorded the audit event.
	if last := run.Messages[len(run.Messages)-1]; last.Role != domain.RoleAssistant ||
		strings.Join(last.TextParts(), "") != "unreachable" {
		t.Fatalf("wrap-up must produce a final assistant message, got %+v", last)
	}
	wrapupEvent := false
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventBudgetWrapupStarted {
			wrapupEvent = true
		}
	}
	if !wrapupEvent {
		t.Fatal("budget.wrapup_started event missing")
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

	// A session token budget already exhausted at loop entry (debt from an
	// earlier prompt) must not kill the run mid-work: the soft landing
	// injects one wrap-up turn and the run terminates with
	// budget_exhausted after the model's final answer.
	clock := domain.NewFakeClock(time.Now().UTC())
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxTokens: 100}, clock)
	run.Usage.InputTokens = 200

	loop := &Loop{
		Run:      run,
		Model:    model,
		Registry: registry,
		Logger:   logger,
	}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want clean soft-landing termination", err)
	}

	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("expected budget_exhausted, got %s", run.State.Outcome)
	}
	// The transcript carries the wrap-up instruction and the final answer.
	wrapUpPrompt, finalAnswer := false, false
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && msg.Metadata["kind"] == "budget_wrapup" {
			wrapUpPrompt = true
		}
		if msg.Role == domain.RoleAssistant && strings.Join(msg.TextParts(), "") == "hi" {
			finalAnswer = true
		}
	}
	if !wrapUpPrompt || !finalAnswer {
		t.Fatalf("soft landing transcript incomplete: prompt=%v answer=%v", wrapUpPrompt, finalAnswer)
	}
}

// Regression (REVIEW M2): an approval request failure used to return with
// the run still active and no terminal event, unlike every callModel
// failure path. The run must terminate with OutcomeFailed.
type askAllPolicy struct{}

func (askAllPolicy) Evaluate(domain.PreparedCall) domain.Verdict {
	return domain.Verdict{Decision: domain.DecisionAsk, Source: "baseline"}
}

type errorApprover struct{}

func (errorApprover) RequestApproval(context.Context, domain.ApprovalRequest) (domain.Decision, error) {
	return domain.DecisionDeny, fmt.Errorf("approval channel broken")
}

func TestLoopApprovalRequestFailureTerminatesRun(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		ToolCalls: []domain.ToolCall{
			{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)},
		},
		StopReason: domain.StopToolUse,
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{
		Run: run, Model: model, Registry: registry, Logger: slog.Default(),
		Policy: askAllPolicy{}, Approver: errorApprover{},
	}

	err := loop.Execute(context.Background())
	if err == nil {
		t.Fatal("expected approval request failure")
	}
	if run.State.Lifecycle != domain.LifecycleTerminal || run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("run state = %+v, want terminal/failed", run.State)
	}
}

// failOnExecutionStartStore fails persistence once a tool execution starts,
// simulating a disk failure at the worst possible moment.
type failOnExecutionStartStore struct {
	*fakes.FakeStore
}

func (s *failOnExecutionStartStore) AppendEventsAndCheckpoint(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	for _, ev := range events {
		if ev.Type == domain.EventToolExecutionStarted {
			return fmt.Errorf("disk on fire")
		}
	}
	return s.FakeStore.AppendEventsAndCheckpoint(ctx, sessionID, expectedVersion, events, checkpoint)
}

// Regression (REVIEW M2): a flush failure before tool execution used to
// leave the run active with no terminal event.
func TestLoopExecutionFlushFailureTerminatesRun(t *testing.T) {
	store := &failOnExecutionStartStore{FakeStore: fakes.NewFakeStore()}
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		ToolCalls: []domain.ToolCall{
			{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)},
		},
		StopReason: domain.StopToolUse,
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{Run: run, Model: model, Registry: registry, Store: store, Logger: slog.Default()}

	err := loop.Execute(context.Background())
	if err == nil {
		t.Fatal("expected flush failure")
	}
	if run.State.Lifecycle != domain.LifecycleTerminal || run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("run state = %+v, want terminal/failed", run.State)
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
	if err := store.CreateSession(context.Background(), run.SessionID, domain.WorkspaceID{}); err != nil {
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
	if len(events) != 8 {
		t.Fatalf("expected 8 persisted events, got %d", len(events))
	}
	if _, err := domain.UnmarshalMessageEventPayload(events[0].Payload); err != nil {
		t.Fatalf("invalid persisted user message: %v", err)
	}
	if events[2].Type != domain.EventBudgetUpdated || events[3].Type != domain.EventModelRequestHeader || events[4].Type != domain.EventModelRequestStarted {
		t.Fatalf("missing turn budget, request header or model request audit event: %v", collectEventTypes(events))
	}
	if _, err := domain.UnmarshalMessageEventPayload(events[5].Payload); err != nil {
		t.Fatalf("invalid persisted assistant message: %v", err)
	}
	if events[6].Type != domain.EventBudgetUpdated {
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

// stubSectionedPromptBuilder exercises the SectionedPromptBuilder path:
// split static/dynamic parts instead of one flat string.
type stubSectionedPromptBuilder struct {
	static  string
	dynamic string
	rules   []domain.ContextRuleRef
}

func (s stubSectionedPromptBuilder) Build(context.Context) (string, []domain.ContextRuleRef, error) {
	text := s.static
	if s.dynamic != "" {
		text = strings.TrimRight(text, "\n") + "\n\n" + s.dynamic
	}
	return text, s.rules, nil
}

func (s stubSectionedPromptBuilder) BuildSections(context.Context) (prompt.Sections, error) {
	return prompt.Sections{Static: s.static, Dynamic: s.dynamic, Refs: s.rules}, nil
}

func TestLoopSplitsSystemPromptForCaching(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn})
	run := newTestRun(domain.DefaultLimits())
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "question"}},
		CreatedAt: run.Clock.Now(),
	})
	run.Plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemInProgress},
	}}
	loop := &Loop{
		Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default(),
		SystemPrompt: stubSectionedPromptBuilder{
			static:  "STATIC PART",
			dynamic: "DYNAMIC PART",
			rules:   []domain.ContextRuleRef{{Source: "loom://builtin/test", Hash: "sha256:abc"}},
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
	// [static system (cache-marked), dynamic system, plan note, user].
	if len(msgs) != 4 {
		t.Fatalf("expected 4 request messages, got %+v", msgs)
	}
	if msgs[0].Role != domain.RoleSystem || msgs[0].Parts[0].Text != "STATIC PART" {
		t.Fatalf("static system message = %+v", msgs[0])
	}
	if msgs[0].Metadata[domain.MetadataPromptCache] != domain.PromptCacheEphemeral {
		t.Fatalf("static part must carry the cache marker, metadata = %+v", msgs[0].Metadata)
	}
	if msgs[1].Role != domain.RoleSystem || msgs[1].Parts[0].Text != "DYNAMIC PART" {
		t.Fatalf("dynamic system message = %+v", msgs[1])
	}
	if len(msgs[1].Metadata) != 0 {
		t.Fatalf("dynamic part must not be cache-marked, metadata = %+v", msgs[1].Metadata)
	}
	if msgs[2].Role != domain.RoleSystem || !strings.Contains(msgs[2].Parts[0].Text, "[task plan]") {
		t.Fatalf("plan note = %+v", msgs[2])
	}
	if msgs[3].Role != domain.RoleUser {
		t.Fatalf("transcript head = %+v", msgs[3])
	}
	// The audit refs flow through the split path unchanged.
	if len(calls[0].ContextManifest.Rules) != 1 || calls[0].ContextManifest.Rules[0].Source != "loom://builtin/test" {
		t.Fatalf("manifest rules = %+v", calls[0].ContextManifest.Rules)
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

// Malformed argument payloads are no longer a protocol error: the
// aggregator preserves them as valid placeholder JSON and lets the tool
// layer reject the call recoverably (see
// TestLoopSurvivesMalformedToolCallArguments).
func TestAggregateStreamSanitizesMalformedArguments(t *testing.T) {
	clock := domain.NewFakeClock(time.Unix(0, 0).UTC())
	stream := &scriptedStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: domain.NewToolCallID().String(), ToolName: "read_file"},
		{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: "{"},
		{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
	}}
	response, err := aggregateStream(stream, clock)
	if err != nil {
		t.Fatalf("aggregateStream error = %v, want sanitized response", err)
	}
	calls := response.Message.ToolCalls()
	if len(calls) != 1 {
		t.Fatalf("tool calls = %d, want 1", len(calls))
	}
	args := string(calls[0].Arguments)
	if !json.Valid(calls[0].Arguments) {
		t.Fatalf("sanitized args are not valid JSON: %q", args)
	}
	if !strings.Contains(args, "__malformed_arguments") || !strings.Contains(args, "\u007b") && !strings.Contains(args, "{") {
		t.Fatalf("raw payload not preserved as evidence: %q", args)
	}
}

func TestAggregateStreamPersistsReasoningBlocks(t *testing.T) {
	clock := domain.NewFakeClock(time.Unix(0, 0).UTC())
	stream := &scriptedStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventReasoningStart},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "deep "},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "thoughts"},
		{Kind: domain.ModelEventReasoningEnd, ReasoningSignature: "sig-1"},
		{Kind: domain.ModelEventTextDelta, TextDelta: "answer"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}
	response, err := aggregateStream(stream, clock)
	if err != nil {
		t.Fatalf("aggregateStream error = %v", err)
	}
	parts := response.Message.Parts
	if len(parts) != 2 {
		t.Fatalf("parts = %+v, want reasoning + text", parts)
	}
	if parts[0].Kind != domain.PartReasoning || parts[0].Reasoning == nil {
		t.Fatalf("parts[0] = %+v", parts[0])
	}
	if parts[0].Reasoning.Text != "deep thoughts" || parts[0].Reasoning.Signature != "sig-1" || parts[0].Reasoning.Redacted {
		t.Fatalf("reasoning = %+v", parts[0].Reasoning)
	}
	if parts[1].Kind != domain.PartText || parts[1].Text != "answer" {
		t.Fatalf("parts[1] = %+v", parts[1])
	}
	if err := response.Message.Validate(); err != nil {
		t.Fatalf("message must validate: %v", err)
	}
}

func TestAggregateStreamPersistsInterleavedReasoningBlocks(t *testing.T) {
	clock := domain.NewFakeClock(time.Unix(0, 0).UTC())
	toolCallID := domain.NewToolCallID()
	stream := &scriptedStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventReasoningStart},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "first"},
		{Kind: domain.ModelEventReasoningEnd, ReasoningSignature: "sig-1"},
		{Kind: domain.ModelEventReasoningStart},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "second"},
		{Kind: domain.ModelEventReasoningEnd, ReasoningSignature: "sig-2"},
		{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: toolCallID.String(), ToolName: "read_file"},
		{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: `{"path":"/tmp/x"}`},
		{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
	}}
	response, err := aggregateStream(stream, clock)
	if err != nil {
		t.Fatalf("aggregateStream error = %v", err)
	}
	parts := response.Message.Parts
	if len(parts) != 3 {
		t.Fatalf("parts = %+v, want 2 reasoning blocks + tool call", parts)
	}
	if parts[0].Reasoning.Signature != "sig-1" || parts[1].Reasoning.Signature != "sig-2" {
		t.Fatalf("reasoning blocks = %+v / %+v", parts[0].Reasoning, parts[1].Reasoning)
	}
	if err := response.Message.Validate(); err != nil {
		t.Fatalf("message must validate: %v", err)
	}
}

func TestAggregateStreamReasoningOnlyResponseIsEmpty(t *testing.T) {
	clock := domain.NewFakeClock(time.Unix(0, 0).UTC())
	stream := &scriptedStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventReasoningStart},
		{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: "only thoughts"},
		{Kind: domain.ModelEventReasoningEnd, ReasoningSignature: "sig-1"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}
	if _, err := aggregateStream(stream, clock); err == nil {
		t.Fatal("reasoning-only response must remain an empty answer")
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
	// The resolved payload must name the REQUESTED event's ID (the approval
	// ID): downstream projections key pending cards by it, and frontends
	// match approval.resolved frames against it.
	if resolvedPayload.ApprovalID != events[requestedIdx].ID {
		t.Fatalf("resolved approval_id = %s, want requested event id %s", resolvedPayload.ApprovalID, events[requestedIdx].ID)
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

// Regression test for the session failure where every approved R3 run_cmd
// (shell or require_escalated) died with a "security" error before
// execution: validatePreparedExecution demanded the prepared risk equal the
// definition's static tier, while run_cmd legitimately elevates R2→R3 per
// call. Elevation must pass; only downgrades are rejected.
func TestLoopExecutesElevatedRiskCallAfterApproval(t *testing.T) {
	callID := domain.NewToolCallID()
	r3 := domain.R3
	tool := newMutableTool(mutableToolConfig{
		definition:    newTestToolDefinition("run_cmd", []domain.Capability{domain.CapProcessExec}),
		canonicalArgs: json.RawMessage(`{"program":"pandora","args":["--help"]}`),
		readPaths:     []string{"/workspace"},
		writePaths:    []string{"/workspace"},
		approvalDesc:  "Run; 'pandora' '--help'; ESCALATED(no-sandbox)[ok?]",
		risk:          &r3,
		result: domain.ToolResult{
			Status:     domain.ToolStatusSuccess,
			StartedAt:  time.Date(2025, 1, 1, 0, 1, 0, 0, time.UTC),
			FinishedAt: time.Date(2025, 1, 1, 0, 1, 2, 0, time.UTC),
			Content:    []domain.ContentPart{{Kind: domain.PartText, Text: `{"stdout":"usage..."}`}},
		},
	})
	approver := &callbackApprover{decision: domain.DecisionAllow}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID:        callID,
				Name:      "run_cmd",
				Arguments: json.RawMessage(`{"program":"pandora","args":["--help"]}`),
			}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	store := fakes.NewFakeStore()
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "show pandora help")

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register tool: %v", err)
	}
	loop := &Loop{Run: run, Model: model, Store: store, Approver: approver, Registry: registry, Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	if got := tool.ExecuteCount(); got != 1 {
		t.Fatalf("ExecuteCount = %d, want 1 (approved R3 call must execute)", got)
	}
	result, ok := findToolResult(run, callID)
	if !ok {
		t.Fatalf("tool result missing for %s", callID)
	}
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("tool result status = %s, want success (error=%+v)", result.Status, result.Error)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	resolvedIdx := eventIndex(events, domain.EventPermissionResolved)
	startedIdx := eventIndex(events, domain.EventToolExecutionStarted)
	completedIdx := eventIndex(events, domain.EventToolExecutionCompleted)
	if !(resolvedIdx >= 0 && startedIdx > resolvedIdx && completedIdx > startedIdx) {
		t.Fatalf("approved R3 call must reach execution: %v", collectEventTypes(events))
	}
	if len(approver.Requests()) != 1 || approver.Requests()[0].Call.Risk != domain.R3 {
		t.Fatalf("approval request must carry the elevated R3 tier: %+v", approver.Requests())
	}
}

func TestValidatePreparedExecutionRiskTiers(t *testing.T) {
	definition := newTestToolDefinition("run_cmd", []domain.Capability{domain.CapProcessExec}) // static R2
	original := domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd"}
	preparedFor := func(risk domain.RiskLevel) domain.PreparedCall {
		return domain.PreparedCall{
			Call:       domain.ToolCall{ID: original.ID, Name: "run_cmd"},
			Definition: definition,
			Risk:       risk,
		}
	}

	// Equal and elevated tiers are accepted; elevation happens per call for
	// shell or require_escalated run_cmd invocations.
	for _, risk := range []domain.RiskLevel{domain.R2, domain.R3} {
		if err := validatePreparedExecution(original, preparedFor(risk), definition); err != nil {
			t.Fatalf("risk %v: unexpected drift error: %v", risk, err)
		}
	}
	// A tier below the definition default weakens the approved policy and
	// must keep failing closed.
	if err := validatePreparedExecution(original, preparedFor(domain.R1), definition); !hasErrorCode(err, domain.ErrSecurity) {
		t.Fatalf("lowered risk error = %v, want security", err)
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

func TestLoopApprovalErrorClosesUnresolvedCalls(t *testing.T) {
	callID1 := domain.NewToolCallID()
	callID2 := domain.NewToolCallID()
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
		ToolCalls: []domain.ToolCall{
			{ID: callID1, Name: "write_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)},
			{ID: callID2, Name: "write_note", Arguments: json.RawMessage(`{"path":"notes.txt"}`)},
		},
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

	// Both calls must be closed with an interrupted result — never confused
	// with a user denial — so the transcript stays replayable for providers
	// that reject dangling tool calls.
	for _, callID := range []domain.ToolCallID{callID1, callID2} {
		result, ok := findToolResult(run, callID)
		if !ok {
			t.Fatalf("call %s has no result; transcript would be dangling", callID)
		}
		if result.Error == nil || result.Error.Code != "interrupted" {
			t.Fatalf("call %s result = %+v, want interrupted error (not permission_denied)", callID, result)
		}
		if result.Error.Code == "permission_denied" {
			t.Fatalf("approver backend error must not masquerade as a user denial: %+v", result)
		}
	}
	assertTranscriptPairingComplete(t, run.Messages)

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

// assertTranscriptPairingComplete verifies every tool call in the transcript
// has a recorded result — the invariant providers validate on replay.
func assertTranscriptPairingComplete(t *testing.T, messages []domain.Message) {
	t.Helper()
	if dangling := unresolvedToolCalls(messages); len(dangling) > 0 {
		t.Fatalf("transcript has %d dangling tool calls: %v", len(dangling), dangling)
	}
}

func TestContinueRunClosesDanglingToolCalls(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 25, 12, 0, 0, 0, time.UTC))
	sessionID := domain.NewSessionID()
	callID1 := domain.NewToolCallID()
	callID2 := domain.NewToolCallID()

	// A terminal checkpoint whose transcript has two calls with no results —
	// exactly what an approval-interrupted run leaves behind.
	messages := []domain.Message{
		{
			ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
			Status: domain.MessageStatusFinal, Revision: 1,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "run two things"}}, CreatedAt: clock.Now(),
		},
		{
			ID: domain.NewMessageID(), Sequence: 2, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID1, Name: "run_cmd", Arguments: json.RawMessage(`{"program":"pandora"}`)}},
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID2, Name: "run_cmd", Arguments: json.RawMessage(`{"program":"pandora"}`)}},
			},
			CreatedAt: clock.Now(),
		},
	}
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 7,
		State:    domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeFailed},
		Messages: messages, CreatedAt: clock.Now(),
	}

	run, err := ContinueRun(checkpoint, messages, 7, domain.DefaultLimits(), clock)
	if err != nil {
		t.Fatalf("ContinueRun: %v", err)
	}
	assertTranscriptPairingComplete(t, run.Messages)
	for _, callID := range []domain.ToolCallID{callID1, callID2} {
		result, ok := findToolResult(run, callID)
		if !ok || result.Error == nil || result.Error.Code != "interrupted" {
			t.Fatalf("call %s result = %+v ok=%v, want interrupted", callID, result, ok)
		}
	}

	// The repaired transcript must satisfy the same sequential ordering the
	// continuation validation enforces, so it can be persisted and continued.
	for i, message := range run.Messages {
		if message.Sequence != int64(i+1) {
			t.Fatalf("repaired transcript message[%d].Sequence = %d, want %d", i, message.Sequence, i+1)
		}
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
	// risk overrides the definition's static risk when non-nil, modelling
	// tools like run_cmd that elevate the tier per call (shell/escalated).
	risk *domain.RiskLevel
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
	risk          *domain.RiskLevel
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
		risk:          cfg.risk,
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
	risk := def.Risk()
	if t.risk != nil {
		risk = *t.risk
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      def.Name,
			Arguments: canonicalArgs,
		},
		Definition:   def,
		Risk:         risk,
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

func (p fixedPolicy) Evaluate(domain.PreparedCall) domain.Verdict {
	return domain.Verdict{Decision: domain.Decision(p), Source: "baseline"}
}

type contextCheckingStore struct {
	base *fakes.FakeStore
}

func (s *contextCheckingStore) CreateSession(ctx context.Context, id domain.SessionID, workspaceID domain.WorkspaceID) error {
	return s.base.CreateSession(ctx, id, workspaceID)
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

func (s *contextCheckingStore) RecordFileChange(ctx context.Context, sessionID domain.SessionID, path string, beforeExisted bool, beforeHash string, beforeContent []byte, afterHash string) error {
	return s.base.RecordFileChange(ctx, sessionID, path, beforeExisted, beforeHash, beforeContent, afterHash)
}

func (s *contextCheckingStore) InspectSession(ctx context.Context, sessionID domain.SessionID) (domain.SessionInspection, error) {
	return s.base.InspectSession(ctx, sessionID)
}

type failingStore struct {
	base       *fakes.FakeStore
	failOnType domain.EventType
	err        error
}

func (s *failingStore) CreateSession(ctx context.Context, sessionID domain.SessionID, workspaceID domain.WorkspaceID) error {
	return s.base.CreateSession(ctx, sessionID, workspaceID)
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

func (s *failingStore) RecordFileChange(ctx context.Context, sessionID domain.SessionID, path string, beforeExisted bool, beforeHash string, beforeContent []byte, afterHash string) error {
	return s.base.RecordFileChange(ctx, sessionID, path, beforeExisted, beforeHash, beforeContent, afterHash)
}

func (s *failingStore) InspectSession(ctx context.Context, sessionID domain.SessionID) (domain.SessionInspection, error) {
	return s.base.InspectSession(ctx, sessionID)
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
	if err := store.CreateSession(context.Background(), sessionID, domain.WorkspaceID{}); err != nil {
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

// --- Context-occupancy budgeting (P0) ---

func TestLoopExecuteInputTokenBudgetDoesNotKill(t *testing.T) {
	// Cumulative input tokens measure cost, not loss of control: a run that
	// blows past MaxInputTokens in one call must still complete normally —
	// context pressure is handled by compaction, never by termination.
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text: "hi", StopReason: domain.StopEndTurn, UsageIn: 500, UsageOut: 5,
	})
	limits := domain.DefaultLimits()
	limits.MaxInputTokens = 100
	run := newTestRun(limits)
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want nil (token totals are not a kill dimension)", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if run.Usage.InputTokens != 500 {
		t.Fatalf("Usage.InputTokens = %d, want 500 (still accounted for display)", run.Usage.InputTokens)
	}
}

func TestShouldCompactOnOccupancy(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Window: WindowModel{Effective: 100, CompactTrigger: 80, CompactTarget: 50}}

	loop.lastCallContext = 85 // past the window-derived trigger
	if !loop.shouldCompact(context.Background()) {
		t.Fatal("occupancy past the trigger should compact")
	}
	loop.lastCallContext = 50
	if loop.shouldCompact(context.Background()) {
		t.Fatal("occupancy below the trigger must not compact")
	}
	loop.Compaction.Force = true
	if !loop.shouldCompact(context.Background()) {
		t.Fatal("forceCompact must trigger compaction")
	}
}

// Regression: a compaction pass that cannot shrink the transcript (e.g.
// everything sits inside the keep-recent window, so masking and archival
// both no-op) must not retrigger on the next loop iteration. Otherwise the
// loop spins forever between preparing and compacting — never calling the
// model again — while spamming compaction events and checkpoints (this once
// grew a session DB to tens of gigabytes in minutes).
func TestShouldCompactDoesNotRetriggerWithoutGrowth(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Window: WindowModel{Effective: 100, CompactTrigger: 80, CompactTarget: 50}}

	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: strings.Repeat("x", 400)}},
		CreatedAt: time.Now(),
	})
	if !loop.shouldCompact(context.Background()) {
		t.Fatal("transcript at the occupancy line should trigger compaction")
	}

	// Simulate a no-progress pass: the condenser left the transcript unchanged.
	loop.Compaction.lastEst = estTokens(run.Messages)
	if loop.shouldCompact(context.Background()) {
		t.Fatal("compaction must not retrigger while the transcript has not grown")
	}
	// Metered occupancy pressure alone must not bypass the guard: another
	// pass over the same transcript cannot help; the run must proceed and
	// let the provider accept or reject the request.
	loop.lastCallContext = 95
	if loop.shouldCompact(context.Background()) {
		t.Fatal("metered occupancy must not retrigger compaction without transcript growth")
	}

	// A provider context-overflow rejection still forces a pass.
	loop.Compaction.Force = true
	if !loop.shouldCompact(context.Background()) {
		t.Fatal("forceCompact must bypass the no-growth guard")
	}
	loop.Compaction.Force = false
	loop.lastCallContext = 0

	// Real transcript growth re-arms compaction.
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "more context here"}},
		CreatedAt: time.Now(),
	})
	if !loop.shouldCompact(context.Background()) {
		t.Fatal("transcript growth past the last pass must re-arm compaction")
	}
}

func TestBudgetNoticeInjection(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{
		Run:   run,
		Model: fakes.NewFakeModel(fakes.ScriptEntry{Text: "HANDOFF", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5}),
		Window: WindowModel{
			Effective: 100, CompactTrigger: 80, CompactTarget: 50, NoticeLevels: []int64{60, 75},
		},
	}

	// Level 1: occupancy crosses the first window-derived level.
	loop.lastCallContext = 65
	loop.injectBudgetNotices(context.Background())
	if len(run.Messages) != 1 || run.Messages[0].Role != domain.RoleSystem ||
		!strings.Contains(run.Messages[0].TextParts()[0], "narrow the scope") {
		t.Fatalf("level-1 occupancy reminder missing: %+v", run.Messages)
	}

	// Level 2: crossing the second level fires the compaction-imminent
	// notice; each level fires once.
	loop.lastCallContext = 78
	loop.injectBudgetNotices(context.Background())
	if len(run.Messages) != 2 || !strings.Contains(run.Messages[1].TextParts()[0], "auto-compaction is imminent") {
		t.Fatalf("level-2 occupancy reminder missing: %+v", run.Messages)
	}
	loop.injectBudgetNotices(context.Background())
	if len(run.Messages) != 2 {
		t.Fatalf("notices must fire once per level: %+v", run.Messages)
	}

	// The reminder is auditable through the budget.notice event.
	noticeEvents := 0
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventBudgetNotice {
			noticeEvents++
		}
	}
	if noticeEvents != 2 {
		t.Fatalf("budget.notice events = %d, want 2", noticeEvents)
	}

	// Compaction re-arms the occupancy notices and resets the calibrated
	// occupancy.
	run.State.Phase = domain.PhaseCompacting
	if err := loop.compact(context.Background()); err != nil {
		t.Fatalf("compact() error = %v", err)
	}
	if loop.notices.fired[dimensionOccupancy] != 0 || loop.lastCallContext != 0 {
		t.Fatalf("compact must re-arm occupancy notices and reset occupancy: fired=%d lastCallContext=%d",
			loop.notices.fired[dimensionOccupancy], loop.lastCallContext)
	}
}

func TestBudgetNoticeTokensAndCost(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	run := NewRun(domain.NewSessionID(), domain.Limits{
		MaxTokens:           100,
		MaxEstimatedCostUSD: 1.0,
	}, clock)
	loop := &Loop{Run: run}

	run.Usage.InputTokens = 85 // 85% → level 1
	run.Usage.CostUSD = 0.97   // 97% → level 2
	loop.injectBudgetNotices(context.Background())
	if len(run.Messages) != 2 {
		t.Fatalf("tokens level-1 and cost level-2 reminders expected: %+v", run.Messages)
	}
	if !strings.Contains(run.Messages[0].TextParts()[0], "session token budget") ||
		!strings.Contains(run.Messages[1].TextParts()[0], "cost budget") {
		t.Fatalf("unexpected reminder texts: %+v", run.Messages)
	}
	// Same levels never refire.
	loop.injectBudgetNotices(context.Background())
	if len(run.Messages) != 2 {
		t.Fatalf("resource reminders must fire once per level: %+v", run.Messages)
	}
}

func TestContextOverflowForcesCompactionAndRetries(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Error: "request failed: maximum context length exceeded"},
		fakes.ScriptEntry{Text: "recovered", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v, want retry after overflow to succeed", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 2 {
		t.Fatalf("model calls = %d, want 2 (overflow then retry)", calls)
	}
	foundCompaction := false
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventContextCompacted {
			foundCompaction = true
		}
	}
	if !foundCompaction {
		t.Fatal("overflow must force a compaction before the retry")
	}
	if loop.Compaction.fitFailures != 0 {
		t.Fatalf("successful retry must reset fit failures, got %d", loop.Compaction.fitFailures)
	}
}

func TestContextOverflowTwiceTerminates(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Error: "maximum context length exceeded"},
		fakes.ScriptEntry{Error: "maximum context length exceeded"},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}

	err := loop.Execute(context.Background())
	if err == nil || !strings.Contains(err.Error(), "twice in a row") {
		t.Fatalf("Execute() error = %v, want double-overflow termination", err)
	}
	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("outcome = %s, want budget_exhausted", run.State.Outcome)
	}
}

// --- Goal primitive (P2) ---

func TestGoalContinuationAndCompletion(t *testing.T) {
	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"objective":"fix all tests"}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "progress", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"status":"complete"}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: registry, GoalCell: cell, Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if run.Goal == nil || run.Goal.Status != domain.GoalStatusComplete || run.Goal.Objective != "fix all tests" {
		t.Fatalf("goal = %+v, want completed objective", run.Goal)
	}
	continuationFound := false
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && strings.Contains(strings.Join(msg.TextParts(), ""), "Continue working toward the active goal") {
			continuationFound = true
		}
	}
	if !continuationFound {
		t.Fatal("active goal must inject a continuation prompt at end of turn")
	}
	goalEvents := 0
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventGoalUpdated {
			goalEvents++
		}
	}
	if goalEvents < 2 {
		t.Fatalf("goal.updated events = %d, want >= 2 (activate + complete)", goalEvents)
	}
}

// TestGoalCompleteCarriesFinalSummary locks the "close with summary"
// contract: update_goal accepts objective together with status (the model's
// natural way to say "done, here is what was achieved"), and the summary is
// recorded on the goal — previously rejected as mutually exclusive.
func TestGoalCompleteCarriesFinalSummary(t *testing.T) {
	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"objective":"initial goal"}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "progress", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"objective":"achieved: all packs shipped","status":"complete"}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: registry, GoalCell: cell, Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.Goal == nil || run.Goal.Status != domain.GoalStatusComplete {
		t.Fatalf("goal = %+v, want completed", run.Goal)
	}
	if run.Goal.Objective != "achieved: all packs shipped" {
		t.Fatalf("goal objective = %q, want the closing summary recorded", run.Goal.Objective)
	}
}

func TestGoalBudgetSoftLanding(t *testing.T) {
	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"objective":"big refactor","token_budget":10}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "working", StopReason: domain.StopEndTurn, UsageIn: 8, UsageOut: 4},
		fakes.ScriptEntry{Text: "wrapping up", StopReason: domain.StopEndTurn, UsageIn: 2, UsageOut: 2},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: registry, GoalCell: cell, Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.Goal == nil || run.Goal.Status != domain.GoalStatusBudgetLimited {
		t.Fatalf("goal = %+v, want budget_limited", run.Goal)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded (soft landing, not a budget kill)", run.State.Outcome)
	}
	wrapUpFound := false
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && strings.Contains(strings.Join(msg.TextParts(), ""), "budget_limited") {
			wrapUpFound = true
		}
	}
	if !wrapUpFound {
		t.Fatal("budget exhaustion must inject one wrap-up turn, not terminate mid-work")
	}
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want 3 (set, work, wrap-up)", calls)
	}
}

// Regression (REVIEW M1): the wrap-up prompt tells the model it may call
// update_goal with status "complete" when the goal is actually done, but
// drainGoalUpdates only applied Close while the goal was Active — against a
// budget_limited goal the close was silently dropped even though the tool
// result reported success.
func TestGoalBudgetLimitedCanBeCompleted(t *testing.T) {
	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"objective":"big refactor","token_budget":10}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "working", StopReason: domain.StopEndTurn, UsageIn: 8, UsageOut: 4},
		// The wrap-up turn: the model notices the work is actually done and
		// closes the goal, as the budget-limit prompt invites it to.
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "update_goal",
			Arguments: json.RawMessage(`{"status":"complete"}`),
		}}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, Model: model, Registry: registry, GoalCell: cell, Logger: slog.Default()}

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.Goal == nil || run.Goal.Status != domain.GoalStatusComplete {
		t.Fatalf("goal = %+v, want complete (a budget_limited goal must be closable)", run.Goal)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
}

// Regression (REVIEW R10): the update_goal approval description truncated
// the objective at byte 60, which could split a multi-byte rune.
func TestUpdateGoalApprovalDescTruncatesAtRuneBoundary(t *testing.T) {
	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	// 61 runes / 65 bytes: the old byte cut at 60 splits the first 中.
	objective := strings.Repeat("a", 59) + "中中"
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "update_goal",
		Arguments: json.RawMessage(`{"objective":"` + objective + `"}`),
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if !utf8.ValidString(prepared.ApprovalDesc) {
		t.Fatalf("approval desc is not valid UTF-8: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "…") {
		t.Fatalf("approval desc should mark truncation: %q", prepared.ApprovalDesc)
	}
}

func TestUpdateGoalToolValidation(t *testing.T) {
	for _, raw := range []string{
		`{}`,
		`{"status":"active"}`,
		`{"objective":"x","token_budget":-5}`,
		`{"status":"complete","token_budget":1000}`,
	} {
		if _, err := decodeUpdateGoalArgs(json.RawMessage(raw)); err == nil {
			t.Fatalf("decodeUpdateGoalArgs(%s) must fail", raw)
		}
	}
	args, err := decodeUpdateGoalArgs(json.RawMessage(`{"objective":" ship it ","token_budget":1000}`))
	if err != nil {
		t.Fatalf("decodeUpdateGoalArgs(valid) error = %v", err)
	}
	if args.Objective != "ship it" || args.TokenBudget != 1000 {
		t.Fatalf("parsed args = %+v", args)
	}

	// Closing may carry a final objective summary (the model's natural
	// "done, here is what was achieved" — previously rejected as
	// mutually exclusive).
	args, err = decodeUpdateGoalArgs(json.RawMessage(`{"objective":"finished the pack work","status":"complete"}`))
	if err != nil {
		t.Fatalf("decodeUpdateGoalArgs(complete+objective) error = %v", err)
	}
	if args.Objective != "finished the pack work" || args.Status != "complete" {
		t.Fatalf("parsed close args = %+v", args)
	}

	cell := NewGoalCell()
	tool, err := NewUpdateGoalTool(cell)
	if err != nil {
		t.Fatalf("NewUpdateGoalTool: %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "update_goal",
		Arguments: json.RawMessage(`{"objective":"ship it"}`),
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if prepared.Risk != domain.R1 || prepared.ArgsHash == "" {
		t.Fatalf("prepared = %+v, want R1 with args hash", prepared)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s", result.Status)
	}
	update, ok := cell.Take()
	if !ok || update.Objective != "ship it" {
		t.Fatalf("cell update = %+v, ok=%v", update, ok)
	}
}

func TestRunSuccessScore(t *testing.T) {
	liveCtx := context.Background()
	canceledCtx, cancel := context.WithCancel(context.Background())
	cancel()

	t.Run("bare cancellation is not scored", func(t *testing.T) {
		if _, _, scored := runSuccessScore(liveCtx, context.Canceled, domain.OutcomeCancelled); scored {
			t.Fatal("context.Canceled must not be scored")
		}
	})
	t.Run("wrapped cancellation is not scored", func(t *testing.T) {
		err := fmt.Errorf("model stream consumption: %w", context.Canceled)
		if _, _, scored := runSuccessScore(liveCtx, err, domain.OutcomeFailed); scored {
			t.Fatal("wrapped context.Canceled must not be scored even with failed outcome")
		}
	})
	t.Run("chain-less cancel error with cancelled ctx is not scored", func(t *testing.T) {
		// The provider stream degrades errors to strings, so a mid-stream
		// Ctrl+C resurfaces as errors.New("context canceled") — no chain.
		// The run context's own state is the fallback signal.
		err := errors.New("model stream consumption: context canceled")
		if _, _, scored := runSuccessScore(canceledCtx, err, domain.OutcomeFailed); scored {
			t.Fatal("cancelled run context must not be scored even with a chain-less error")
		}
	})
	t.Run("cancelled ctx with nil error is not scored", func(t *testing.T) {
		if _, _, scored := runSuccessScore(canceledCtx, nil, domain.OutcomeCancelled); scored {
			t.Fatal("cancelled run context must not be scored")
		}
	})
	t.Run("real errors score zero", func(t *testing.T) {
		value, comment, scored := runSuccessScore(liveCtx, errors.New("provider 500"), domain.OutcomeFailed)
		if !scored || value != 0 || comment != "provider 500" {
			t.Fatalf("got (%v, %q, %v)", value, comment, scored)
		}
	})
	t.Run("budget exhaustion scores zero", func(t *testing.T) {
		if value, _, scored := runSuccessScore(liveCtx, nil, domain.OutcomeBudgetExhausted); !scored || value != 0 {
			t.Fatalf("got (%v, %v)", value, scored)
		}
	})
	t.Run("success outcomes score one", func(t *testing.T) {
		for _, outcome := range []domain.Outcome{domain.OutcomeSucceeded, domain.OutcomeCompletedUnverified} {
			if value, _, scored := runSuccessScore(liveCtx, nil, outcome); !scored || value != 1 {
				t.Fatalf("outcome %s: got (%v, %v)", outcome, value, scored)
			}
		}
	})
}

func TestToolResultTracePreviewKeepsValidUTF8(t *testing.T) {
	// The 500-byte preview cut lands inside a multi-byte character: the
	// excerpt must stay valid UTF-8 (OTLP protobuf rejects invalid strings)
	// and must end on a complete rune — no replacement char mojibake.
	body := strings.Repeat("a", 490) + "完整的技能正文内容" + strings.Repeat("b", 100)
	result := domain.ToolResult{
		Content: []domain.ContentPart{{Kind: domain.PartText, Text: body}},
	}
	preview := toolResultTracePreview(result, 500)
	if !utf8.ValidString(preview) {
		t.Fatalf("preview is not valid UTF-8: %q", preview[len(preview)-20:])
	}
	if !strings.HasSuffix(preview, "…") {
		t.Fatalf("preview must carry the truncation marker: %q", preview[len(preview)-10:])
	}
	if strings.Contains(preview, "�") {
		t.Fatalf("preview must cut at a rune boundary, not substitute: %q", preview[len(preview)-20:])
	}
}

// slowSafeTool is a concurrent-safe test tool with a scripted execution
// delay — the wall-clock witness for parallel batch execution.
type slowSafeTool struct {
	def   domain.ToolDefinition
	delay time.Duration
}

func (t *slowSafeTool) Definition() domain.ToolDefinition { return t.def }
func (t *slowSafeTool) ConcurrentSafe() bool              { return true }

func (t *slowSafeTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	sum := sha256.Sum256(call.Arguments)
	return domain.PreparedCall{
		Call: call, Definition: t.def, Risk: domain.R1,
		ArgsHash: hex.EncodeToString(sum[:])[:16],
	}, nil
}

func (t *slowSafeTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	started := time.Now()
	time.Sleep(t.delay)
	return domain.ToolResult{
		CallID: prepared.Call.ID, Status: domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: "ok"}},
		StartedAt:  started,
		FinishedAt: time.Now(),
	}
}

func TestSegmentBatchSplitsOnSafety(t *testing.T) {
	safe := &slowSafeTool{def: newTestToolDefinition("read_file", []domain.Capability{domain.CapFSRead})}
	unsafe := fakes.EchoTool() // FakeTool does not opt into concurrency
	exec := func(tool domain.Tool) preparedExec {
		return preparedExec{tool: tool}
	}
	segments := segmentBatch([]preparedExec{
		exec(safe), exec(unsafe), exec(safe), exec(safe), exec(unsafe), exec(safe),
	})
	var sizes []int
	for _, seg := range segments {
		sizes = append(sizes, len(seg))
	}
	want := []int{1, 1, 2, 1, 1}
	if len(sizes) != len(want) {
		t.Fatalf("segments = %v, want %v", sizes, want)
	}
	for i := range want {
		if sizes[i] != want[i] {
			t.Fatalf("segments = %v, want %v", sizes, want)
		}
	}
}

func TestLoopExecutesConcurrentSafeBatchInParallel(t *testing.T) {
	toolA := &slowSafeTool{def: newTestToolDefinition("read_file", []domain.Capability{domain.CapFSRead}), delay: 200 * time.Millisecond}
	toolB := &slowSafeTool{def: newTestToolDefinition("list_dir", []domain.Capability{domain.CapFSRead}), delay: 200 * time.Millisecond}
	callA := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	callB := domain.ToolCall{ID: domain.NewToolCallID(), Name: "list_dir", Arguments: json.RawMessage(`{"path":"dir_b"}`)}

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{callA, callB},
			StopReason: domain.StopToolUse,
			UsageIn:    100, UsageOut: 30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	for _, tool := range []domain.Tool{toolA, toolB} {
		if err := registry.Register(tool); err != nil {
			t.Fatalf("Register: %v", err)
		}
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read both"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{Run: run, Model: model, Registry: registry, Logger: slog.Default()}

	start := time.Now()
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	elapsed := time.Since(start)
	// Serial execution would cost >= 400ms of scripted delay.
	if elapsed > 350*time.Millisecond {
		t.Fatalf("two 200ms executions took %v — not parallel", elapsed)
	}

	// Results land in call order (deterministic recording), and every
	// started event precedes every result — the parallel durability
	// invariant.
	var resultIDs []domain.ToolCallID
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				resultIDs = append(resultIDs, part.ToolResult.CallID)
			}
		}
	}
	if len(resultIDs) != 2 || resultIDs[0] != callA.ID || resultIDs[1] != callB.ID {
		t.Fatalf("result order = %v, want [A B]", resultIDs)
	}
	lastStarted, firstResult := -1, len(run.PendingEvents())
	for i, evt := range run.PendingEvents() {
		switch evt.Type {
		case domain.EventToolExecutionStarted:
			lastStarted = i
		case domain.EventToolResultAdded:
			if i < firstResult {
				firstResult = i
			}
		}
	}
	if lastStarted < 0 || firstResult == len(run.PendingEvents()) || lastStarted > firstResult {
		t.Fatalf("started/result event order violated: lastStarted=%d firstResult=%d", lastStarted, firstResult)
	}
}

func TestLoopMixedBatchSerializesUnsafeCalls(t *testing.T) {
	safe := &slowSafeTool{def: newTestToolDefinition("read_file", []domain.Capability{domain.CapFSRead}), delay: 50 * time.Millisecond}
	unsafe := fakes.EchoTool()
	callSafe1 := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"a"}`)}
	callUnsafe := domain.ToolCall{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"x"}`)}
	callSafe2 := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"b"}`)}

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{callSafe1, callUnsafe, callSafe2},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	for _, tool := range []domain.Tool{safe, unsafe} {
		if err := registry.Register(tool); err != nil {
			t.Fatalf("Register: %v", err)
		}
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "mixed batch"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{Run: run, Model: model, Registry: registry, Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	var resultIDs []domain.ToolCallID
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				resultIDs = append(resultIDs, part.ToolResult.CallID)
			}
		}
	}
	if len(resultIDs) != 3 || resultIDs[0] != callSafe1.ID || resultIDs[1] != callUnsafe.ID || resultIDs[2] != callSafe2.ID {
		t.Fatalf("mixed batch result order = %v, want strict call order", resultIDs)
	}
}

func TestLoopMaxOutputSalvageTurn(t *testing.T) {
	// Two consecutive output-cap truncations arm the salvage wrap-up: the
	// run ends with a conclusion (OutcomeCompletedUnverified) instead of
	// failing on a third paid truncation (docs/SUBAGENT_DESIGN.md §12).
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "第一部分（被截断）", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		fakes.ScriptEntry{Text: "第二部分（仍被截断）", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		fakes.ScriptEntry{Text: "简要结论。", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 50},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "research"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeCompletedUnverified {
		t.Fatalf("outcome = %s, want completed_unverified (salvage landing)", run.State.Outcome)
	}
	if got := LastAssistantText(run.Messages); got != "简要结论。" {
		t.Fatalf("final answer = %q, want the salvage-turn conclusion", got)
	}
	var wrapUpSeen, promptSeen bool
	for _, evt := range run.PendingEvents() {
		if evt.Type == domain.EventBudgetWrapupStarted && strings.Contains(string(evt.Payload), `"max_output"`) {
			wrapUpSeen = true
		}
	}
	for _, msg := range run.Messages {
		if msg.Role == domain.RoleUser && strings.Contains(strings.Join(msg.TextParts(), ""), "output token limit") {
			promptSeen = true
		}
	}
	if !wrapUpSeen || !promptSeen {
		t.Fatalf("salvage wrap-up not armed: event=%v prompt=%v", wrapUpSeen, promptSeen)
	}
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want 3 (two truncations + one salvage)", calls)
	}
}

func TestLoopMaxOutputSalvageTurnItselfCapped(t *testing.T) {
	// If even the salvage turn overflows, the run lands with whatever text
	// it has — it must NOT loop forever on a persistently verbose model.
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "第一部分", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		fakes.ScriptEntry{Text: "第二部分", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		fakes.ScriptEntry{Text: "补救也写不完", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "research"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeCompletedUnverified {
		t.Fatalf("outcome = %s, want completed_unverified", run.State.Outcome)
	}
	if got := LastAssistantText(run.Messages); got != "补救也写不完" {
		t.Fatalf("final answer = %q, want the capped salvage text", got)
	}
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want exactly 3 (no infinite salvage loop)", calls)
	}
}

// Output-cap truncations only arm the salvage wrap-up when they are
// CONSECUTIVE: a normal turn in between resets the streak (REVIEW H15).
func TestLoopMaxOutputStreakResetsOnNormalTurn(t *testing.T) {
	toolTurn := fakes.ScriptEntry{
		ToolCalls: []domain.ToolCall{{
			ID: domain.NewToolCallID(), Name: "read_file",
			Arguments: json.RawMessage(`{"path":"a.go"}`),
		}},
		StopReason: domain.StopToolUse,
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "第一部分", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		toolTurn, // a normal tool turn must break the truncation streak
		fakes.ScriptEntry{Text: "第二部分", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 50},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "research"}},
		CreatedAt: time.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register: %v", err)
	}
	loop := &Loop{
		Run: run, Model: model, Registry: registry, Logger: slog.Default(),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded (streak was broken by the tool turn)", run.State.Outcome)
	}
	for _, msg := range run.Messages {
		if strings.Contains(strings.Join(msg.TextParts(), ""), "output token limit") {
			t.Fatal("non-consecutive truncations must not arm the salvage wrap-up")
		}
	}
	if calls := len(model.Calls()); calls != 4 {
		t.Fatalf("model calls = %d, want 4", calls)
	}
}

// A goal-token wrap-up turn that overflows the output cap must land with
// whatever text it produced — the goal wrap-up used to fall through every
// guard and could loop forever (REVIEW H15).
func TestLoopGoalWrapUpTurnOverflowStillLands(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "工作完成", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 50},
		fakes.ScriptEntry{Text: "总结写到一半被截断", StopReason: domain.StopMaxOutput, UsageIn: 100, UsageOut: 400},
	)
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	run.Goal = &domain.Goal{
		Objective: "finish the task", TokenBudget: 10, TokensUsed: 10,
		Status: domain.GoalStatusActive, CreatedAt: time.Now(), UpdatedAt: time.Now(),
	}
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded (goal wrap-up landing)", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 2 {
		t.Fatalf("model calls = %d, want exactly 2 (no wrap-up loop)", calls)
	}
}

// An unrecognized stop reason gets a bounded number of retries, then the
// run fails instead of re-asking the model forever (REVIEW H15).
func TestLoopUnknownStopReasonRetriesBounded(t *testing.T) {
	entry := fakes.ScriptEntry{Text: "……", StopReason: domain.StopReason("vendor_custom"), UsageIn: 100, UsageOut: 50}
	model := fakes.NewFakeModel(entry, entry, entry, entry, entry)
	run := newTestRun(domain.Limits{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{Run: run, Model: model, Registry: NewToolRegistry(), Logger: slog.Default()}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed (unrecognized stop reason)", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != maxUnknownStopRetries {
		t.Fatalf("model calls = %d, want %d (bounded retries)", calls, maxUnknownStopRetries)
	}
}

func TestLoopFoldsExternalToolUsageIntoBudget(t *testing.T) {
	// delegate_task's contract: a tool reports externally-metered tokens
	// (a sub-agent run's consumption) in its result metadata, and the
	// loop folds them into the run budget alongside its own model calls
	// (docs/SUBAGENT_DESIGN.md §5.2).
	delegatingTool := fakes.NewFakeTool(
		domain.ToolDefinition{
			Name:         "read_file",
			Description:  "Read file contents",
			InputSchema:  json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
			Capabilities: []domain.Capability{domain.CapFSRead},
			Source:       domain.ToolSourceBuiltin,
		},
		domain.ToolResult{
			Status:  domain.ToolStatusSuccess,
			Content: []domain.ContentPart{{Kind: domain.PartText, Text: "conclusion"}},
			Metadata: map[string]string{
				domain.ToolMetaExternalInputTokens:  "500",
				domain.ToolMetaExternalOutputTokens: "120",
			},
			StartedAt:  time.Now(),
			FinishedAt: time.Now(),
		},
	)
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"x.go"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	if err := registry.Register(delegatingTool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "delegate this"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run:      run,
		Model:    model,
		Registry: registry,
		Logger:   slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	// Model-metered 100/30 + externally-reported 500/120.
	if run.Usage.InputTokens != 600 || run.Usage.OutputTokens != 150 {
		t.Fatalf("usage = %d/%d, want 600/150 (model + folded external)",
			run.Usage.InputTokens, run.Usage.OutputTokens)
	}
}

// Regression: a reverse proxy in front of the provider (an internal
// openresty gateway) rejects oversized request bodies with HTTP 413 long
// before token occupancy trips the model's context window. The loop must
// read that as a fit failure (forced compaction + retry), not a hard
// error — otherwise every subsequent turn fails identically because the
// failed request stays in the transcript.
func TestIsContextOverflowErrorClassifiesGateway413(t *testing.T) {
	t.Parallel()

	// Typed path: providers wrap httpc.StatusError with %w, the loop wraps
	// again ("model stream: %w") — errors.As must see through both.
	typed := fmt.Errorf("model stream: %w",
		fmt.Errorf("anthropic provider: %w",
			&httpc.StatusError{Code: http.StatusRequestEntityTooLarge, Status: "413 Request Entity Too Large"}))
	if !isRequestTooLargeError(typed) {
		t.Fatal("typed 413 StatusError must classify as request-too-large")
	}
	if !isContextOverflowError(typed) {
		t.Fatal("typed 413 StatusError must classify as a fit failure")
	}

	// String-degraded path: mid-stream errors lose the error chain, so
	// the nginx/openresty phrasing must match by needle.
	degraded := errors.New("anthropic provider: HTTP 413 413 Payload Too Large: <html><head><title>413 Request Entity Too Large</title></head></html>")
	if !isContextOverflowError(degraded) {
		t.Fatal("string-degraded 413 (payload too large) must classify as a fit failure")
	}
}

func TestIsContextOverflowErrorIgnoresOtherFailures(t *testing.T) {
	t.Parallel()

	for _, code := range []int{400, 401, 403, 404, 429, 500, 502} {
		err := fmt.Errorf("openai provider: %w", &httpc.StatusError{Code: code, Status: fmt.Sprintf("%d Some Status", code)})
		if isRequestTooLargeError(err) {
			t.Fatalf("status %d must not classify as request-too-large", code)
		}
		if isContextOverflowError(err) {
			t.Fatalf("status %d without overflow wording must not trigger compaction", code)
		}
	}
	if isContextOverflowError(nil) || isRequestTooLargeError(nil) {
		t.Fatal("nil error must not classify")
	}
	// A semantic provider overflow (400 with window wording) still
	// classifies via the needles.
	if !isContextOverflowError(errors.New("openai provider: HTTP 400: prompt is too long")) {
		t.Fatal("semantic context overflow must still classify")
	}
	// Gateways that meter raw prompt length (including inline base64
	// images) reject with length phrasing, not window phrasing — observed
	// verbatim from aigc. It must engage the compaction+retry path.
	for _, msg := range []string{
		"Prompt exceeds max length",
		"prompt exceeds max length",
		"input exceeds the maximum length",
	} {
		if !isContextOverflowError(errors.New(msg)) {
			t.Fatalf("length-phrased rejection %q must classify as a fit failure", msg)
		}
	}
}

func TestHandleContextOverflow413ForcesCompactionThenTerminates(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	if _, err := run.TransitionTo(domain.PhaseCallingModel); err != nil {
		t.Fatalf("TransitionTo: %v", err)
	}
	loop := &Loop{Run: run}
	cause := fmt.Errorf("anthropic provider: %w",
		&httpc.StatusError{Code: http.StatusRequestEntityTooLarge, Status: "413 Request Entity Too Large"})

	if err := loop.handleContextOverflow(context.Background(), cause); err != nil {
		t.Fatalf("first 413 = %v, want forced compaction instead of an error", err)
	}
	if !loop.Compaction.Force {
		t.Fatal("first 413 must arm ForceCompact")
	}

	// The retry goes through a fresh model call (PhaseCallingModel) before
	// the second rejection arrives.
	if _, err := run.TransitionTo(domain.PhaseCallingModel); err != nil {
		t.Fatalf("TransitionTo: %v", err)
	}
	err := loop.handleContextOverflow(context.Background(), cause)
	if err == nil || !strings.Contains(err.Error(), "HTTP 413") {
		t.Fatalf("second 413 = %v, want a 413-specific terminal message", err)
	}
	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("outcome = %s, want budget_exhausted", run.State.Outcome)
	}

	// A semantic overflow keeps the context-window wording.
	run2 := newTestRun(domain.DefaultLimits())
	if _, err := run2.TransitionTo(domain.PhaseCallingModel); err != nil {
		t.Fatalf("TransitionTo: %v", err)
	}
	loop2 := &Loop{Run: run2}
	if err := loop2.handleContextOverflow(context.Background(), errors.New("prompt is too long")); err != nil {
		t.Fatalf("first overflow = %v, want forced compaction", err)
	}
	if _, err := run2.TransitionTo(domain.PhaseCallingModel); err != nil {
		t.Fatalf("TransitionTo: %v", err)
	}
	err = loop2.handleContextOverflow(context.Background(), errors.New("prompt is too long"))
	if err == nil || !strings.Contains(err.Error(), "context size twice") {
		t.Fatalf("second overflow = %v, want the context-window terminal message", err)
	}
}

// Regression: the trace output of a failed run must not inherit the
// previous run's reply from the shared session transcript (observed in
// Langfuse: four consecutive 413-failed runs all reported an earlier
// successful run's answer as their output).
func TestRunAssistantTextScopesToCurrentRun(t *testing.T) {
	t.Parallel()
	run := newTestRun(domain.DefaultLimits())

	prior := textMessage(domain.RoleAssistant, "上一轮的答复")
	prior.Metadata = map[string]string{"run_id": domain.NewRunID().String()}
	run.Messages = append(
		run.Messages,
		textMessage(domain.RoleAssistant, "更老的答复"), // legacy, unstamped
		prior,
		textMessage(domain.RoleUser, "再试一次"),
	)
	if got := runAssistantText(run.Messages, run.ID); got != "" {
		t.Fatalf("failed run must report empty output, got %q", got)
	}

	// Once this run produces a reply it becomes the trace output.
	run.AddAssistantMessage(textMessage(domain.RoleAssistant, "本轮的答复"))
	if got := runAssistantText(run.Messages, run.ID); got != "本轮的答复" {
		t.Fatalf("output = %q, want the current run's reply", got)
	}

	// A text-less assistant message (pure tool call) does not shadow the
	// reply.
	run.AddAssistantMessage(domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{
			ID: domain.NewToolCallID(), Name: "run_cmd",
		}}},
		CreatedAt: time.Now(),
	})
	if got := runAssistantText(run.Messages, run.ID); got != "本轮的答复" {
		t.Fatalf("output = %q, want to skip empty assistant text", got)
	}
}

func TestActionablePrepareErrorGuidesWorkspaceEscape(t *testing.T) {
	escapeErr := domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid",
		domain.WithCause(fmt.Errorf("path %q escapes workspace root %q", "/outside", "/ws")))
	fileToolCall := domain.ToolCall{ID: domain.NewToolCallID(), Name: "list_dir"}
	msg := actionablePrepareError(fileToolCall, escapeErr)
	if !strings.Contains(msg, escapeErr.Error()) {
		t.Fatalf("guidance must keep the original error, got %q", msg)
	}
	if !strings.Contains(msg, "restricted to the workspace root") || !strings.Contains(msg, "run_cmd") {
		t.Fatalf("guidance missing actionable advice, got %q", msg)
	}

	// run_cmd gets its own wording: "use run_cmd instead" would be circular.
	// Its working_dir is workspace-scoped while the command body may
	// reference external paths.
	runCmdCall := domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd"}
	workingDirErr := domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid",
		domain.WithCause(fmt.Errorf("path %q escapes workspace root %q", "/outside", "/ws")))
	msg = actionablePrepareError(runCmdCall, workingDirErr)
	if !strings.Contains(msg, "working_dir must stay inside the workspace") {
		t.Fatalf("run_cmd guidance missing working_dir advice, got %q", msg)
	}
	if strings.Contains(msg, "use run_cmd instead") {
		t.Fatalf("run_cmd guidance must not suggest run_cmd itself, got %q", msg)
	}

	// Non-security failures and unrelated security failures stay verbatim.
	if got := actionablePrepareError(fileToolCall, errors.New("boom")); got != "boom" {
		t.Fatalf("plain error changed: %q", got)
	}
	other := domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	if got := actionablePrepareError(fileToolCall, other); got != other.Error() {
		t.Fatalf("unrelated security error changed: %q", got)
	}
}

func TestPrepareFailureArgsSummary(t *testing.T) {
	t.Run("invalid JSON yields nil", func(t *testing.T) {
		if got := prepareFailureArgsSummary(json.RawMessage(`{bad`)); got != nil {
			t.Fatalf("got %v, want nil", got)
		}
	})
	t.Run("no whitelisted keys yields nil", func(t *testing.T) {
		if got := prepareFailureArgsSummary(json.RawMessage(`{"content":"x","limit":1}`)); got != nil {
			t.Fatalf("got %v, want nil", got)
		}
	})
	t.Run("whitelisted strings extracted, rest dropped", func(t *testing.T) {
		got := prepareFailureArgsSummary(json.RawMessage(
			`{"path":"go/pl/loom/internal/config/example.go","pattern":"storage","content":"secret-body","env":{"K":"V"},"limit":50}`,
		))
		if got["path"] != "go/pl/loom/internal/config/example.go" || got["pattern"] != "storage" {
			t.Fatalf("whitelisted keys missing: %v", got)
		}
		for _, key := range []string{"content", "env", "limit"} {
			if _, ok := got[key]; ok {
				t.Fatalf("key %q must not be persisted: %v", key, got)
			}
		}
	})
	t.Run("non-string whitelisted values skipped", func(t *testing.T) {
		got := prepareFailureArgsSummary(json.RawMessage(`{"path":42,"type":["go"]}`))
		if got != nil {
			t.Fatalf("got %v, want nil", got)
		}
	})
	t.Run("long values truncated", func(t *testing.T) {
		long := strings.Repeat("a", 300)
		got := prepareFailureArgsSummary(json.RawMessage(`{"path":"` + long + `"}`))
		if len(got["path"]) != 203 || !strings.HasSuffix(got["path"], "...") {
			t.Fatalf("value not truncated to 200+marker: %q", got["path"])
		}
	})
}

func TestAppendPrepareFailureEventsIncludesArgsSummary(t *testing.T) {
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	loop := &Loop{Run: run}
	tc := domain.ToolCall{
		ID:   domain.NewToolCallID(),
		Name: "search",
		Arguments: json.RawMessage(
			`{"path":"go/pl/loom/internal/config/example.go","pattern":"storage","content":"secret-body"}`,
		),
	}
	loop.appendPrepareFailureEvents(tc, "deadbeef")

	if len(run.pendingEvents) != 2 {
		t.Fatalf("pending events = %d, want prepared+started pair", len(run.pendingEvents))
	}
	var payload toolCallAuditPayload
	if err := json.Unmarshal(run.pendingEvents[0].Payload, &payload); err != nil {
		t.Fatalf("unmarshal prepared payload: %v", err)
	}
	if !payload.PrepareFailed || payload.ArgsRawHash != "deadbeef" {
		t.Fatalf("degraded payload = %+v", payload)
	}
	if payload.ArgsSummary["path"] != "go/pl/loom/internal/config/example.go" ||
		payload.ArgsSummary["pattern"] != "storage" {
		t.Fatalf("args summary = %v, want path+pattern", payload.ArgsSummary)
	}
	if _, ok := payload.ArgsSummary["content"]; ok {
		t.Fatalf("content must not be persisted: %v", payload.ArgsSummary)
	}
}

// aggregateStream preserves the former aggregation helper for tests while
// delegating validation to StreamAggregator.
func aggregateStream(stream domain.ModelStream, clock domain.Clock) (streamResponse, error) {
	agg := NewStreamAggregator(clock, StreamHooks{})
	if err := consumeStream(stream, agg); err != nil {
		response := streamResponse{}
		if agg.HasPartialContent() {
			response.Message = agg.InterruptedMessage()
		}
		return response, err
	}
	message, stop, inputTokens, outputTokens, err := agg.Finalize()
	if err != nil {
		response := streamResponse{}
		if agg.HasPartialContent() {
			response.Message = agg.InterruptedMessage()
		}
		return response, err
	}
	return streamResponse{Message: message, StopReason: stop, InputTokens: inputTokens, OutputTokens: outputTokens}, nil
}
