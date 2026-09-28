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
// Created: 2026/09/27

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// update_task action discriminators.
const (
	taskActionGoal = "goal"
	taskActionPlan = "plan"
)

// UpdateTaskTool unifies the former update_goal/update_plan pair behind one
// action-dispatched tool: action="goal" mutates the cross-turn goal through
// the GoalCell, action="plan" replaces the plan snapshot through the
// PlanCell. Both mutations are drained by the loop after the tool batch, so
// the tool itself stays side-effect-free w.r.t. the run state.
type UpdateTaskTool struct {
	def      domain.ToolDefinition
	goalCell *GoalCell
	planCell *PlanCell
}

// NewUpdateTaskTool creates the tool bound to the given cells.
func NewUpdateTaskTool(goalCell *GoalCell, planCell *PlanCell) (*UpdateTaskTool, error) {
	if goalCell == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "goal cell is required")
	}
	if planCell == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "plan cell is required")
	}
	def := domain.ToolDefinition{
		Name: "update_task",
		Description: "Manage the run's task state: a cross-turn goal (action='goal') and a step-by-step plan (action='plan'). " +
			"action='goal' sets, redirects, or closes a cross-turn goal for long-running work. While a goal is active the run " +
			"automatically continues with a reminder of the objective after each pause (the goal persists across turns " +
			"and compactions), so use it only for multi-step tasks that must reach a verified end state — never for " +
			"trivial single-step work. Set 'objective' to activate or redirect; optional 'token_budget' counts cumulative " +
			"input+output tokens and, when exhausted, marks the goal budget_limited and gives you one final turn to " +
			"summarize — it never hard-stops you mid-work. Call status='complete' only with requirement-by-requirement " +
			"evidence from the current state, or status='blocked' only when truly stuck without user input; when closing " +
			"with a status, 'objective' may carry the final summary recorded on the goal. " +
			"action='plan' updates the task plan: the checklist you maintain for the current multi-step task. " +
			"Submit the COMPLETE plan snapshot on every call — each call fully replaces the previous plan (not a diff). " +
			"'plan' lists the steps (at least 2); each step carries a goal, a status ('pending' | 'in_progress' | 'completed'), " +
			"and optional evidence notes (one-line verifications, a list of strings) for a completed step. " +
			"'title' is a few words naming the overall objective — required when you first create the plan, omittable on later revisions. " +
			"The plan's latest state is automatically shown to you before every model call and persists across turns and compaction; " +
			"when to plan, step granularity, and in_progress discipline follow the Task Planning guidance.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{` +
			`"action":{"type":"string","enum":["goal","plan"],"description":"goal: manage the cross-turn goal; plan: replace the task plan snapshot."},` +
			`"objective":{"type":"string","minLength":1,"maxLength":8192,"description":"goal action: objective text, or the final summary when closing with a status."},` +
			`"token_budget":{"type":"integer","minimum":1,"description":"goal action: cumulative input+output token budget for the goal."},` +
			`"status":{"type":"string","enum":["complete","blocked"],"description":"goal action: close the goal with this outcome."},` +
			`"title":{"type":"string","maxLength":120,"description":"plan action: short plan title (required on creation, omittable on revisions)."},` +
			`"plan":{"type":"array","minItems":2,"description":"plan action: the complete step snapshot.","items":{"type":"object","additionalProperties":false,"properties":{"goal":{"type":"string","minLength":1,"maxLength":1024},"status":{"type":"string","enum":["pending","in_progress","completed"]},"evidence":{"type":"array","items":{"type":"string","maxLength":1024}}},"required":["goal","status"]}}` +
			`},"required":["action"]}`),
		Source: domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &UpdateTaskTool{def: def, goalCell: goalCell, planCell: planCell}, nil
}

// Definition returns the tool definition.
func (t *UpdateTaskTool) Definition() domain.ToolDefinition { return t.def }

