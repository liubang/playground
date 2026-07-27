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
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"

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
		len(run.Messages) != 1 {
		t.Fatalf("unexpected continued run: %+v pending=%+v", run, run.pendingEvents)
	}
	// A continuation starts a fresh per-prompt budget window: the
	// checkpoint's cumulative usage must not throttle the new prompt.
	if run.Usage != (domain.Usage{}) {
		t.Fatalf("continued run usage = %+v, want zeroed budget window", run.Usage)
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "read test.go"}},
		CreatedAt: time.Now(),
	})

	type contextSample struct {
		est       int
		lastInput int64
	}
	var samples []contextSample
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		StreamHooks: StreamHooks{
			OnContextUsage: func(estTokens int, lastCallInputTokens int64) {
				samples = append(samples, contextSample{est: estTokens, lastInput: lastCallInputTokens})
			},
		},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error: %v", err)
	}

	// Expect at least three reports: after the tool-call response, after the
	// tool batch, and after the final response.
	if len(samples) < 3 {
		t.Fatalf("context usage reports = %d, want ≥ 3: %+v", len(samples), samples)
	}
	if samples[0].est <= 0 || samples[0].lastInput != 100 {
		t.Fatalf("first report = %+v, want est>0 and provider input 100", samples[0])
	}
	if samples[1].lastInput != 100 {
		t.Fatalf("tool-batch report = %+v, want carried lastCallInput 100", samples[1])
	}
	last := samples[len(samples)-1]
	if last.lastInput != 200 || last.est <= 0 {
		t.Fatalf("final report = %+v, want est>0 and provider input 200", last)
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
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
		ContextWindow: 100,
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
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
	run := newTestRun(domain.Limits{MaxTurns: 10, MaxToolCalls: 10, MaxOutputTokens: 4096})
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

func (p fixedPolicy) Evaluate(domain.PreparedCall) domain.Decision { return domain.Decision(p) }

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
	loop := &Loop{Run: run, ContextWindow: 100}

	loop.lastCallInput = 85 // ≥80% of the window
	if !loop.shouldCompact() {
		t.Fatal("occupancy at 85% of window should trigger compaction")
	}
	loop.lastCallInput = 50
	if loop.shouldCompact() {
		t.Fatal("occupancy at 50% of window must not trigger compaction")
	}
	run.Messages = append(run.Messages, toolResultMessage(bigOutput(200_000)))
	if !loop.shouldCompact() {
		t.Fatal("estimate above TargetTokens should trigger compaction regardless of occupancy")
	}
	run.Messages = nil
	loop.forceCompact = true
	if !loop.shouldCompact() {
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
	loop := &Loop{Run: run, ContextWindow: 100}

	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: strings.Repeat("x", 400)}},
		CreatedAt: time.Now(),
	})
	if !loop.shouldCompact() {
		t.Fatal("transcript at the occupancy line should trigger compaction")
	}

	// Simulate a no-progress pass: the condenser left the transcript unchanged.
	loop.lastCompactEst = estTokens(run.Messages)
	if loop.shouldCompact() {
		t.Fatal("compaction must not retrigger while the transcript has not grown")
	}
	// Metered occupancy pressure alone must not bypass the guard: another
	// pass over the same transcript cannot help; the run must proceed and
	// let the provider accept or reject the request.
	loop.lastCallInput = 95
	if loop.shouldCompact() {
		t.Fatal("metered occupancy must not retrigger compaction without transcript growth")
	}

	// A provider context-overflow rejection still forces a pass.
	loop.forceCompact = true
	if !loop.shouldCompact() {
		t.Fatal("forceCompact must bypass the no-growth guard")
	}
	loop.forceCompact = false
	loop.lastCallInput = 0

	// Real transcript growth re-arms compaction.
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "more context here"}},
		CreatedAt: time.Now(),
	})
	if !loop.shouldCompact() {
		t.Fatal("transcript growth past the last pass must re-arm compaction")
	}
}

func TestBudgetNoticeInjection(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run, ContextWindow: 100}

	loop.lastCallInput = 85
	loop.maybeInjectBudgetNotice()
	if len(run.Messages) != 1 || run.Messages[0].Role != domain.RoleSystem ||
		!strings.Contains(run.Messages[0].TextParts()[0], "remain before auto-compaction") {
		t.Fatalf("80%% reminder missing: %+v", run.Messages)
	}
	if loop.budgetNoticeLevel != 1 {
		t.Fatalf("budgetNoticeLevel = %d, want 1 after the 80%% reminder", loop.budgetNoticeLevel)
	}
	loop.lastCallInput = 95
	loop.maybeInjectBudgetNotice()
	if len(run.Messages) != 2 || !strings.Contains(run.Messages[1].TextParts()[0], "nearly full") {
		t.Fatalf("90%% self-handoff notice missing: %+v", run.Messages)
	}

	// Compaction re-arms the notices and resets the calibrated occupancy.
	run.State.Phase = domain.PhaseCompacting
	if err := loop.compact(context.Background()); err != nil {
		t.Fatalf("compact() error = %v", err)
	}
	if loop.budgetNoticeLevel != 0 || loop.lastCallInput != 0 {
		t.Fatalf("compact must re-arm notices and reset occupancy: level=%d lastCallInput=%d", loop.budgetNoticeLevel, loop.lastCallInput)
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
	if loop.compactFitFailures != 0 {
		t.Fatalf("successful retry must reset fit failures, got %d", loop.compactFitFailures)
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

func TestUpdateGoalToolValidation(t *testing.T) {
	for _, raw := range []string{
		`{}`,
		`{"status":"active"}`,
		`{"objective":"x","status":"complete"}`,
		`{"objective":"x","token_budget":-5}`,
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
