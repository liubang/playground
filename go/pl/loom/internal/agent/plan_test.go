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
	`{"goal":"read existing code","status":"completed","evidence":["read goal.go"]},` +
	`{"goal":"implement update_task","status":"in_progress"},` +
	`{"goal":"add tests","status":"pending"}]}`

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
		`{"goal":"read existing code","status":"completed","evidence":"read goal.go"},` +
		`{"goal":"implement update_task","status":"completed","evidence":null},` +
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

// Cross-harness models emit the status names they learned elsewhere
// (Claude Code / Codex / Gemini all use pending/completed) or loom's own
// pre-rename todo/done; both are mapped onto the canonical trio instead
// of rejected, the same tolerance the string-evidence normalizer shows.
func TestUpdatePlanPrepareNormalizesStatusAliases(t *testing.T) {
	tool, _ := newPlanTool(t)
	args := `{"action":"plan","plan":[` +
		`{"goal":"a","status":"done","evidence":["ok"]},` +
		`{"goal":"b","status":"in-progress"},` +
		`{"goal":"c","status":"todo"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, args))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	plan, _, _, err := decodeUpdatePlanArgs(prepared.Call.Arguments)
	if err != nil {
		t.Fatalf("canonical arguments no longer decode: %v", err)
	}
	wants := []domain.PlanItemStatus{domain.PlanItemCompleted, domain.PlanItemInProgress, domain.PlanItemPending}
	for i, want := range wants {
		if plan.Items[i].Status != want {
			t.Fatalf("items[%d].Status = %q, want normalized %q", i, plan.Items[i].Status, want)
		}
	}
	// The canonical arguments carry the normalized names, so the Execute
	// re-decode sees only the canonical vocabulary.
	canonical := string(prepared.Call.Arguments)
	for _, stale := range []string{"todo", "done", "in-progress"} {
		if strings.Contains(canonical, stale) {
			t.Fatalf("canonical arguments still carry alias %q: %s", stale, canonical)
		}
	}
}

func TestUpdatePlanPrepareRejectsInvalidSnapshots(t *testing.T) {
	tool, _ := newPlanTool(t)
	cases := map[string]string{
		"single step":        `{"action":"plan","plan":[{"goal":"only one","status":"in_progress"}]}`,
		"empty plan":         `{"action":"plan","plan":[]}`,
		"missing plan":       `{"action":"plan"}`,
		"unknown action":     `{"action":"task","plan":[{"goal":"a","status":"pending"},{"goal":"b","status":"pending"}]}`,
		"unknown field":      `{"action":"plan","plan":[{"goal":"a","status":"pending"},{"goal":"b","status":"pending"}],"extra":1}`,
		"empty goal":         `{"action":"plan","plan":[{"goal":"  ","status":"pending"},{"goal":"b","status":"pending"}]}`,
		"bad status":         `{"action":"plan","plan":[{"goal":"a","status":"doing"},{"goal":"b","status":"pending"}]}`,
		"two in_progress":    `{"action":"plan","plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"in_progress"}]}`,
		"unknown item field": `{"action":"plan","plan":[{"goal":"a","status":"pending","step":"a"},{"goal":"b","status":"pending"}]}`,
		"numeric evidence":   `{"action":"plan","plan":[{"goal":"a","status":"completed","evidence":5},{"goal":"b","status":"pending"}]}`,
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
		`{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`
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
	longArgs := `{"action":"plan","title":"` + long + `","plan":[{"goal":"a","status":"pending"},{"goal":"b","status":"pending"}]}`
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
		{Index: 1, Goal: "b", Status: domain.PlanItemPending},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Title != "overall objective" {
		t.Fatalf("title = %q, want set at creation", run.Plan.Title)
	}

	// A revision without a title keeps the existing one.
	cell.Put(domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "a", Status: domain.PlanItemCompleted},
		{Index: 1, Goal: "b", Status: domain.PlanItemInProgress},
	}})
	loop.drainPlanUpdates()
	if run.Plan.Title != "overall objective" {
		t.Fatalf("title = %q after title-less revision, want preserved", run.Plan.Title)
	}

	// A revision with a new title replaces it.
	cell.Put(domain.Plan{Title: "renamed", Items: []domain.PlanItem{
		{Index: 0, Goal: "a", Status: domain.PlanItemCompleted},
		{Index: 1, Goal: "b", Status: domain.PlanItemCompleted},
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
		`"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`
	prepared, err := tool.Prepare(context.Background(), planCall(t, args))
	if err != nil {
		t.Fatalf("Prepare with goal fields error: %v", err)
	}
	// The stripped fields ride the prepared call for disclosure; the
	// canonical arguments carry the schema shape only — an internal field
	// there is dropped by the transcript rewrite's schema projection, and
	// the freshness re-Prepare then mismatches the signed form
	// (sess_eb40ddfc64b734371efe224695f6beeb).
	if strings.Contains(string(prepared.Call.Arguments), "ignored_fields") {
		t.Fatalf("canonical arguments must not carry ignored_fields: %s", prepared.Call.Arguments)
	}
	if len(prepared.IgnoredFields) != 3 || prepared.IgnoredFields[0] != "objective" ||
		prepared.IgnoredFields[1] != "token_budget" || prepared.IgnoredFields[2] != "status" {
		t.Fatalf("prepared.IgnoredFields = %v, want [objective token_budget status]", prepared.IgnoredFields)
	}
	// The freshness contract: the schema-projected replay (what the
	// transcript rewrite leaves behind) re-Prepares to the signed
	// canonical form byte-for-byte.
	projected := modelFacingCanonicalArgs(prepared)
	fresh, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID: prepared.Call.ID, Name: "update_task", Arguments: projected,
	})
	if err != nil {
		t.Fatalf("freshness re-Prepare of projected arguments error: %v", err)
	}
	matched, err := canonicalJSONEqual(fresh.Call.Arguments, prepared.Call.Arguments)
	if err != nil || !matched {
		t.Fatalf("freshness replay diverges: fresh=%s signed=%s", fresh.Call.Arguments, prepared.Call.Arguments)
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

// The goal action mirrors the plan action's contract: a stray plan-owned
// field (title) is stripped and disclosed via the prepared call, never
// the canonical arguments, so the schema-projected replay re-Prepares
// identically under the freshness check (sess_eb40ddfc64b734371efe224695f6beeb).
func TestUpdateGoalPrepareToleratesPlanFields(t *testing.T) {
	tool, _ := newPlanTool(t)
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"action":"goal","objective":"ship it","title":"stray title"}`))
	if err != nil {
		t.Fatalf("Prepare with plan fields error: %v", err)
	}
	if strings.Contains(string(prepared.Call.Arguments), "ignored_fields") {
		t.Fatalf("canonical arguments must not carry ignored_fields: %s", prepared.Call.Arguments)
	}
	if len(prepared.IgnoredFields) != 1 || prepared.IgnoredFields[0] != "title" {
		t.Fatalf("prepared.IgnoredFields = %v, want [title]", prepared.IgnoredFields)
	}
	projected := modelFacingCanonicalArgs(prepared)
	fresh, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID: prepared.Call.ID, Name: "update_task", Arguments: projected,
	})
	if err != nil {
		t.Fatalf("freshness re-Prepare of projected arguments error: %v", err)
	}
	matched, err := canonicalJSONEqual(fresh.Call.Arguments, prepared.Call.Arguments)
	if err != nil || !matched {
		t.Fatalf("freshness replay diverges: fresh=%s signed=%s", fresh.Call.Arguments, prepared.Call.Arguments)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	var payload struct {
		Applied       bool     `json:"applied"`
		IgnoredFields []string `json:"ignored_fields"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("result payload undecodable: %v", err)
	}
	if !payload.Applied || len(payload.IgnoredFields) != 1 || payload.IgnoredFields[0] != "title" {
		t.Fatalf("result payload = %+v, want applied with ignored_fields [title]", payload)
	}
}

