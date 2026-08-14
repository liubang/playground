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
// Created: 2026/08/03

package app

import (
	"context"
	"encoding/json"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// envCall records one model call's turn-context attribution env and last
// user message, so the test can attribute each call to its owning session.
type envCall struct {
	env      map[string]string
	lastUser string
}

// envRecordingModel wraps a FakeModel and captures per-call attribution.
type envRecordingModel struct {
	inner *fakes.FakeModel
	mu    sync.Mutex
	calls []envCall
}

func (m *envRecordingModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	lastUser := ""
	for i := len(req.Messages) - 1; i >= 0; i-- {
		if req.Messages[i].Role == domain.RoleUser {
			for _, part := range req.Messages[i].Parts {
				if part.Kind == domain.PartText {
					lastUser += part.Text
				}
			}
			break
		}
	}
	m.mu.Lock()
	m.calls = append(m.calls, envCall{env: process.SessionEnvFromContext(ctx), lastUser: lastUser})
	m.mu.Unlock()
	return m.inner.Stream(ctx, req)
}

func (m *envRecordingModel) recorded() []envCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]envCall, len(m.calls))
	copy(out, m.calls)
	return out
}

func (m *envRecordingModel) envForPrompt(t *testing.T, prompt string) map[string]string {
	t.Helper()
	for _, call := range m.recorded() {
		if call.lastUser == prompt {
			return call.env
		}
	}
	t.Fatalf("no model call with last user message %q", prompt)
	return nil
}

