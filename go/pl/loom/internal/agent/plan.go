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
	"encoding/json"
	"fmt"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// PlanCell is the mailbox between the update_task tool (which cannot see
// the Run) and the loop (which owns it). One pending snapshot is kept; a
// second update in the same batch replaces it — the tool submits a full
// snapshot each call, so the last one wins by construction.
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

// drainPlanUpdates applies a pending plan snapshot to the run and records
// the audit event. Called after every tool batch, next to drainGoalUpdates.
// A snapshot that somehow fails validation here (the tool already validated
// it) is dropped with a warning instead of killing the run.
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
	l.runaway.markProgress(l.Run.Clock)
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
			fmt.Fprintf(&sb, " — evidence: %s", toolkit.Ellipsize(strings.Join(item.Evidence, "; "), planNoteEvidenceMaxLen))
		}
		sb.WriteString("\n")
	}
	// Guidance is deliberately stage-boundary rather than "update
	// immediately": every update is a full snapshot that persists in the
	// transcript, so updates belong at step transitions, not mid-step.
	sb.WriteString("Rule: update the plan at step boundaries (mark the finished step done, start the next); avoid mid-step or back-to-back revisions.")
	return sb.String()
}

// --- update_task plan-action arguments ---

// updatePlanArgsItem is the wire form of one plan step: the model submits
// goals without indexes; the tool assigns them in order.
type updatePlanArgsItem struct {
	Goal     string   `json:"goal"`
	Status   string   `json:"status"`
	Evidence []string `json:"evidence"`
}

// updatePlanArgs is the canonical wire form of an update_task call with
// action "plan". The Action field pins the canonical arguments to the plan
// action.
type updatePlanArgs struct {
	Action string               `json:"action"`
	Title  string               `json:"title"`
	Plan   []updatePlanArgsItem `json:"plan"`
}

// updatePlanArgsRawItem is the decoding form of a plan step: Evidence stays
// raw so a bare string — a common model deviation from the array the schema
// declares — can be normalized instead of rejected (a strict decode error
// costs a whole tool round-trip, and the model almost always retries with
// exactly this fix).
type updatePlanArgsRawItem struct {
	Goal     string          `json:"goal"`
	Status   string          `json:"status"`
	Evidence json.RawMessage `json:"evidence"`
}

// updatePlanArgsRaw is the decoding form of a plan-action call: the Action
// field is decoded so the strict decoder rejects the goal action's fields
// as unknown; it is validated against taskActionPlan.
type updatePlanArgsRaw struct {
	Action string                  `json:"action"`
	Title  string                  `json:"title"`
	Plan   []updatePlanArgsRawItem `json:"plan"`
}

// decodePlanItemEvidence normalizes the evidence field: absent/null → nil,
// an array of strings → as-is, a bare string → wrapped as a one-element
// slice.
func decodePlanItemEvidence(raw json.RawMessage) ([]string, error) {
	if len(raw) == 0 || string(raw) == "null" {
		return nil, nil
	}
	if raw[0] == '"' {
		var single string
		if err := json.Unmarshal(raw, &single); err != nil {
			return nil, err
		}
		return []string{single}, nil
	}
	var list []string
	if err := json.Unmarshal(raw, &list); err != nil {
		return nil, err
	}
	return list, nil
}

// decodeUpdatePlanArgs parses, normalizes, and validates a plan snapshot:
// unknown fields are rejected, indexes are assigned in array order, and the
// result must satisfy Plan.Validate (at most one in_progress).
func decodeUpdatePlanArgs(raw json.RawMessage) (domain.Plan, json.RawMessage, error) {
	var args updatePlanArgsRaw
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput, "invalid update_task plan arguments", domain.WithCause(err))
	}
	if args.Action != taskActionPlan {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("plan arguments require action %q, got %q", taskActionPlan, args.Action))
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
	canonicalArgs := updatePlanArgs{Action: args.Action, Title: args.Title, Plan: make([]updatePlanArgsItem, 0, len(args.Plan))}
	for i, raw := range args.Plan {
		goal := strings.TrimSpace(raw.Goal)
		if goal == "" {
			return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("plan step %d: goal is required", i+1))
		}
		evidence, err := decodePlanItemEvidence(raw.Evidence)
		if err != nil {
			return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("plan step %d: invalid evidence", i+1), domain.WithCause(err))
		}
		item := domain.PlanItem{
			Index:  i,
			Goal:   goal,
			Status: domain.PlanItemStatus(strings.TrimSpace(raw.Status)),
		}
		for _, ev := range evidence {
			if trimmed := strings.TrimSpace(ev); trimmed != "" {
				item.Evidence = append(item.Evidence, trimmed)
			}
		}
		items = append(items, item)
		canonicalArgs.Plan = append(canonicalArgs.Plan, updatePlanArgsItem{Goal: raw.Goal, Status: raw.Status, Evidence: evidence})
	}
	plan := domain.Plan{Title: title, Items: items}
	if err := plan.Validate(); err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInvalidInput, "invalid plan snapshot", domain.WithCause(err))
	}
	canonical, err := json.Marshal(canonicalArgs)
	if err != nil {
		return domain.Plan{}, nil, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	return plan, canonical, nil
}