// decodeUpdateTaskAction peeks the action discriminator without rejecting
// unknown sibling fields — the action-specific strict decoders own that.
// A missing discriminator is inferred from the field set.
func decodeUpdateTaskAction(raw json.RawMessage) (string, error) {
	var envelope struct {
		Action string `json:"action"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return "", domain.NewError(domain.ErrInvalidInput, "invalid update_task arguments", domain.WithCause(err))
	}
	switch envelope.Action {
	case taskActionGoal, taskActionPlan:
		return envelope.Action, nil
	case "":
		return inferUpdateTaskAction(raw)
	default:
		return "", domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("unknown action %q: must be %q or %q", envelope.Action, taskActionGoal, taskActionPlan))
	}
}

// inferUpdateTaskAction recovers a missing action discriminator from the
// field set: a plan payload means the plan action, goal-owned fields mean
// the goal action. Models occasionally skip the discriminator when the
// payload itself already names the operation.
func inferUpdateTaskAction(raw json.RawMessage) (string, error) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		return "", domain.NewError(domain.ErrInvalidInput, "invalid update_task arguments", domain.WithCause(err))
	}
	for _, name := range crossActionFields[taskActionGoal] { // plan-owned fields
		if _, ok := fields[name]; ok {
			return taskActionPlan, nil
		}
	}
	for _, name := range crossActionFields[taskActionPlan] { // goal-owned fields
		if _, ok := fields[name]; ok {
			return taskActionGoal, nil
		}
	}
	return "", domain.NewError(domain.ErrInvalidInput, `action is required: "goal" or "plan"`)
}

// crossActionFields maps each action to the top-level fields owned by the
// OTHER action, in canonical disclosure order.
var crossActionFields = map[string][]string{
	taskActionGoal: {"title", "plan"},
	taskActionPlan: {"objective", "token_budget", "status"},
}

// stripCrossActionFields removes fields owned by the other action before
// the strict decode, returning the cleaned JSON and the stripped field
// names (in canonical order). The flat schema invites models to mirror
// every visible property into one call; a strict rejection of the stray
// fields costs a whole tool round-trip per retry — and models tend to
// repeat the mistake (the grep normalizer precedent). Stripping with
// in-band disclosure (ignored_fields in the result) fixes the call
// without hiding the correction. Unparseable input passes through
// untouched — the strict decoder owns reporting it.
//
// Null-valued strays are removed silently: models also mirror optional
// properties they have no value for as explicit nulls (grep treats them
// the same way), and disclosing those would be noise. Only fields that
// carried a real value are reported.
func stripCrossActionFields(raw json.RawMessage, action string) (json.RawMessage, []string) {
	stray := crossActionFields[action]
	if len(stray) == 0 {
		return raw, nil
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		return raw, nil
	}
	var stripped []string
	removed := false
	for _, name := range stray {
		value, ok := fields[name]
		if !ok {
			continue
		}
		delete(fields, name)
		removed = true
		if !isJSONNullRaw(value) {
			stripped = append(stripped, name)
		}
	}
	if !removed {
		return raw, nil
	}
	clean, err := json.Marshal(fields)
	if err != nil {
		return raw, nil
	}
	return clean, stripped
}

// isJSONNullRaw reports whether raw is the JSON null literal (mirrors
// builtin.isJSONNull; duplicated to keep the agent package free of tool
// internals).
func isJSONNullRaw(raw json.RawMessage) bool {
	return strings.TrimSpace(string(raw)) == "null"
}

// updateTaskNormalizeShape mirrors the flat update_task schema so the
// lenient normalizer can repair shape deviations (null-mirrored fields, a
// stringified token_budget) before action inference and dispatch. Dropping
// nulls ahead of inference keeps a null "plan" from masquerading as a
// plan-action payload; the strip step still reports real cross-action
// fields, preserving the "loud on real values, silent on nulls" contract.
type updateTaskNormalizeShape struct {
	Action      string                  `json:"action"`
	Objective   string                  `json:"objective"`
	TokenBudget int64                   `json:"token_budget"`
	Status      string                  `json:"status"`
	Title       string                  `json:"title"`
	Plan        []updatePlanArgsRawItem `json:"plan"`
}

var updateTaskNormalizeShapeType = reflect.TypeOf(updateTaskNormalizeShape{})

// Prepare validates and canonicalizes the call; it is side-effect-free.
func (t *UpdateTaskTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	normalized := toolkit.NormalizeArgsJSON(call.Arguments, updateTaskNormalizeShapeType)
	action, err := decodeUpdateTaskAction(normalized)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	var canonical json.RawMessage
	var desc string
	switch action {
	case taskActionGoal:
		args, err := decodeUpdateGoalArgs(normalized)
		if err != nil {
			return domain.PreparedCall{}, err
		}
		canonical, err = json.Marshal(args)
		if err != nil {
			return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
		}
		desc = goalApprovalDesc(args)
	case taskActionPlan:
		plan, planCanonical, _, err := decodeUpdatePlanArgs(normalized)
		if err != nil {
			return domain.PreparedCall{}, err
		}
		canonical = planCanonical
		desc = planApprovalDesc(plan)
	}
	call.Arguments = canonical
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: desc,
		ArgsHash:     toolkit.ArgsFingerprint(canonical),
	}, nil
}

// goalApprovalDesc renders the one-line approval/audit summary for a goal
// mutation.
func goalApprovalDesc(args updateGoalArgs) string {
	if args.Status != "" {
		return fmt.Sprintf("Mark goal %s", args.Status)
	}
	// Rune-aware truncation: a byte cut can split a multi-byte rune.
	if len([]rune(args.Objective)) > 60 {
		return fmt.Sprintf("Set goal: %s", toolkit.Ellipsize(args.Objective, 60))
	}
	return fmt.Sprintf("Set goal: %s", args.Objective)
}

// planApprovalDesc renders the one-line approval/audit summary for a plan
// snapshot.
func planApprovalDesc(plan domain.Plan) string {
	if current := plan.CurrentInProgress(); current != nil {
		return fmt.Sprintf("Update plan (%d steps): %s", len(plan.Items), current.Goal)
	}
	return fmt.Sprintf("Update plan (%d steps)", len(plan.Items))
}

// Execute queues the mutation for the loop. The tool result confirms
// acceptance; the resulting state is reported back through the
// goal.updated/plan.revised audit events and the next request's goal
// continuation prompt or plan status note.
func (t *UpdateTaskTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := domain.RealClock{}.Now()
	action, err := decodeUpdateTaskAction(prepared.Call.Arguments)
	if err != nil {
		return toolErrorResult(prepared.Call.ID, startedAt, err)
	}

	var payload map[string]any
	switch action {
	case taskActionGoal:
		args, err := decodeUpdateGoalArgs(prepared.Call.Arguments)
		if err != nil {
			return toolErrorResult(prepared.Call.ID, startedAt, err)
		}
		update := GoalUpdate{Objective: args.Objective, TokenBudget: args.TokenBudget}
		if args.Status != "" {
			update.Close = domain.GoalStatus(args.Status)
		}
		t.goalCell.Put(update)
		payload = map[string]any{
			"action":       action,
			"applied":      true,
			"objective":    args.Objective,
			"token_budget": args.TokenBudget,
			"close":        args.Status,
			"note":         "goal update accepted; it takes effect after this tool batch",
		}
		if len(args.IgnoredFields) > 0 {
			payload["ignored_fields"] = args.IgnoredFields
		}
	case taskActionPlan:
		plan, _, ignored, err := decodeUpdatePlanArgs(prepared.Call.Arguments)
		if err != nil {
			return toolErrorResult(prepared.Call.ID, startedAt, err)
		}
		t.planCell.Put(plan)
		payload = map[string]any{
			"action":  action,
			"applied": true,
			"items":   len(plan.Items),
			"note":    "plan update accepted; it takes effect after this tool batch",
		}
		if len(ignored) > 0 {
			payload["ignored_fields"] = ignored
		}
	}

	raw, err := json.Marshal(payload)
	if err != nil {
		return toolErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode result", domain.WithCause(err)))
	}
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: domain.RealClock{}.Now(),
	}
}
