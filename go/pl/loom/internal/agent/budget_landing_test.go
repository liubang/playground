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
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxEstimatedCostUSD: 1.0, MaxWallTime: time.Hour}, domain.RealClock{})
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
		})
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID:   domain.NewToolCallID(),
				Name: "read_file",
				Arguments: json.RawMessage(
					`{"__malformed_arguments":"{bad json","error":"model emitted invalid arguments JSON; re-issue the tool call with valid arguments"}`),
			}},
			StopReason: domain.StopToolUse,
		},
		fakes.ScriptEntry{Text: "recovered", StopReason: domain.StopEndTurn},
	)
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxWallTime: time.Hour}, domain.RealClock{})
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

// --- unified ingestion truncation (§4.5) ---

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
	run.WrapUpPending = dimensionWallTime
	run.appendEvent(domain.EventBudgetWrapupStarted, domain.BudgetWrapupPayload{
		Dimension: dimensionWallTime, Usage: 1, Limit: 1,
	})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: budgetWrapUpPrompt(dimensionWallTime)}},
		CreatedAt: clock.Now(),
		Metadata:  map[string]string{"kind": "budget_wrapup"},
	})

	events := run.PendingEvents()
	recovered, err := RecoverRun(sessionID, nil, run.Messages, events, int64(len(events)), domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun error = %v", err)
	}
	if recovered.WrapUpPending != dimensionWallTime {
		t.Fatalf("WrapUpPending = %q, want %q (re-armed from the event)", recovered.WrapUpPending, dimensionWallTime)
	}
}
