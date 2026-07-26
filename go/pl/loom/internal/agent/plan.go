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
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PlanCell is the mailbox between the update_plan tool (which cannot see the
// Run) and the loop (which owns it). One pending snapshot is kept; a second
// update in the same batch replaces it — the tool submits a full snapshot
// each call, so the last one wins by construction.
type PlanCell struct {
	mu   sync.Mutex
	plan domain.Plan
	has  bool
}

// NewPlanCell creates an empty plan mailbox.
func NewPlanCell() *PlanCell { return &PlanCell{} }

// Put stores a plan snapshot for the loop to drain.
func (c *PlanCell) Put(p domain.Plan) {
	c.mu.Lock()
	c.plan, c.has = p, true
	c.mu.Unlock()
}

// Take returns and clears the pending snapshot, if any.
func (c *PlanCell) Take() (domain.Plan, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	p, ok := c.plan, c.has
	c.plan, c.has = domain.Plan{}, false
	return p, ok
}

// drainPlanUpdates applies a pending update_plan snapshot to the run and
// records the audit event. Called after every tool batch, next to
// drainGoalUpdates. A snapshot that somehow fails validation here (the tool
// already validated it) is dropped with a warning instead of killing the run.
func (l *Loop) drainPlanUpdates() {
	if l.PlanCell == nil {
		return
	}
	plan, ok := l.PlanCell.Take()
	if !ok {
		return
	}
	if err := plan.Validate(); err != nil {
		if l.Logger != nil {
			l.Logger.Warn("dropping invalid plan snapshot", "error", err)
		}
		return
	}
	if plan.Title == "" {
		// Revisions that omit the title keep the one set at creation.
		plan.Title = l.Run.Plan.Title
	}
	l.Run.Plan = plan
	l.planRevisedThisRun = true
	l.Run.appendEvent(domain.EventPlanRevised, plan)
}

// planStatusNote renders the ephemeral system message that re-injects the
// current plan into every model request. It is rebuilt per request, never
// persisted, so it survives context compaction and crash recovery for free.
//
// Token economy: the note rides EVERY model request, so evidence is kept
// only for the two most recently completed steps and truncated — older
// steps collapse to their status line (codex injects nothing at all; loom
// keeps the note because the plan must survive compaction, but not the
// full evidence trail).
const (
	planNoteEvidenceItems  = 2
	planNoteEvidenceMaxLen = 80
)

func planStatusNote(plan domain.Plan) string {
	done := 0
	lastDone := -1
	for i, item := range plan.Items {
		if item.Status == domain.PlanItemDone {
			done++
			lastDone = i
		}
	}
	// Evidence rides only with the most recent done steps.
	prevDone := -1
	for i := lastDone - 1; i >= 0 && prevDone < 0; i-- {
		if plan.Items[i].Status == domain.PlanItemDone {
			prevDone = i
		}
	}
	current := "none"
	if item := plan.CurrentInProgress(); item != nil {
		current = item.Goal
	}
	var sb strings.Builder
	if plan.Title != "" {
		fmt.Fprintf(&sb, "[task plan] %s: %d/%d done; current: %s\n", plan.Title, done, len(plan.Items), current)
	} else {
		fmt.Fprintf(&sb, "[task plan] %d/%d done; current: %s\n", done, len(plan.Items), current)
	}
	for i, item := range plan.Items {
		fmt.Fprintf(&sb, "%d. [%s] %s", i+1, item.Status, item.Goal)
		if item.Status == domain.PlanItemDone && len(item.Evidence) > 0 && (i == lastDone || (planNoteEvidenceItems > 1 && i == prevDone)) {
			fmt.Fprintf(&sb, " — evidence: %s", truncateRunes(strings.Join(item.Evidence, "; "), planNoteEvidenceMaxLen))
		}
		sb.WriteString("\n")
	}
	// Guidance is deliberately stage-boundary rather than "update
	// immediately": every update is a full snapshot that persists in the
	// transcript, so updates belong at step transitions, not mid-step.
	sb.WriteString("Rule: update the plan at step boundaries (mark the finished step done, start the next); avoid mid-step or back-to-back revisions.")
	return sb.String()
}

// truncateRunes bounds s to max runes, marking truncation.
func truncateRunes(s string, max int) string {
	r := []rune(s)
	if len(r) <= max {
		return s
	}
	return string(r[:max]) + "…"
}

// --- update_plan tool ---

// updatePlanArgsItem is the wire form of one plan step: the model submits
// goals without indexes; the tool assigns them in order.
type updatePlanArgsItem struct {
	Goal     string   `json:"goal"`
	Status   string   `json:"status"`
	Evidence []string `json:"evidence"`
}

type updatePlanArgs struct {
	Title string               `json:"title"`
	Plan  []updatePlanArgsItem `json:"plan"`
}

// UpdatePlanTool lets the model maintain the run's task plan. Mutations go
// through the PlanCell; the loop applies them after the tool batch, so the
// tool itself is side-effect-free w.r.t. the run state.
type UpdatePlanTool struct {
	def  domain.ToolDefinition
	cell *PlanCell
}

// NewUpdatePlanTool creates the tool bound to the given cell.
func NewUpdatePlanTool(cell *PlanCell) (*UpdatePlanTool, error) {
	if cell == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "plan cell is required")
	}
	def := domain.ToolDefinition{
		Name: "update_plan",
		Description: "Update the task plan: a checklist you maintain to track progress on multi-step work. " +
			"Submit the COMPLETE plan snapshot on every call (full replacement, not a diff). " +
		"Rules: skip this tool for straightforward tasks (roughly the easiest 25%); never create single-step plans; " +
		"keep at most one step in_progress — mark the current step done (with brief evidence) before starting the next; " +
		"update at step boundaries: each call is a full snapshot that stays in the transcript, so revise when a step completes or the plan changes — not mid-step. " +
			"Give the plan a short 'title' (a few words naming the overall objective) when creating it; " +
			"later revisions may omit it to keep the existing title. " +
			"Only mark a step done after its deliverable actually exists (edits applied, commands verified, conclusions " +
			"written); for final summary/report steps, write the visible answer FIRST, then call update_plan in the same " +
			"turn — never pre-mark a step as done. " +
			"The plan persists across turns, context compactions, and crash recovery, and its latest state is " +
			"automatically shown to you before every model call, so never repeat it in your messages.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"title":{"type":"string","maxLength":120},"plan":{"type":"array","minItems":2,"items":{"type":"object","additionalProperties":false,"properties":{"goal":{"type":"string","minLength":1,"maxLength":1024},"status":{"type":"string","enum":["todo","in_progress","done"]},"evidence":{"type":"array","items":{"type":"string","maxLength":1024}}},"required":["goal","status"]}}},"required":["plan"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"applied":{"type":"boolean"},"items":{"type":"integer"},"note":{"type":"string"}},"required":["applied","note"]}`),
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	return &UpdatePlanTool{def: def, cell: cell}, nil
}

