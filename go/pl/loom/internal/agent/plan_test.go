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
// Created: 2026/07/26

package agent

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func newPlanTool(t *testing.T) (*UpdateTaskTool, *PlanCell) {
	t.Helper()
	cell := NewPlanCell()
	tool, err := NewUpdateTaskTool(NewGoalCell(), cell)
	if err != nil {
		t.Fatalf("NewUpdateTaskTool error: %v", err)
	}
	return tool, cell
}

func planCall(t *testing.T, args string) domain.ToolCall {
	t.Helper()
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: "update_task", Arguments: json.RawMessage(args)}
}

const validPlanArgs = `{"action":"plan","plan":[` +
	`{"goal":"read existing code","status":"done","evidence":["read goal.go"]},` +
	`{"goal":"implement update_task","status":"in_progress"},` +
	`{"goal":"add tests","status":"todo"}]}`

func TestUpdatePlanPrepareValidSnapshot(t *testing.T) {
	tool, _ := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t, validPlanArgs))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	if prepared.Risk != domain.R1 {
		t.Fatalf("risk = %v, want R1 (bookkeeping, no approval)", prepared.Risk)
	}
	if !strings.Contains(prepared.ApprovalDesc, "implement update_task") {
		t.Fatalf("approval desc should name the in-progress step: %q", prepared.ApprovalDesc)
	}
	// Canonical arguments round-trip to the same plan.
	plan, _, _, err := decodeUpdatePlanArgs(prepared.Call.Arguments)
	if err != nil {
		t.Fatalf("canonical arguments no longer decode: %v", err)
	}
	if len(plan.Items) != 3 {
		t.Fatalf("items = %d, want 3", len(plan.Items))
	}
	for i, item := range plan.Items {
		if item.Index != i {
			t.Fatalf("items[%d].Index = %d, want reassigned %d", i, item.Index, i)
		}
	}
	if got := plan.Items[0].Evidence; len(got) != 1 || got[0] != "read goal.go" {
		t.Fatalf("evidence lost: %+v", got)
	}
}

// Models occasionally emit evidence as a bare string instead of the
// one-element array the schema declares; the tool normalizes it instead of
// burning a tool round-trip on a strict decode error.
func TestUpdatePlanPrepareToleratesStringEvidence(t *testing.T) {
	tool, _ := newPlanTool(t)
	args := `{"action":"plan","plan":[` +
		`{"goal":"read existing code","status":"done","evidence":"read goal.go"},` +
		`{"goal":"implement update_task","status":"done","evidence":null},` +
		`{"goal":"add tests","status":"in_progress"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, args))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	// The canonical arguments carry the normalized array form.
	if !strings.Contains(string(prepared.Call.Arguments), `"evidence":["read goal.go"]`) {
		t.Fatalf("canonical arguments not normalized: %s", prepared.Call.Arguments)
	}
	plan, _, _, err := decodeUpdatePlanArgs(prepared.Call.Arguments)
	if err != nil {
		t.Fatalf("canonical arguments no longer decode: %v", err)
	}
	if got := plan.Items[0].Evidence; len(got) != 1 || got[0] != "read goal.go" {
		t.Fatalf("string evidence not wrapped: %+v", got)
	}
	if got := plan.Items[1].Evidence; len(got) != 0 {
		t.Fatalf("null evidence = %v, want none", got)
	}
}

func TestUpdatePlanPrepareRejectsInvalidSnapshots(t *testing.T) {
	tool, _ := newPlanTool(t)
	cases := map[string]string{
		"single step":        `{"action":"plan","plan":[{"goal":"only one","status":"in_progress"}]}`,
		"empty plan":         `{"action":"plan","plan":[]}`,
		"missing plan":       `{"action":"plan"}`,
		"unknown action":     `{"action":"task","plan":[{"goal":"a","status":"todo"},{"goal":"b","status":"todo"}]}`,
		"unknown field":      `{"action":"plan","plan":[{"goal":"a","status":"todo"},{"goal":"b","status":"todo"}],"extra":1}`,
		"empty goal":         `{"action":"plan","plan":[{"goal":"  ","status":"todo"},{"goal":"b","status":"todo"}]}`,
		"bad status":         `{"action":"plan","plan":[{"goal":"a","status":"doing"},{"goal":"b","status":"todo"}]}`,
		"two in_progress":    `{"action":"plan","plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"in_progress"}]}`,
		"unknown item field": `{"action":"plan","plan":[{"goal":"a","status":"todo","step":"a"},{"goal":"b","status":"todo"}]}`,
		"numeric evidence":   `{"action":"plan","plan":[{"goal":"a","status":"done","evidence":5},{"goal":"b","status":"todo"}]}`,
	}
	for name, args := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := tool.Prepare(context.Background(), planCall(t, args)); err == nil {
				t.Fatalf("Prepare(%s) succeeded, want rejection", args)
			}
		})
	}
}

