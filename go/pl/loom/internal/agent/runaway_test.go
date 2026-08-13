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
	"fmt"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// scriptToolCalls builds consecutive script entries repeating one tool call.
func scriptToolCalls(call domain.ToolCall, times int, finalText string) []fakes.ScriptEntry {
	entries := make([]fakes.ScriptEntry, 0, times+1)
	for i := 0; i < times; i++ {
		entries = append(entries, fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: call.Name, Arguments: call.Arguments}},
			StopReason: domain.StopToolUse,
		})
	}
	if finalText != "" {
		entries = append(entries, fakes.ScriptEntry{Text: finalText, StopReason: domain.StopEndTurn})
	}
	return entries
}

func runLoopWithTool(t *testing.T, tool domain.Tool, script ...fakes.ScriptEntry) (*Run, *Loop, error) {
	t.Helper()
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(script...),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
	}
	return run, loop, loop.Execute(context.Background())
}

// The repeated-call detector warns on the second identical call and
// terminates on the third (docs/CONTEXT_DESIGN.md §4.4.3).
func TestRunawayRepeatedCallsTerminate(t *testing.T) {
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	run, _, err := runLoopWithTool(t, fakes.ReadFileTool(), scriptToolCalls(call, 3, "")...)
	if err == nil || !strings.Contains(err.Error(), "identical arguments") {
		t.Fatalf("Execute error = %v, want repeated-call termination", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	// The warning fired after the second repeat.
	warning := false
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventBudgetNotice {
			warning = true
		}
	}
	if !warning {
		t.Fatal("second repeat must queue a runaway warning notice")
	}
	// Every issued call got a paired result — no dangling calls.
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("transcript must stay paired: %+v", dangling)
	}
}

// Prepare failures feed the repeated-call detector (hashed from the raw
// arguments) but never the execution-failure streak.
func TestRunawayRepeatedPrepareFailures(t *testing.T) {
	tool := fakes.ReadFileTool().WithPrepareFn(
		func(_ context.Context, _ domain.ToolCall) (domain.PreparedCall, error) {
			return domain.PreparedCall{}, errors.New("context must be between 0 and 5")
		},
	)
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"context":10}`)}
	run, _, err := runLoopWithTool(t, tool, scriptToolCalls(call, 3, "")...)
	if err == nil || !strings.Contains(err.Error(), "identical arguments") {
		t.Fatalf("Execute error = %v, want repeated-call termination on prepare failures", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
	if run.Usage.ToolCalls != 3 {
		t.Fatalf("tool results recorded = %d, want 3 (prepare failures still record results)", run.Usage.ToolCalls)
	}
}

// Consecutive execution-phase failures terminate; a success resets the streak.
func TestRunawayConsecutiveExecFailures(t *testing.T) {
	failTool := fakes.NewFakeTool(domain.ToolDefinition{
		Name:         "flaky",
		Description:  "always fails",
		InputSchema:  json.RawMessage(`{"type":"object"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, domain.ToolResult{
		Status: domain.ToolStatusError,
		Error:  &domain.ToolError{Code: "unavailable", Message: "boom"},
	})
	entries := make([]fakes.ScriptEntry, 0, 5)
	for i := 0; i < 5; i++ {
		entries = append(entries, fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID: domain.NewToolCallID(), Name: "flaky",
				Arguments: json.RawMessage(`{"n":` + fmt.Sprint(i+1) + `}`),
			}},
			StopReason: domain.StopToolUse,
		})
	}
	run, _, err := runLoopWithTool(t, failTool, entries...)
	if err == nil || !strings.Contains(err.Error(), "consecutive tool executions failed") {
		t.Fatalf("Execute error = %v, want consecutive-failure termination", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed", run.State.Outcome)
	}
}

// A stall — turns with no progress signal at all — earns a converge
// reminder but never a termination.
func TestRunawayStallWarnsWithoutTerminating(t *testing.T) {
	// The first call's new signature is progress; the two repeats produce
	// nothing new, so the stall counter reaches the warning threshold.
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	entries := scriptToolCalls(call, 3, "final answer")
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(entries...),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		Runaway: domain.RunawayConfig{MaxRepeatedCalls: 10, MaxConsecutiveFailures: 10, StallWarnTurns: 2},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v (a stall warns, never terminates)", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	stallNotice := false
	for _, msg := range run.Messages {
		if strings.Contains(strings.Join(msg.TextParts(), ""), "no visible progress") {
			stallNotice = true
		}
	}
	if !stallNotice {
		t.Fatal("two progress-less turns must inject the stall reminder")
	}
}

// The converge reminder fires every stall_warn_turns while the stall
// persists — not just once (REVIEW H15).
func TestRunawayStallReminderFiresPeriodically(t *testing.T) {
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	// 5 repeated calls after the first-seen one: with StallWarnTurns=2 the
	// stall streak reaches 2 (level 1) and then 4 (level 2) before the run
	// ends, so exactly two reminders must be injected.
	entries := scriptToolCalls(call, 6, "final answer")
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(entries...),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		Runaway: domain.RunawayConfig{MaxRepeatedCalls: 10, MaxConsecutiveFailures: 10, StallWarnTurns: 2},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v (a stall warns, never terminates)", err)
	}
	notices := 0
	for _, msg := range run.Messages {
		if strings.Contains(strings.Join(msg.TextParts(), ""), "no visible progress") {
			notices++
		}
	}
	if notices != 2 {
		t.Fatalf("stall reminders = %d, want 2 (one per stall_warn_turns while stalled)", notices)
	}
}