// Definition returns the tool definition.
func (t *UpdatePlanTool) Definition() domain.ToolDefinition { return t.def }

// Prepare validates and canonicalizes the call; it is side-effect-free.
func (t *UpdatePlanTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	plan, canonical, err := decodeUpdatePlanArgs(call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	call.Arguments = canonical
	sum := sha256.Sum256(canonical)
	desc := fmt.Sprintf("Update plan (%d steps)", len(plan.Items))
	if current := plan.CurrentInProgress(); current != nil {
		desc = fmt.Sprintf("Update plan (%d steps): %s", len(plan.Items), current.Goal)
	}
	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: desc,
		ArgsHash:     hex.EncodeToString(sum[:])[:16],
	}, nil
}

// Execute queues the snapshot for the loop. The tool result confirms
// acceptance; the resulting plan state is reported back through the
// plan.revised audit event and the plan status note of the next request.
func (t *UpdatePlanTool) Execute(_ context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := domain.RealClock{}.Now()
	plan, _, err := decodeUpdatePlanArgs(prepared.Call.Arguments)
	if err != nil {
		return updatePlanError(prepared.Call.ID, startedAt, err)
	}
	t.cell.Put(plan)
	payload := map[string]any{
		"applied": true,
		"items":   len(plan.Items),
		"note":    "plan update accepted; it takes effect after this tool batch",
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return updatePlanError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode result", domain.WithCause(err)))
	}
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: domain.RealClock{}.Now(),
	}
}

// decodeUpdatePlanArgs parses, normalizes, and validates a plan snapshot:
// unknown fields are rejected, indexes are assigned in array order, and the
// result must satisfy Plan.Validate (at most one in_progress).
func decodeUpdatePlanArgs(raw json.RawMessage) (domain.Plan, json.RawMessage, error) {
	var args updatePlanArgs
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput, "invalid update_plan arguments", domain.WithCause(err))
	}
	if len(args.Plan) < 2 {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("plan must contain at least 2 steps (got %d); never make single-step plans", len(args.Plan)))
	}
	title := strings.TrimSpace(args.Title)
	if titleRunes := []rune(title); len(titleRunes) > 120 {
		title = string(titleRunes[:120])
	}
	items := make([]domain.PlanItem, 0, len(args.Plan))
	for i, raw := range args.Plan {
		goal := strings.TrimSpace(raw.Goal)
		if goal == "" {
			return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("plan step %d: goal is required", i+1))
		}
		item := domain.PlanItem{
			Index:  i,
			Goal:   goal,
			Status: domain.PlanItemStatus(strings.TrimSpace(raw.Status)),
		}
		for _, ev := range raw.Evidence {
			if trimmed := strings.TrimSpace(ev); trimmed != "" {
				item.Evidence = append(item.Evidence, trimmed)
			}
		}
		items = append(items, item)
	}
	plan := domain.Plan{Title: title, Items: items}
	if err := plan.Validate(); err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput, "invalid plan snapshot", domain.WithCause(err))
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	return plan, canonical, nil
}

func updatePlanError(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	var agentErr *domain.AgentError
	code, message := string(domain.ErrInternal), err.Error()
	if domain.As(err, &agentErr) {
		code, message = string(agentErr.Code), agentErr.Message
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusError,
		Error:      &domain.ToolError{Code: code, Message: message},
		StartedAt:  startedAt,
		FinishedAt: domain.RealClock{}.Now(),
	}
}
