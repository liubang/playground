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
				{Index: 0, Goal: "step 1", Status: PlanItemCompleted, Evidence: []string{"test passed"}},
				{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
				{Index: 2, Goal: "step 3", Status: PlanItemPending},
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
				{Index: 0, Status: PlanItemPending},
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
		{Index: 0, Goal: "step 1", Status: PlanItemCompleted},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
		{Index: 2, Goal: "step 3", Status: PlanItemPending},
	}}

	cur := plan.CurrentInProgress()
	if cur == nil || cur.Goal != "step 2" {
		t.Fatal("expected step 2 in progress")
	}
}

func TestPlanNextPending(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemCompleted},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
		{Index: 2, Goal: "step 3", Status: PlanItemPending},
	}}

	next := plan.NextPending()
	if next == nil || next.Goal != "step 3" {
		t.Fatal("expected step 3 as next pending")
	}
}

func TestPlanIsComplete(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemCompleted},
		{Index: 1, Goal: "step 2", Status: PlanItemCompleted},
	}}

	if !plan.IsComplete() {
		t.Error("expected plan to be complete")
	}
}

func TestPlanIsNotComplete(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: PlanItemCompleted},
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
	item := PlanItem{Index: 0, Goal: "step 1", Status: PlanItemPending}
	if item.Status != PlanItemPending {
		t.Fatal("expected pending")
	}
	item.Status = PlanItemInProgress
	if item.Status != PlanItemInProgress {
		t.Fatal("expected in_progress")
	}
	item.Status = PlanItemCompleted
	item.Evidence = append(item.Evidence, "test passed")
	if item.Status != PlanItemCompleted {
		t.Fatal("expected completed")
	}
}

func TestNormalizePlanItemStatus(t *testing.T) {
	cases := map[PlanItemStatus]PlanItemStatus{
		// Canonical values pass through.
		PlanItemPending:    PlanItemPending,
		PlanItemInProgress: PlanItemInProgress,
		PlanItemCompleted:  PlanItemCompleted,
		// Pre-rename loom names.
		"todo": PlanItemPending,
		"done": PlanItemCompleted,
		// Common cross-harness variants.
		"in-progress": PlanItemInProgress,
		"inprogress":  PlanItemInProgress,
		"complete":    PlanItemCompleted,
		"finished":    PlanItemCompleted,
		// Unknown values pass through so Validate can reject them.
		"doing": "doing",
		"":      "",
	}
	for in, want := range cases {
		if got := NormalizePlanItemStatus(in); got != want {
			t.Errorf("NormalizePlanItemStatus(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestPlanNormalizeStatuses(t *testing.T) {
	plan := Plan{Items: []PlanItem{
		{Index: 0, Goal: "step 1", Status: "done"},
		{Index: 1, Goal: "step 2", Status: PlanItemInProgress},
		{Index: 2, Goal: "step 3", Status: "todo"},
	}}
	plan.NormalizeStatuses()
	for i, want := range []PlanItemStatus{PlanItemCompleted, PlanItemInProgress, PlanItemPending} {
		if plan.Items[i].Status != want {
			t.Errorf("items[%d].Status = %q, want %q", i, plan.Items[i].Status, want)
		}
	}
	if err := plan.Validate(); err != nil {
		t.Fatalf("normalized plan must validate: %v", err)
	}
}