// Models also hoist item-level properties to the top level while
// mirroring the schema (sess_5fb89a53cf2ed7ae37ac02d3a52eb3c1: a
// top-level "evidence": null bounced two prepare retries). The hoisted
// field is stripped like a cross-action stray — silently when null, with
// disclosure when it carried a real value — and the canonical arguments
// stay schema-shaped so the freshness replay is idempotent.
func TestUpdatePlanPrepareToleratesHoistedItemFields(t *testing.T) {
	tool, cell := newPlanTool(t)

	// A null hoist is dropped silently, exactly like null cross-action
	// mirrors.
	prepared, err := tool.Prepare(context.Background(), planCall(t,
		`{"action":"plan","evidence":null,`+
			`"plan":[{"goal":"a","status":"in_progress","evidence":["did a"]},{"goal":"b","status":"pending"}]}`))
	if err != nil {
		t.Fatalf("Prepare with hoisted null evidence error: %v", err)
	}
	var topLevel map[string]json.RawMessage
	if err := json.Unmarshal(prepared.Call.Arguments, &topLevel); err != nil {
		t.Fatalf("canonical arguments undecodable: %v", err)
	}
	if _, ok := topLevel["evidence"]; ok {
		t.Fatalf("hoisted evidence must not survive into canonical arguments: %s", prepared.Call.Arguments)
	}
	if len(prepared.IgnoredFields) != 0 {
		t.Fatalf("null hoist must be silent, prepared.IgnoredFields = %v", prepared.IgnoredFields)
	}
	// The item-level evidence inside the plan survives untouched.
	if !strings.Contains(string(prepared.Call.Arguments), `"evidence":["did a"]`) {
		t.Fatalf("item-level evidence lost: %s", prepared.Call.Arguments)
	}
	projected := modelFacingCanonicalArgs(prepared)
	fresh, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID: prepared.Call.ID, Name: "update_task", Arguments: projected,
	})
	if err != nil {
		t.Fatalf("freshness re-Prepare of projected arguments error: %v", err)
	}
	if matched, err := canonicalJSONEqual(fresh.Call.Arguments, prepared.Call.Arguments); err != nil || !matched {
		t.Fatalf("freshness replay diverges: fresh=%s signed=%s", fresh.Call.Arguments, prepared.Call.Arguments)
	}
	if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if _, ok := cell.Take(); !ok {
		t.Fatal("plan cell empty after Execute")
	}

	// A hoist carrying a real value is stripped with disclosure.
	prepared, err = tool.Prepare(context.Background(), planCall(t,
		`{"action":"plan","evidence":["top-level stray"],`+
			`"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`))
	if err != nil {
		t.Fatalf("Prepare with hoisted evidence value error: %v", err)
	}
	if len(prepared.IgnoredFields) != 1 || prepared.IgnoredFields[0] != "evidence" {
		t.Fatalf("prepared.IgnoredFields = %v, want [evidence]", prepared.IgnoredFields)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s, want success: %+v", result.Status, result.Error)
	}
	if !strings.Contains(result.Content[0].Text, `"ignored_fields":["evidence"]`) {
		t.Fatalf("result must disclose the hoisted field: %s", result.Content[0].Text)
	}
}

