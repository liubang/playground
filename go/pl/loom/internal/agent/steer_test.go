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
