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
// Created: 2026/07/29

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// --- soft landing: auto-deny during the wrap-up turn (CONTEXT_DESIGN §4.4.2) ---

// A model that ignores the "no tools" wrap-up instruction gets every call
// denied outright (never routed to approval) and the run terminates with a
// fully paired transcript.
func TestBudgetWrapUpAutoDeniesToolCalls(t *testing.T) {
	readTool := fakes.ReadFileTool()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`),
			}},
			StopReason: domain.StopToolUse,
			UsageIn:    2_000_000, // prices the first call past the cost budget
		},
		// The wrap-up turn: the model tries another tool call anyway.
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"b.go"}`),
			}},
			StopReason: domain.StopToolUse,
		},
	)
	registry := NewToolRegistry()
	if err := registry.Register(readTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxEstimatedCostUSD: 1.0}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		CostInputUSDPerMTok: 1.0, // $1/MTok → first call costs $2 ≥ $1 budget
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}

	if run.State.Outcome != domain.OutcomeBudgetExhausted {
		t.Fatalf("outcome = %s, want budget_exhausted", run.State.Outcome)
	}
	// The wrap-up call was denied, never executed, and the transcript is paired.
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("wrap-up denial must keep the transcript paired: %+v", dangling)
	}
	if executed := len(readTool.ExecutedCalls()); executed != 1 {
		t.Fatalf("tool executions = %d, want 1 (the wrap-up call must not execute)", executed)
	}
	denied := false
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil &&
				part.ToolResult.Error != nil && strings.Contains(part.ToolResult.Error.Message, "budget wrap-up") {
				denied = true
			}
		}
	}
	if !denied {
		t.Fatal("wrap-up tool call must be denied with the wrap-up reason")
	}
}

// --- prepare_failed event pairing + malformed-arguments hint (§4.6) ---

func TestPrepareFailedKeepsEventStreamPaired(t *testing.T) {
	tool := fakes.ReadFileTool().WithPrepareFn(
		func(_ context.Context, _ domain.ToolCall) (domain.PreparedCall, error) {
			return domain.PreparedCall{}, errors.New(`json: unknown field "__malformed_arguments"`)
		},
	)
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID:   domain.NewToolCallID(),
				Name: "read_file",
				Arguments: json.RawMessage(
					// An empty embedded payload (the provider streamed no
					// arguments at all) declines the tolerant repair pass,
					// so read-only read_file still exercises the strict
					// interception path — repairable near-miss JSON now
					// routes into normal Prepare instead (see
					// TestLoopMalformedArgumentsRepairedForReadTools).
					`{"__malformed_arguments":"","error":"model emitted invalid arguments JSON; re-issue the tool call with valid arguments"}`,
				),
			}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "recovered", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	loop := &Loop{
		Run: run, Model: model,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}

	// The failed preparation still produced prepared + started events, so
	// consumers never see a completion without a start.
	var prepared, started, completed int
	for _, evt := range run.pendingEvents {
		switch evt.Type {
		case domain.EventToolCallPrepared:
			prepared++
			var payload toolCallAuditPayload
			if err := json.Unmarshal(evt.Payload, &payload); err != nil || !payload.PrepareFailed || payload.ArgsRawHash == "" {
				t.Fatalf("degraded prepared payload = %s", evt.Payload)
			}
		case domain.EventToolExecutionStarted:
			started++
		case domain.EventToolExecutionCompleted:
			completed++
		}
	}
	if prepared != 1 || started != 1 || completed != 1 {
		t.Fatalf("event pairing = %d/%d/%d, want 1/1/1", prepared, started, completed)
	}

	// The model saw the embedded hint, not the internal placeholder field.
	var resultText string
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.Error != nil {
				resultText = part.ToolResult.Error.Message
			}
		}
	}
	if !strings.Contains(resultText, "re-issue the tool call with valid arguments") {
		t.Fatalf("error must surface the embedded hint, got %q", resultText)
	}
	if strings.Contains(resultText, "unknown field") {
		t.Fatalf("internal placeholder field name leaked to the model: %q", resultText)
	}
}

// --- unknown-field guidance surfacing (§4.6) ---

func TestUnknownFieldGuidanceReachesModel(t *testing.T) {
	// The chronic cross-toolkit inheritance case from live transcripts:
	// the model invents max_output_tokens for run_cmd. The rejection must
	// name the closest valid field so the next attempt self-corrects.
	type runCmdArgs struct {
		Command        string `json:"command"`
		MaxOutputBytes int    `json:"max_output_bytes"`
	}
	tool := fakes.NewFakeTool(domain.ToolDefinition{
		Name:         "run_cmd",
		Description:  "Run a shell command",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}, domain.ToolResult{Status: domain.ToolStatusSuccess}).WithPrepareFn(
		func(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
			if _, err := toolkit.DecodeLenient[runCmdArgs](call.Arguments); err != nil {
				return domain.PreparedCall{}, err
			}
			return domain.PreparedCall{}, errors.New("unreachable in this test")
		},
	)
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop, run := newPairingTestLoop(t, domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "run_cmd",
		Arguments: json.RawMessage(`{"command":"seq 1 100000","max_output_tokens":4000}`),
	}, registry, DefaultPolicy{}, fakes.NewFakeApprover(domain.DecisionAllow))
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	var errText string
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.Error != nil {
				errText = part.ToolResult.Error.Message
			}
		}
	}
	if !strings.Contains(errText, `did you mean "max_output_bytes"?`) {
		t.Fatalf("model-facing error lacks the did-you-mean hint: %q", errText)
	}
	if !strings.Contains(errText, "Valid fields: command, max_output_bytes") {
		t.Fatalf("model-facing error lacks the valid field list: %q", errText)
	}
}

