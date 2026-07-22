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

import "testing"

func TestPlanValidation(t *testing.T) {
	tests := []struct {
		name    string
		plan    Plan
		wantErr bool
	}{
		{
			"valid plan",
			Plan{Items: []PlanItem{
				{Index: 0, Goal: "step 1", Status: PlanItemDone, Evidence: []string{"test passed"}},
				{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
				{Index: 2, Goal: "step 3", Status: PlanItemTodo},
			}},
			false,
		},
		{
			"two in_progress",
			Plan{Items: []PlanItem{
				{Index: 0, Goal: "step 1", Status: PlanItemInProgress},
				{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
			}},
			true,
		},
		{
			"invalid status",
			Plan{Items: []PlanItem{
				{Index: 0, Goal: "step 1", Status: "unknown"},
			}},
			true,
		},
		{
			"empty goal",
			Plan{Items: []PlanItem{
				{Index: 0, Status: PlanItemTodo},
			}},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.plan.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestPlanCurrentInProgress(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemDone},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
		{Index: 2, Goal: "step 3", Status: PlanItemTodo},
	}}

	cur := plan.CurrentInProgress()
	if cur == nil || cur.Goal != "step 2" {
		t.Fatal("expected step 2 in progress")
	}
}

func TestPlanNextTodo(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemDone},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
		{Index: 2, Goal: "step 3", Status: PlanItemTodo},
	}}

	next := plan.NextTodo()
	if next == nil || next.Goal != "step 3" {
		t.Fatal("expected step 3 as next todo")
	}
}

func TestPlanIsComplete(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemDone},
		{Index: 1, Goal: "step 2", Status: PlanItemDone},
	}}

	if !plan.IsComplete() {
		t.Error("expected plan to be complete")
	}
}

func TestPlanIsNotComplete(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemDone},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
	}}

	if plan.IsComplete() {
		t.Error("expected plan to not be complete")
	}
}

func TestEmptyPlanNotComplete(t *testing.T) {
	plan := Plan{}
	if plan.IsComplete() {
		t.Error("empty plan should not be complete")
	}
}

func TestPlanItemStatusTransition(t *testing.T) {
	item := PlanItem{Index: 0, Goal: "step 1", Status: PlanItemTodo}
	if item.Status != PlanItemTodo {
		t.Fatal("expected todo")
	}
	item.Status = PlanItemInProgress
	if item.Status != PlanItemInProgress {
		t.Fatal("expected in_progress")
	}
	item.Status = PlanItemDone
	item.Evidence = append(item.Evidence, "test passed")
	if item.Status != PlanItemDone {
		t.Fatal("expected done")
	}
}
