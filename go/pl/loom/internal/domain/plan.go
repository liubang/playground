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

const (
	PlanItemTodo       PlanItemStatus = "todo"
	PlanItemInProgress PlanItemStatus = "in_progress"
	PlanItemDone       PlanItemStatus = "done"
)

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
	case PlanItemTodo, PlanItemInProgress, PlanItemDone:
	default:
		return fmt.Errorf("invalid plan item status %q", p.Status)
	}
	if p.Goal == "" {
		return fmt.Errorf("plan item goal required")
	}
	return nil
}

// Plan is the dynamic task plan for a Run.
type Plan struct {
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

// NextTodo returns the next todo item, if any.
func (p Plan) NextTodo() *PlanItem {
	for i := range p.Items {
		if p.Items[i].Status == PlanItemTodo {
			return &p.Items[i]
		}
	}
	return nil
}

// IsComplete reports whether all items are done.
func (p Plan) IsComplete() bool {
	for _, item := range p.Items {
		if item.Status != PlanItemDone {
			return false
		}
	}
	return len(p.Items) > 0
}
