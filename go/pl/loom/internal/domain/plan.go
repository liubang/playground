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

package domain

import "fmt"

// PlanItemStatus tracks the state of a single plan item.
type PlanItemStatus string

// The canonical statuses deliberately match the naming mainstream agents
// (Claude Code TodoWrite, Codex update_plan, Gemini write_todos) converged
// on: models produce them far more reliably than loom's original
// todo/done pair, which cross-harness models kept "correcting" to
// pending/completed.
const (
	PlanItemPending    PlanItemStatus = "pending"
	PlanItemInProgress PlanItemStatus = "in_progress"
	PlanItemCompleted  PlanItemStatus = "completed"
)

// legacyPlanItemStatuses maps loom's pre-rename statuses and common
// cross-harness variants onto the canonical trio. Models trained on other
// agents' transcripts emit these no matter what the schema says; mapping
// them costs nothing and saves a full tool round-trip per occurrence.
var legacyPlanItemStatuses = map[PlanItemStatus]PlanItemStatus{
	"todo":        PlanItemPending,
	"done":        PlanItemCompleted,
	"in-progress": PlanItemInProgress,
	"inprogress":  PlanItemInProgress,
	"complete":    PlanItemCompleted,
	"finished":    PlanItemCompleted,
}

// NormalizePlanItemStatus maps legacy loom names ("todo", "done") and
// common variants onto the canonical statuses; unrecognized values pass
// through unchanged so validation still rejects them.
func NormalizePlanItemStatus(s PlanItemStatus) PlanItemStatus {
	if canonical, ok := legacyPlanItemStatuses[s]; ok {
		return canonical
	}
	return s
}

// PlanItem represents a single step in the dynamic plan.
type PlanItem struct {
	Index    int            `json:"index"`
	Goal     string         `json:"goal"`
	Status   PlanItemStatus `json:"status"`
	Evidence []string       `json:"evidence,omitempty"`
}

// Validate checks the plan item.
func (p PlanItem) Validate() error {
	switch p.Status {
	case PlanItemPending, PlanItemInProgress, PlanItemCompleted:
	default:
		return fmt.Errorf("invalid plan item status %q (valid: %s, %s, %s)",
			p.Status, PlanItemPending, PlanItemInProgress, PlanItemCompleted)
	}
	if p.Goal == "" {
		return fmt.Errorf("plan item goal required")
	}
	return nil
}

// Plan is the dynamic task plan for a Run.
type Plan struct {
	// Title is a short model-authored name for the overall objective, shown
	// as the plan panel's title row. Optional; snapshots that omit it keep
	// the previously set title (see drainPlanUpdates).
	Title string     `json:"title,omitempty"`
	Items []PlanItem `json:"items"`
}

// Validate checks the plan invariants:
//   - at most one item is in_progress
//   - done items should have evidence
func (p Plan) Validate() error {
	inProgress := 0
	for _, item := range p.Items {
		if err := item.Validate(); err != nil {
			return err
		}
		if item.Status == PlanItemInProgress {
			inProgress++
		}
	}
	if inProgress > 1 {
		return fmt.Errorf("at most one plan item can be in_progress, got %d", inProgress)
	}
	return nil
}

// CurrentInProgress returns the in-progress item, if any.
func (p Plan) CurrentInProgress() *PlanItem {
	for i := range p.Items {
		if p.Items[i].Status == PlanItemInProgress {
			return &p.Items[i]
		}
	}
	return nil
}

// NextPending returns the next pending item, if any.
func (p Plan) NextPending() *PlanItem {
	for i := range p.Items {
		if p.Items[i].Status == PlanItemPending {
			return &p.Items[i]
		}
	}
	return nil
}

// IsComplete reports whether all items are completed.
func (p Plan) IsComplete() bool {
	for _, item := range p.Items {
		if item.Status != PlanItemCompleted {
			return false
		}
	}
	return len(p.Items) > 0
}

// NormalizeStatuses rewrites legacy and variant item statuses to their
// canonical forms in place. Call it on plans restored from checkpoints
// written before the pending/completed rename so the re-injected plan
// note never shows the model a stale vocabulary.
func (p *Plan) NormalizeStatuses() {
	for i := range p.Items {
		p.Items[i].Status = NormalizePlanItemStatus(p.Items[i].Status)
	}
}