func TestUpdatePlanTitleHandling(t *testing.T) {
	tool, cell := newPlanTool(t)

	// Title set at creation.
	withTitle := `{"action":"plan","title":"loom 架构梳理","plan":[` +
		`{"goal":"a","status":"in_progress"},{"goal":"b","status":"todo"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, withTitle))
	if err != nil {
		t.Fatalf("Prepare with title error: %v", err)
	}
	tool.Execute(context.Background(), prepared)
	plan, ok := cell.Take()
	if !ok || plan.Title != "loom 架构梳理" {
		t.Fatalf("title not captured: %q (ok=%v)", plan.Title, ok)
	}

	// Title capped at 120 runes.
	long := strings.Repeat("长", 130)
	longArgs := `{"action":"plan","title":"` + long + `","plan":[{"goal":"a","status":"todo"},{"goal":"b","status":"todo"}]}`
	prepared, err = tool.Prepare(context.Background(), planCall(t, longArgs))
	if err != nil {
		t.Fatalf("Prepare long title error: %v", err)
	}
	tool.Execute(context.Background(), prepared)
	plan, _ = cell.Take()
	if got := len([]rune(plan.Title)); got != 120 {
		t.Fatalf("title length = %d runes, want capped 120", got)
	}
}

func TestDrainPlanUpdatesPreservesTitleAcrossRevisions(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	cell := NewPlanCell()
	loop := &Loop{Run: run, PlanCell: cell}

	cell.Put(domain.Plan{Title: "overall objective", Items: []domain.PlanItem{
		{Index: 0, Goal: "a", Status: domain.PlanItemInProgress},
		{Index: 1, Goal: "b", Status: domain.PlanItemTodo},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Title != "overall objective" {
		t.Fatalf("title = %q, want set at creation", run.Plan.Title)
	}

	// A revision without a title keeps the existing one.
	cell.Put(domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "a", Status: domain.PlanItemDone},
		{Index: 1, Goal: "b", Status: domain.PlanItemInProgress},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Title != "overall objective" {
		t.Fatalf("title = %q after title-less revision, want preserved", run.Plan.Title)
	}

	// A revision with a new title replaces it.
	cell.Put(domain.Plan{Title: "renamed", Items: []domain.PlanItem{
		{Index: 0, Goal: "a", Status: domain.PlanItemDone},
		{Index: 1, Goal: "b", Status: domain.PlanItemDone},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Title != "renamed" {
		t.Fatalf("title = %q after renamed revision, want replaced", run.Plan.Title)
	}
}

// Models routinely mirror the flat schema and fill goal-owned fields
// (objective/token_budget/status) into a plan call. The strict-decode
// rejection of those fields produced retry doom loops in the wild, so the
// tool strips them and discloses the correction via ignored_fields.
func TestUpdatePlanPrepareToleratesGoalFields(t *testing.T) {
	tool, cell := newPlanTool(t)
	args := `{"action":"plan","objective":"analyze the table","token_budget":5000,"status":"complete",` +
		`"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"todo"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, args))
	if err != nil {
		t.Fatalf("Prepare with goal fields error: %v", err)
	}
	// The canonical arguments record the stripped fields for audit.
	if !strings.Contains(string(prepared.Call.Arguments), `"ignored_fields":["objective","token_budget","status"]`) {
		t.Fatalf("canonical arguments missing ignored_fields record: %s", prepared.Call.Arguments)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	var payload struct {
		Applied       bool     `json:"applied"`
		Items         int      `json:"items"`
		IgnoredFields []string `json:"ignored_fields"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("result payload undecodable: %v", err)
	}
	if !payload.Applied || payload.Items != 2 {
		t.Fatalf("plan not applied: %+v", payload)
	}
	if len(payload.IgnoredFields) != 3 || payload.IgnoredFields[0] != "objective" {
		t.Fatalf("ignored_fields = %v, want [objective token_budget status]", payload.IgnoredFields)
	}
	// The goal side must NOT fire: stripping means the objective never
	// reaches the goal cell (the goal machinery would otherwise start
	// cross-turn continuation the model never asked for).
	plan, ok := cell.Take()
	if !ok || len(plan.Items) != 2 {
		t.Fatalf("plan cell = %+v (ok=%v), want 2-item snapshot", plan, ok)
	}
}

// Null-valued cross-action fields are removed silently: models mirror
// optional properties they have no value for as explicit nulls, and
// disclosing those would be noise on every call.
func TestUpdatePlanPrepareIgnoresNullGoalFieldsSilently(t *testing.T) {
	tool, _ := newPlanTool(t)
	args := `{"action":"plan","objective":null,"token_budget":null,"status":null,` +
		`"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"todo"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, args))
	if err != nil {
		t.Fatalf("Prepare with null goal fields error: %v", err)
	}
	if strings.Contains(string(prepared.Call.Arguments), "ignored_fields") {
		t.Fatalf("null strays must not be disclosed: %s", prepared.Call.Arguments)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if strings.Contains(result.Content[0].Text, "ignored_fields") {
		t.Fatalf("result must not disclose null strays: %s", result.Content[0].Text)
	}
}

// A missing action discriminator is inferred from the payload: a plan
// array means the plan action, goal-owned fields mean the goal action.
func TestUpdateTaskPrepareInfersMissingAction(t *testing.T) {
	tool, cell := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"todo"}]}`))
	if err != nil {
		t.Fatalf("Prepare without action error: %v", err)
	}
	if !strings.Contains(string(prepared.Call.Arguments), `"action":"plan"`) {
		t.Fatalf("canonical arguments must pin the inferred action: %s", prepared.Call.Arguments)
	}
	if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if plan, ok := cell.Take(); !ok || len(plan.Items) != 2 {
		t.Fatalf("plan cell = %+v (ok=%v), want 2-item snapshot", plan, ok)
	}

	// An empty payload has nothing to infer from.
	if _, err := tool.Prepare(context.Background(), planCall(t, `{}`)); err == nil {
		t.Fatal("Prepare({}) succeeded, want rejection")
	}
}

// Inference runs AFTER null-normalization: a null plan field must not
// masquerade as a plan payload — the real objective field picks the goal
// action instead (Prepare drops null mirrors before inferring).
func TestUpdateTaskInferSkipsNullMirroredFields(t *testing.T) {
	tool, _ := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"objective":"ship the refactor","plan":null,"title":null}`))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	if !strings.Contains(string(prepared.Call.Arguments), `"action":"goal"`) {
		t.Fatalf("null plan field must not trigger plan inference: %s", prepared.Call.Arguments)
	}
}