// Null-valued cross-action fields are removed silently: models mirror
// optional properties they have no value for as explicit nulls, and
// disclosing those would be noise on every call.
func TestUpdatePlanPrepareIgnoresNullGoalFieldsSilently(t *testing.T) {
	tool, _ := newPlanTool(t)
	args := `{"action":"plan","objective":null,"token_budget":null,"status":null,` +
		`"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`
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
		`{"plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`))
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
		`{"objective":"ship it","plan":[{"goal":"a","status":"in_progress"},{"goal":"b","status":"pending"}]}`))
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	canonical := string(prepared.Call.Arguments)
	if !strings.Contains(canonical, `"action":"plan"`) {
		t.Fatalf("ambiguous payload must infer the plan action: %s", canonical)
	}
	if strings.Contains(canonical, "ignored_fields") {
		t.Fatalf("canonical arguments must not carry ignored_fields: %s", canonical)
	}
	if len(prepared.IgnoredFields) != 1 || prepared.IgnoredFields[0] != "objective" {
		t.Fatalf("prepared.IgnoredFields = %v, want [objective]", prepared.IgnoredFields)
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
		{Index: 0, Goal: "read existing code", Status: domain.PlanItemCompleted},
		{Index: 1, Goal: "implement update_task", Status: domain.PlanItemCompleted},
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
		{Index: 0, Goal: "step one", Status: domain.PlanItemCompleted, Evidence: []string{"verified"}},
		{Index: 1, Goal: "step two", Status: domain.PlanItemInProgress},
		{Index: 2, Goal: "step three", Status: domain.PlanItemPending},
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
	for _, want := range []string{"[task plan] 1/3 completed", "current: step two", "[completed] step one", "evidence: verified"} {
		if !strings.Contains(text, want) {
			t.Fatalf("plan note missing %q:\n%s", want, text)
		}
	}
	// The note is ephemeral: the transcript must not carry it.
	if len(run.Messages) != 1 {
		t.Fatalf("plan note leaked into the transcript: %d messages", len(run.Messages))
	}

	// A complete plan is not re-injected.
	run.Plan.Items[1].Status = domain.PlanItemCompleted
	run.Plan.Items[2].Status = domain.PlanItemCompleted
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
		{Index: 0, Goal: "oldest", Status: domain.PlanItemCompleted, Evidence: []string{"old-evidence-should-vanish"}},
		{Index: 1, Goal: "older", Status: domain.PlanItemCompleted, Evidence: []string{"another-old-evidence"}},
		{Index: 2, Goal: "recent", Status: domain.PlanItemCompleted, Evidence: []string{"recent-evidence"}},
		{Index: 3, Goal: "latest", Status: domain.PlanItemCompleted, Evidence: []string{longEvidence}},
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
	for _, want := range []string{"[completed] oldest", "[completed] older", "4/5 completed"} {
		if !strings.Contains(note, want) {
			t.Fatalf("note missing %q:\n%s", want, note)
		}
	}
}

func TestRecoverRunReplaysPlanRevisions(t *testing.T) {
	clock := domain.NewFakeClock(time.Date(2026, 7, 26, 12, 0, 0, 0, time.UTC))
	sessionID := domain.NewSessionID()
	plan := domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "step one", Status: domain.PlanItemCompleted},
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