// --- early-rejection event pairing beyond prepare_failed (§4.6) ---

// countToolEventTriple tallies the prepared/started/completed audit events
// in the run's pending events.
func countToolEventTriple(run *Run) (prepared, started, completed int) {
	for _, evt := range run.pendingEvents {
		switch evt.Type {
		case domain.EventToolCallPrepared:
			prepared++
		case domain.EventToolExecutionStarted:
			started++
		case domain.EventToolExecutionCompleted:
			completed++
		}
	}
	return prepared, started, completed
}

// toolResultErrorCode extracts the first tool error code from the
// transcript, if any.
func toolResultErrorCode(run *Run) string {
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.Error != nil {
				return part.ToolResult.Error.Code
			}
		}
	}
	return ""
}

// newPairingTestLoop builds a one-call loop: the model issues tc, then
// ends the turn. registry and policy are caller-controlled.
func newPairingTestLoop(t *testing.T, tc domain.ToolCall, registry *ToolRegistry, policy Policy, approver domain.Approver) (*Loop, *Run) {
	t.Helper()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{tc}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	return &Loop{
		Run: run, Model: model, Policy: policy,
		Approver: approver, Registry: registry, Logger: slog.Default(),
	}, run
}

func TestUnknownToolKeepsEventStreamPaired(t *testing.T) {
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop, run := newPairingTestLoop(t, domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "totally_unknown_tool",
		Arguments: json.RawMessage(`{"path":"x"}`),
	}, registry, DefaultPolicy{}, fakes.NewFakeApprover(domain.DecisionAllow))
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	if prepared, started, completed := countToolEventTriple(run); prepared != 1 || started != 1 || completed != 1 {
		t.Fatalf("event pairing = %d/%d/%d, want 1/1/1", prepared, started, completed)
	}
	if code := toolResultErrorCode(run); code != "unknown_tool" {
		t.Fatalf("error code = %q, want unknown_tool", code)
	}
}

func TestViewImageGateKeepsEventStreamPaired(t *testing.T) {
	viewImage := fakes.NewFakeTool(domain.ToolDefinition{
		Name:         "view_image",
		Description:  "Attach an image",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}}}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, domain.ToolResult{Status: domain.ToolStatusSuccess})
	registry := NewToolRegistry()
	if err := registry.Register(viewImage); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop, run := newPairingTestLoop(t, domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "view_image",
		Arguments: json.RawMessage(`{"path":"assets/diagram.png"}`),
	}, registry, DefaultPolicy{}, fakes.NewFakeApprover(domain.DecisionAllow))
	loop.SupportsImages = false // text-only model: the vision gate fires
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	if prepared, started, completed := countToolEventTriple(run); prepared != 1 || started != 1 || completed != 1 {
		t.Fatalf("event pairing = %d/%d/%d, want 1/1/1", prepared, started, completed)
	}
	if code := toolResultErrorCode(run); code != "unsupported_modality" {
		t.Fatalf("error code = %q, want unsupported_modality", code)
	}
	if executed := len(viewImage.ExecutedCalls()); executed != 0 {
		t.Fatalf("gated tool executed %d times, want 0", executed)
	}
}

type denyAllPolicy struct{}

func (denyAllPolicy) Evaluate(domain.PreparedCall) domain.Verdict {
	return domain.Verdict{Source: "test", Decision: domain.DecisionDeny, Reason: "denied by test policy"}
}

func TestPolicyDenyKeepsEventStreamPaired(t *testing.T) {
	tool := fakes.ReadFileTool()
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop, run := newPairingTestLoop(t, domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "read_file",
		Arguments: json.RawMessage(`{"path":"a.txt"}`),
	}, registry, denyAllPolicy{}, fakes.NewFakeApprover(domain.DecisionAllow))
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	// The deny happens after a proper prepare: prepared carries the real
	// audit payload (not the degraded one), started pairs the completion.
	if prepared, started, completed := countToolEventTriple(run); prepared != 1 || started != 1 || completed != 1 {
		t.Fatalf("event pairing = %d/%d/%d, want 1/1/1", prepared, started, completed)
	}
	if code := toolResultErrorCode(run); code != "permission_denied" {
		t.Fatalf("error code = %q, want permission_denied", code)
	}
	if executed := len(tool.ExecutedCalls()); executed != 0 {
		t.Fatalf("denied tool executed %d times, want 0", executed)
	}
}