// TestServeSessionsDoNotCrossTalk is the G2/G3 regression lock
// (docs/SERVE_DESIGN.md §11): two sessions driven through one
// SessionService must keep goal/plan/steer/question/SessionEnv/approval
// state strictly per-session. The bootstrap carries "trap" cells — any
// leak back to process-level state lands in them and fails the test.
func TestServeSessionsDoNotCrossTalk(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		// A turn 1: goal + plan writes, then the final answer.
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{
			// Completed goal/plan statuses: an active goal or unfinished
			// plan would arm the loop's closing reconcile nudge and burn an
			// extra scripted call.
			{ID: domain.NewToolCallID(), Name: "update_goal", Arguments: json.RawMessage(`{"objective":"goal-A","status":"complete"}`)},
			{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(`{"plan":[{"goal":"step one","status":"done"},{"goal":"step two","status":"done"}]}`)},
		}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "A1 done", StopReason: domain.StopEndTurn},
		// A turn 2: an ask_user question, answered mid-test; the steered
		// message drains into the follow-up request.
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{
			{ID: domain.NewToolCallID(), Name: "ask_user", Arguments: json.RawMessage(`{"question":"pick one","options":[{"label":"x"}]}`)},
		}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "A2 done", StopReason: domain.StopEndTurn},
		// Slack for a possible steer-relay turn.
		fakes.ScriptEntry{Text: "A relay done", StopReason: domain.StopEndTurn},
		// B turn 1.
		fakes.ScriptEntry{Text: "B done", StopReason: domain.StopEndTurn},
	)
	rec := &envRecordingModel{inner: model}
	bootstrap := testBootstrap(store, rec)
	// Trap cells: session state must NEVER land in the process-level cells.
	bootstrap.GoalCell = agent.NewGoalCell()
	bootstrap.PlanCell = agent.NewPlanCell()
	bootstrap.SteerCell = agent.NewSteerCell()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := NewSingletonWorkspaceService(bootstrap, broker, SessionServiceConfig{})
	defer func() { _ = svc.Shutdown(context.Background()) }()

	hA, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(A): %v", err)
	}
	hB, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(B): %v", err)
	}

	// --- turn A1: goal/plan writes ---
	if _, _, err := svc.SubmitPrompt(ctx, hA.ID, "question-A", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(A1): %v", err)
	}
	waitForIdle(t, hA.Controller)

	// The writes were executed (the loop drained them) but never leaked to
	// the process-level trap cells.
	if _, ok := bootstrap.GoalCell.Take(); ok {
		t.Fatalf("goal write leaked into the process-level GoalCell")
	}
	if _, ok := bootstrap.PlanCell.Take(); ok {
		t.Fatalf("plan write leaked into the process-level PlanCell")
	}
	// A's loop drained its own cells (write and drain on the same cell ⇒
	// tool and loop are bound to the same per-session runtime).
	if _, ok := hA.Runtime.GoalCell.Take(); ok {
		t.Fatalf("session A's GoalCell was not drained by its own loop")
	}
	if _, ok := hA.Runtime.PlanCell.Take(); ok {
		t.Fatalf("session A's PlanCell was not drained by its own loop")
	}
	// B stayed clean.
	if _, ok := hB.Runtime.GoalCell.Take(); ok {
		t.Fatalf("goal write leaked into session B's GoalCell")
	}
	if _, ok := hB.Runtime.PlanCell.Take(); ok {
		t.Fatalf("plan write leaked into session B's PlanCell")
	}
	// SessionEnv attribution for A's calls carries A's session id.
	if got := rec.envForPrompt(t, "question-A")[process.EnvSessionID]; got != hA.ID.String() {
		t.Fatalf("A's turn env LOOM_SESSION_ID = %q, want %s", got, hA.ID)
	}

	// --- turn A2: question + steer isolation ---
	if _, _, err := svc.SubmitPrompt(ctx, hA.ID, "ask something", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(A2): %v", err)
	}
	var pending []domain.EventID
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		pending = hA.Runtime.Questioner.PendingQuestions()
		if len(pending) > 0 {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if len(pending) != 1 {
		t.Fatalf("session A pending questions = %d, want 1", len(pending))
	}
	if got := hB.Runtime.Questioner.PendingQuestions(); len(got) != 0 {
		t.Fatalf("ask_user leaked into session B's questioner: %v", got)
	}

	// While A is busy, a new prompt steers A — and only A.
	if _, _, err := svc.SubmitPrompt(ctx, hA.ID, "steer-A", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(steer-A): %v", err)
	}
	if got := hA.Runtime.SteerCell.Len(); got != 1 {
		t.Fatalf("session A SteerCell len = %d, want 1", got)
	}
	if got := hB.Runtime.SteerCell.Len(); got != 0 {
		t.Fatalf("steer leaked into session B's SteerCell (len=%d)", got)
	}
	if got := bootstrap.SteerCell.Len(); got != 0 {
		t.Fatalf("steer leaked into the process-level SteerCell (len=%d)", got)
	}

	// Approval routing is per handle: pend an approval directly on A.
	approvalReq := domain.ApprovalRequest{
		ID: domain.NewEventID(),
		Call: domain.PreparedCall{
			Call:     domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd"},
			ArgsHash: "hash-A",
		},
	}
	approvalCtx, cancelApproval := context.WithCancel(ctx)
	defer cancelApproval()
	approvalDone := make(chan struct{})
	go func() {
		defer close(approvalDone)
		_, _ = hA.Approver.RequestApproval(approvalCtx, approvalReq)
	}()
	deadline = time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if hA.Approver.PendingCount() == 1 {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if got := hA.Approver.PendingCount(); got != 1 {
		t.Fatalf("session A pending approvals = %d, want 1", got)
	}
	if got := hB.Approver.PendingCount(); got != 0 {
		t.Fatalf("approval leaked into session B's approver (pending=%d)", got)
	}
	cancelApproval()
	<-approvalDone

	// Resolve A's question; A's turn finishes.
	if _, err := svc.AnswerQuestion(ctx, hA.ID, pending[0], domain.QuestionAnswer{Selected: []string{"x"}}); err != nil {
		t.Fatalf("AnswerQuestion: %v", err)
	}
	waitForIdle(t, hA.Controller)

	// --- turn B1: env attribution + no cross-contamination in requests ---
	if _, _, err := svc.SubmitPrompt(ctx, hB.ID, "question-B", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(B1): %v", err)
	}
	waitForIdle(t, hB.Controller)

	if got := rec.envForPrompt(t, "question-B")[process.EnvSessionID]; got != hB.ID.String() {
		t.Fatalf("B's turn env LOOM_SESSION_ID = %q, want %s", got, hB.ID)
	}
	// B's request context must not contain anything from A's turns.
	for i, call := range rec.recorded() {
		if call.lastUser != "question-B" {
			continue
		}
		for _, msg := range model.Calls()[i].Messages {
			for _, part := range msg.Parts {
				if part.Kind != domain.PartText {
					continue
				}
				if strings.Contains(part.Text, "goal-A") || strings.Contains(part.Text, "steer-A") {
					t.Fatalf("session A content leaked into B's request context: %q", part.Text)
				}
			}
		}
	}
}