// Read-only research tasks produce a progress signal on every first-seen
// call signature, so they never trip the stall detector.
func TestRunawayResearchTaskDoesNotStall(t *testing.T) {
	entries := make([]fakes.ScriptEntry, 0, 12)
	for i := 0; i < 11; i++ {
		entries = append(entries, fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID: domain.NewToolCallID(), Name: "read_file",
				Arguments: json.RawMessage(fmt.Sprintf(`{"path":"f%d.go"}`, i)),
			}},
			StopReason: domain.StopToolUse,
		})
	}
	entries = append(entries, fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn})
	run := NewRun(domain.NewSessionID(), domain.Limits{}, domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: time.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(entries...),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		Runaway: domain.RunawayConfig{MaxRepeatedCalls: 20, MaxConsecutiveFailures: 20, StallWarnTurns: 3},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v", err)
	}
	for _, msg := range run.Messages {
		if strings.Contains(strings.Join(msg.TextParts(), ""), "no visible progress") {
			t.Fatal("first-seen signatures are progress; research must not stall")
		}
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
}

// The stall watchdog (CONTEXT_DESIGN §4.4.3): ACTIVE time without any
// progress signal beyond StallTimeout enters the soft-landing wrap-up
// (dimension stall) and the run terminates FAILED — a stall is an
// abnormal ending, not a budget landing.
func TestRunawayStallTimeoutSoftLands(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	// Each routing pass burns 20s of active time (slow preparation); the
	// identical repeat calls produce no new progress signal after the
	// first one.
	tool := fakes.ReadFileTool().WithPrepareFn(
		func(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
			clock.Advance(20 * time.Second)
			return fakes.ReadFileTool().Prepare(ctx, call)
		},
	)
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	entries := scriptToolCalls(call, 8, "wrap-up summary")
	run := NewRun(domain.NewSessionID(), domain.Limits{}, clock)
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: clock.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(entries...),
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		Runaway: domain.RunawayConfig{
			MaxRepeatedCalls: 100, MaxConsecutiveFailures: 100,
			StallWarnTurns: 1000, StallTimeout: time.Minute,
		},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v, want clean soft-landing termination", err)
	}
	if run.State.Outcome != domain.OutcomeFailed {
		t.Fatalf("outcome = %s, want failed (a stall is an abnormal ending)", run.State.Outcome)
	}
	wrapup := false
	for _, evt := range run.pendingEvents {
		if evt.Type != domain.EventBudgetWrapupStarted {
			continue
		}
		var payload domain.BudgetWrapupPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("wrapup payload: %v", err)
		}
		if payload.Dimension != dimensionStall {
			t.Fatalf("wrapup dimension = %q, want stall", payload.Dimension)
		}
		wrapup = true
	}
	if !wrapup {
		t.Fatal("budget.wrapup_started (stall) event missing")
	}
	if dangling := unresolvedToolCalls(run.Messages); len(dangling) > 0 {
		t.Fatalf("transcript must stay paired: %+v", dangling)
	}
}

// clockAdvancingApprover simulates a slow human: every approval takes
// fixed time before allowing.
type clockAdvancingApprover struct {
	clock *domain.FakeClock
	wait  time.Duration
}

func (a clockAdvancingApprover) RequestApproval(context.Context, domain.ApprovalRequest) (domain.Decision, error) {
	a.clock.Advance(a.wait)
	return domain.DecisionAllow, nil
}

// Approval waits are user thinking time, not agent activity: hours of
// approval latency must never trip the stall watchdog (the v2 incident:
// a 24-minute approval wait exhausted the entire wall-clock budget).
func TestRunawayStallWatchdogIgnoresApprovalWait(t *testing.T) {
	clock := domain.NewFakeClock(time.Now().UTC())
	call := domain.ToolCall{Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`)}
	entries := scriptToolCalls(call, 4, "done")
	run := NewRun(domain.NewSessionID(), domain.Limits{}, clock)
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "work"}},
		CreatedAt: clock.Now(),
	})
	registry := NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("Register error: %v", err)
	}
	loop := &Loop{
		Run: run, Model: fakes.NewFakeModel(entries...),
		Policy:   askAllPolicy{},
		Approver: clockAdvancingApprover{clock: clock, wait: 10 * time.Minute},
		Registry: registry, Logger: slog.Default(),
		Runaway: domain.RunawayConfig{
			MaxRepeatedCalls: 100, MaxConsecutiveFailures: 100,
			StallWarnTurns: 1000, StallTimeout: time.Minute,
		},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute error = %v (approval waits must not stall the run)", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded (40m of approval waits must not count)", run.State.Outcome)
	}
}

// Regression: the pre-redesign turn quota (MaxTurns=50) killed legitimate
// long work. A 60-turn run must complete now that turn counts are no
// longer a budget dimension.
func TestLongLegitimateRunIsNotTurnCapped(t *testing.T) {
	entries := make([]fakes.ScriptEntry, 0, 61)
	for i := 0; i < 60; i++ {
		entries = append(entries, fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{{
				ID: domain.NewToolCallID(), Name: "read_file",
				Arguments: json.RawMessage(fmt.Sprintf(`{"path":"f%d.go"}`, i)),
			}},
			StopReason: domain.StopToolUse,
		})
	}
	entries = append(entries, fakes.ScriptEntry{Text: "all done", StopReason: domain.StopEndTurn})
	run, _, err := runLoopWithTool(t, fakes.ReadFileTool(), entries...)
	if err != nil {
		t.Fatalf("Execute error = %v, want a 60-turn run to complete", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if run.Usage.Turns != 61 {
		t.Fatalf("turns = %d, want 61", run.Usage.Turns)
	}
}
