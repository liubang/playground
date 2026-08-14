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
	"errors"
	"slices"
	"testing"
)

func TestSteerCellFIFO(t *testing.T) {
	cell := NewSteerCell()
	for _, text := range []string{"one", "two", "three"} {
		if err := cell.Put(text); err != nil {
			t.Fatalf("Put(%q): %v", text, err)
		}
	}
	if got := cell.Len(); got != 3 {
		t.Fatalf("Len = %d, want 3", got)
	}
	if got := cell.Take(); !slices.Equal(got, []string{"one", "two", "three"}) {
		t.Fatalf("Take = %v, want FIFO order", got)
	}
	if got := cell.Take(); got != nil {
		t.Fatalf("Take after drain = %v, want nil", got)
	}
}

func TestSteerCellPeekIsNonDestructive(t *testing.T) {
	cell := NewSteerCell()
	_ = cell.Put("note")
	peek := cell.Peek()
	if !slices.Equal(peek, []string{"note"}) {
		t.Fatalf("Peek = %v", peek)
	}
	// Mutating the copy must not affect the cell.
	peek[0] = "tampered"
	if got := cell.Peek(); got[0] != "note" {
		t.Fatalf("Peek shares backing array with the queue: %v", got)
	}
	if got := cell.Len(); got != 1 {
		t.Fatalf("Len after Peek = %d, want 1", got)
	}
}

func TestSteerCellRejectsBlankAndFull(t *testing.T) {
	cell := NewSteerCell()
	if err := cell.Put("   "); err == nil {
		t.Fatal("blank text should be rejected")
	}
	for i := range SteerCellCapacity {
		if err := cell.Put(string(rune('a' + i))); err != nil {
			t.Fatalf("Put #%d: %v", i, err)
		}
	}
	if err := cell.Put("overflow"); !errors.Is(err, ErrSteerCellFull) {
		t.Fatalf("full Put error = %v, want ErrSteerCellFull", err)
	}
}

// TestSteerCellDualQueue pins the delivery-target split: Take drains only
// the steer (next-step) queue; followups wait for the turn boundary and
// leave one at a time.
func TestSteerCellDualQueue(t *testing.T) {
	cell := NewSteerCell()
	_ = cell.Put("steer-1")
	_ = cell.PutFollowup("followup-1")
	_ = cell.PutFollowup("followup-2")
	_ = cell.Put("steer-2")

	// Draining steers never touches followups.
	if got := cell.Take(); !slices.Equal(got, []string{"steer-1", "steer-2"}) {
		t.Fatalf("Take = %v, want the steer queue in FIFO order", got)
	}
	if got := cell.FollowupLen(); got != 2 {
		t.Fatalf("FollowupLen = %d, want 2 (untouched by the steer drain)", got)
	}

	// Followups leave one per claim, in order.
	if got, ok := cell.TakeFollowup(); !ok || got != "followup-1" {
		t.Fatalf("TakeFollowup = %q, %v; want followup-1, true", got, ok)
	}
	if got := cell.PeekFollowups(); !slices.Equal(got, []string{"followup-2"}) {
		t.Fatalf("PeekFollowups = %v, want [followup-2]", got)
	}
	if got, ok := cell.TakeFollowup(); !ok || got != "followup-2" {
		t.Fatalf("TakeFollowup = %q, %v; want followup-2, true", got, ok)
	}
	if got, ok := cell.TakeFollowup(); ok || got != "" {
		t.Fatalf("TakeFollowup on empty = %q, %v; want \"\", false", got, ok)
	}
}

// TestSteerCellSharesCapacityAcrossQueues: the soft cap guards total
// pending volume regardless of the queue mix.
func TestSteerCellSharesCapacityAcrossQueues(t *testing.T) {
	cell := NewSteerCell()
	for i := range SteerCellCapacity - 1 {
		if err := cell.Put(string(rune('a' + i))); err != nil {
			t.Fatalf("Put #%d: %v", i, err)
		}
	}
	if err := cell.PutFollowup("last slot"); err != nil {
		t.Fatalf("PutFollowup into the last slot: %v", err)
	}
	if err := cell.PutFollowup("overflow"); !errors.Is(err, ErrSteerCellFull) {
		t.Fatalf("full PutFollowup error = %v, want ErrSteerCellFull", err)
	}
}