func TestUserDenyKeepsEventStreamPaired(t *testing.T) {
	writeTool := fakes.NewFakeTool(domain.ToolDefinition{
		Name:         "write",
		Description:  "Write a file",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
		Capabilities: []domain.Capability{domain.CapFSWrite}, // R2 → DefaultPolicy asks
		Source:       domain.ToolSourceBuiltin,
	}, domain.ToolResult{Status: domain.ToolStatusSuccess})
	registry := NewToolRegistry()
	if err := registry.Register(writeTool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop, run := newPairingTestLoop(t, domain.ToolCall{
		ID: domain.NewToolCallID(), Name: "write",
		Arguments: json.RawMessage(`{"path":"a.txt","content":"x"}`),
	}, registry, DefaultPolicy{}, fakes.NewFakeApprover(domain.DecisionDeny))
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	if prepared, started, completed := countToolEventTriple(run); prepared != 1 || started != 1 || completed != 1 {
		t.Fatalf("event pairing = %d/%d/%d, want 1/1/1", prepared, started, completed)
	}
	if code := toolResultErrorCode(run); code != "permission_denied" {
		t.Fatalf("error code = %q, want permission_denied", code)
	}
	if executed := len(writeTool.ExecutedCalls()); executed != 0 {
		t.Fatalf("user-denied tool executed %d times, want 0", executed)
	}
}

// --- unified ingestion truncation (§4.5) ---

func TestRecordToolResultNormalizesTimestampsToUTC(t *testing.T) {
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	// Tools stamp results with time.Now() (local zone); the loop stamps
	// rejections with Clock.Now() (UTC). Persistence must carry one
	// canonical form or string-sorting consumers misalign the streams.
	local := time.Date(2026, 9, 27, 21, 17, 8, 0, time.FixedZone("CST", 8*3600))
	run.RecordToolResult(domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: "ok"}},
		StartedAt:  local,
		FinishedAt: local,
	})
	got := run.Messages[len(run.Messages)-1].Parts[0].ToolResult
	if got.StartedAt.Location() != time.UTC || got.FinishedAt.Location() != time.UTC {
		t.Fatalf("message timestamps not normalized to UTC: %v / %v", got.StartedAt, got.FinishedAt)
	}
	found := false
	for _, evt := range run.pendingEvents {
		if evt.Type != domain.EventToolExecutionCompleted {
			continue
		}
		found = true
		var payload toolExecutionCompletedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("unmarshal completed payload: %v", err)
		}
		if payload.StartedAt.Location() != time.UTC || payload.FinishedAt.Location() != time.UTC {
			t.Fatalf("event timestamps not normalized to UTC: %v / %v", payload.StartedAt, payload.FinishedAt)
		}
	}
	if !found {
		t.Fatal("no tool.execution_completed event recorded")
	}
}

func TestRecordToolResultTruncatesOversizedOutput(t *testing.T) {
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxToolOutputBytes: 1024}, domain.RealClock{})
	original := strings.Repeat("h", 800) + strings.Repeat("m", 800) + strings.Repeat("t", 800)
	run.RecordToolResult(domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content:   []domain.ContentPart{{Kind: domain.PartText, Text: original}},
		StartedAt: time.Now(), FinishedAt: time.Now(),
	})
	text := run.Messages[len(run.Messages)-1].Parts[0].ToolResult.Content[0].Text
	if len(text) > 1024 {
		t.Fatalf("truncated length = %d, want ≤ 1024", len(text))
	}
	if !strings.HasPrefix(text, "Warning: output truncated (original 2.3KB") {
		t.Fatalf("warning header missing: %q", text[:120])
	}
	if !strings.Contains(text, toolOutputTruncationMark) {
		t.Fatal("head+tail marker missing")
	}
	if !strings.HasSuffix(text, strings.Repeat("t", 100)) {
		t.Fatal("tail portion missing")
	}
	if !utf8.ValidString(text) {
		t.Fatal("truncated text must stay valid UTF-8")
	}
}

// --- wrap-up crash recovery (§4.4.2) ---

func TestRecoverRunReArmsBudgetWrapUp(t *testing.T) {
	sessionID := domain.NewSessionID()
	clock := domain.NewFakeClock(time.Now().UTC())
	run := NewRun(sessionID, domain.DefaultLimits(), clock)
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: clock.Now(),
	})
	run.WrapUpPending = dimensionTokens
	run.appendEvent(domain.EventBudgetWrapupStarted, domain.BudgetWrapupPayload{
		Dimension: dimensionTokens, Usage: 1, Limit: 1,
	})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: budgetWrapUpPrompt(dimensionTokens)}},
		CreatedAt: clock.Now(),
		Metadata:  map[string]string{"kind": "budget_wrapup"},
	})

	events := run.PendingEvents()
	recovered, err := RecoverRun(sessionID, nil, run.Messages, events, int64(len(events)), domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun error = %v", err)
	}
	if recovered.WrapUpPending != dimensionTokens {
		t.Fatalf("WrapUpPending = %q, want %q (re-armed from the event)", recovered.WrapUpPending, dimensionTokens)
	}
}