// When goal-owned and plan-owned fields both carry real values and the
// discriminator is missing, the plan payload wins: plan-owned fields are
// checked first, and the stray objective is stripped with in-band
// disclosure rather than silently dropped or hard-rejected.
func TestUpdateTaskInferPrefersPlanOnAmbiguousPayload(t *testing.T) {
	tool, cell := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"objective":"ship it","plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"todo"}]}`))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	canonical := string(prepared.Call.Arguments)
	if !strings.Contains(canonical, `"action":"plan"`) {
		t.Fatalf("ambiguous payload must infer the plan action: %s", canonical)
	}
	if !strings.Contains(canonical, `"ignored_fields":["objective"]`) {
		t.Fatalf("stray objective must be disclosed via ignored_fields: %s", canonical)
	}
	if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if plan, ok := cell.Take(); !ok || len(plan.Items) != 2 {
		t.Fatalf("plan cell = %+v (ok=%v), want 2-item snapshot", plan, ok)
	}
}

// A stringified token_budget is coerced by the lenient decode layer
// (observed shape deviation in live transcripts).
func TestUpdateTaskGoalToleratesStringifiedTokenBudget(t *testing.T) {
	tool, _ := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"action":"goal","objective":"ship it","token_budget":"5000"}`))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	if !strings.Contains(string(prepared.Call.Arguments), `"token_budget":5000`) {
		t.Fatalf("stringified budget must decode as a number: %s", prepared.Call.Arguments)
	}
}

func TestUpdatePlanExecuteQueuesSnapshot(t *testing.T) {
	tool, cell := newPlanTool(t)
	call := planCall(t, validPlanArgs)
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if result.CallID != call.ID {
		t.Fatalf("result CallID = %s, want %s", result.CallID, call.ID)
	}
	plan, ok := cell.Take()
	if !ok {
		t.Fatal("cell empty after Execute")
	}
	if len(plan.Items) != 3 || plan.CurrentInProgress() == nil {
		t.Fatalf("unexpected plan in cell: %+v", plan)
	}
}

func TestDrainPlanUpdatesAppliesSnapshotAndAudits(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	cell := NewPlanCell()
	loop := &Loop{Run: run, PlanCell: cell}

	plan, _, _, err := decodeUpdatePlanArgs(json.RawMessage(validPlanArgs))
	if err != nil {
		t.Fatalf("decode error: %v", err)
	}
	cell.Put(plan)
	loop.drainPlanUpdates()

	if len(run.Plan.Items) != 3 {
		t.Fatalf("run plan items = %d, want 3", len(run.Plan.Items))
	}
	var revised []domain.Plan
	for _, evt := range run.PendingEvents() {
		if evt.Type == domain.EventPlanRevised {
			var got domain.Plan
			if err := json.Unmarshal(evt.Payload, &got); err != nil {
				t.Fatalf("plan.revised payload undecodable: %v", err)
			}
			revised = append(revised, got)
		}
	}
	if len(revised) != 1 {
		t.Fatalf("plan.revised events = %d, want 1", len(revised))
	}
	if revised[0].Items[1].Status != domain.PlanItemInProgress {
		t.Fatalf("audited plan mismatch: %+v", revised[0].Items)
	}

	// A second snapshot in a later batch replaces the first.
	cell.Put(domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "read existing code", Status: domain.PlanItemDone},
		{Index: 1, Goal: "implement update_task", Status: domain.PlanItemDone},
		{Index: 2, Goal: "add tests", Status: domain.PlanItemInProgress},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Items[2].Status != domain.PlanItemInProgress {
		t.Fatalf("plan not replaced: %+v", run.Plan.Items)
	}
}

func TestDrainPlanUpdatesNilCellIsNoop(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	loop := &Loop{Run: run}
	loop.drainPlanUpdates() // must not panic
	if len(run.Plan.Items) != 0 {
		t.Fatalf("plan changed without a cell: %+v", run.Plan)
	}
}

func TestEffectiveMessagesInjectsPlanNote(t *testing.T) {
	run := newTestRun(domain.DefaultLimits())
	user := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser, Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "do the task"}}, CreatedAt: time.Now(),
	}
	user.Sequence = 1
	run.Messages = append(run.Messages, user)
	loop := &Loop{Run: run}

	// No plan: messages pass through untouched.
	messages, _, _ := loop.effectiveMessages(context.Background())
	if len(messages) != 1 {
		t.Fatalf("messages = %d without plan, want 1", len(messages))
	}

	run.Plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemDone, Evidence: []string{"verified"}},
		{Index: 1, Goal: "step two", Status: domain.PlanItemInProgress},
		{Index: 2, Goal: "step three", Status: domain.PlanItemTodo},
	}}
	messages, _, _ = loop.effectiveMessages(context.Background())
	if len(messages) != 2 {
		t.Fatalf("messages = %d with plan, want 2", len(messages))
	}
	note := messages[0]
	if note.Role != domain.RoleSystem {
		t.Fatalf("plan note role = %s, want system", note.Role)
	}
	text := strings.Join(note.TextParts(), "\n")
	for _, want := range []string{"[task plan] 1/3 done", "current: step two", "[done] step one", "evidence: verified"} {
		if !strings.Contains(text, want) {
			t.Fatalf("plan note missing %q:\n%s", want, text)
		}
	}
	// The note is ephemeral: the transcript must not carry it.
	if len(run.Messages) != 1 {
		t.Fatalf("plan note leaked into the transcript: %d messages", len(run.Messages))
	}

	// A complete plan is not re-injected.
	run.Plan.Items[1].Status = domain.PlanItemDone
	run.Plan.Items[2].Status = domain.PlanItemDone
	messages, _, _ = loop.effectiveMessages(context.Background())
	if len(messages) != 1 {
		t.Fatalf("messages = %d with complete plan, want 1", len(messages))
	}
}

// TestPlanStatusNoteTrimsEvidence pins the token-economy contract: only the
// two most recently completed steps keep evidence in the re-injected note,
// and long evidence is truncated — the note rides every model request, so
// older steps must collapse to their status line.
func TestPlanStatusNoteTrimsEvidence(t *testing.T) {
	longEvidence := strings.Repeat("x", 200)
	note := planStatusNote(domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "oldest", Status: domain.PlanItemDone, Evidence: []string{"old-evidence-should-vanish"}},
		{Index: 1, Goal: "older", Status: domain.PlanItemDone, Evidence: []string{"another-old-evidence"}},
		{Index: 2, Goal: "recent", Status: domain.PlanItemDone, Evidence: []string{"recent-evidence"}},
		{Index: 3, Goal: "latest", Status: domain.PlanItemDone, Evidence: []string{longEvidence}},
		{Index: 4, Goal: "current", Status: domain.PlanItemInProgress},
	}})
	if strings.Contains(note, "old-evidence-should-vanish") || strings.Contains(note, "another-old-evidence") {
		t.Fatalf("old evidence must collapse out of the note:\n%s", note)
	}
	if !strings.Contains(note, "recent-evidence") {
		t.Fatalf("previous done step keeps evidence:\n%s", note)
	}
	if strings.Contains(note, longEvidence) {
		t.Fatalf("long evidence must be truncated:\n%s", note)
	}
	if !strings.Contains(note, "…") {
		t.Fatalf("truncation marker missing:\n%s", note)
	}
	for _, want := range []string{"[done] oldest", "[done] older", "4/5 done"} {
		if !strings.Contains(note, want) {
			t.Fatalf("note missing %q:\n%s", want, note)
		}
	}
}

func TestRecoverRunReplaysPlanRevisions(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 26, 12, 0, 0, 0, time.UTC))
	sessionID := domain.NewSessionID()
	plan := domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemDone},
		{Index: 1, Goal: "step two", Status: domain.PlanItemInProgress},
	}}
	payload, err := domain.MarshalPayload(plan)
	if err != nil {
		t.Fatalf("marshal plan: %v", err)
	}
	events := []domain.Event{
		{ID: domain.NewEventID(), Sequence: 1, SessionID: sessionID, Type: domain.EventSessionCreated, Timestamp: clock.Now()},
		{ID: domain.NewEventID(), Sequence: 2, SessionID: sessionID, Type: domain.EventPlanRevised, Timestamp: clock.Now(), Payload: payload},
	}
	run, err := RecoverRun(sessionID, nil, nil, events, 2, domain.DefaultLimits(), clock, nil)
	if err != nil {
		t.Fatalf("RecoverRun error: %v", err)
	}
	if len(run.Plan.Items) != 2 || run.Plan.Items[1].Status != domain.PlanItemInProgress {
		t.Fatalf("plan not recovered: %+v", run.Plan)
	}
}
