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
// Created: 2026/07/27

package ui

import (
	"strings"
	"testing"
)

func testChoiceItems() []ChoiceItem {
	return []ChoiceItem{
		{Label: "alpha", Desc: "the first option"},
		{Label: "beta", Desc: "the second option"},
		{Label: "gamma"},
	}
}

func TestChoiceListSingleSelectRadio(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Title: "pick one", Items: testChoiceItems()})

	// Confirm answers with the focused row even without an explicit toggle.
	answer, ok := l.Confirm()
	if !ok || len(answer.Selected) != 1 || answer.Selected[0] != "alpha" {
		t.Fatalf("confirm = %+v (ok=%v), want alpha", answer, ok)
	}

	// Toggling another row clears the previous selection (radio).
	l.MoveDown()
	l.MoveDown()
	l.Toggle()
	l.MoveUp()
	l.Toggle()
	answer, ok = l.Confirm()
	if !ok || len(answer.Selected) != 1 || answer.Selected[0] != "beta" {
		t.Fatalf("confirm = %+v (ok=%v), want beta only", answer, ok)
	}
}

func TestChoiceListSingleSelectConfirmPrefersToggled(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Items: testChoiceItems()})

	// Toggle beta, then move the cursor back to alpha: the answer must be
	// beta — the item the radio mark displays — never the cursor row.
	l.MoveDown()
	l.Toggle()
	l.MoveUp()
	answer, ok := l.Confirm()
	if !ok || len(answer.Selected) != 1 || answer.Selected[0] != "beta" {
		t.Fatalf("confirm = %+v (ok=%v), want beta (the toggled item)", answer, ok)
	}
}

func TestChoiceListTitleTruncation(t *testing.T) {
	longTitle := "Model asks:\n" + strings.Repeat("very long question text ", 20)
	l := NewChoiceList(ChoiceListConfig{Title: longTitle, Items: testChoiceItems()})
	out := l.Render(40, 0)
	for _, line := range strings.Split(out, "\n") {
		if len([]rune(line)) > 40 {
			t.Fatalf("line exceeds the width budget (%d runes): %q", len([]rune(line)), line)
		}
	}

	// A title with many lines is capped with an ellipsis.
	manyLines := strings.Join([]string{"l1", "l2", "l3", "l4", "l5", "l6"}, "\n")
	l2 := NewChoiceList(ChoiceListConfig{Title: manyLines, Items: testChoiceItems()})
	out = l2.Render(0, 0)
	if !strings.Contains(out, "…") || strings.Contains(out, "l5") {
		t.Fatalf("title must be capped at %d lines:\n%s", maxChoiceTitleLines, out)
	}
}

func TestChoiceListMultiSelect(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Items: testChoiceItems(), Multi: true})

	// Nothing toggled: confirm refuses.
	if _, ok := l.Confirm(); ok {
		t.Fatal("confirm with no selection must fail")
	}

	l.Toggle() // alpha
	l.MoveDown()
	l.MoveDown()
	l.Toggle() // gamma
	answer, ok := l.Confirm()
	if !ok || len(answer.Selected) != 2 || answer.Selected[0] != "alpha" || answer.Selected[1] != "gamma" {
		t.Fatalf("confirm = %+v (ok=%v), want [alpha gamma]", answer, ok)
	}

	// Untoggling everything refuses again.
	l.Toggle()
	l.MoveUp()
	l.MoveUp()
	l.Toggle()
	if _, ok := l.Confirm(); ok {
		t.Fatal("confirm after untoggling all must fail")
	}
}

func TestChoiceListOtherRow(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Items: testChoiceItems(), OtherRow: true})

	// The free-text row is last; moving past it clamps.
	for i := 0; i < 10; i++ {
		l.MoveDown()
	}
	if !l.onOtherRow() {
		t.Fatal("cursor must land on the other row at the bottom")
	}
	l.MoveDown()
	if !l.onOtherRow() {
		t.Fatal("cursor must clamp on the other row")
	}

	// Toggle is a no-op there; typing edits the text.
	l.Toggle()
	for _, r := range "custom answer" {
		l.TypeRune(r)
	}
	l.Backspace()
	l.Backspace()
	answer, ok := l.Confirm()
	if !ok || answer.CustomText != "custom answ" || len(answer.Selected) != 0 {
		t.Fatalf("confirm = %+v (ok=%v), want custom text", answer, ok)
	}

	// Single-select: empty custom text on the other row refuses.
	l2 := NewChoiceList(ChoiceListConfig{Items: testChoiceItems(), OtherRow: true})
	for i := 0; i < 3; i++ {
		l2.MoveDown()
	}
	if _, ok := l2.Confirm(); ok {
		t.Fatal("empty custom answer must fail")
	}
}

func TestChoiceListMultiSelectWithCustomText(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Items: testChoiceItems(), Multi: true, OtherRow: true})
	l.MoveDown()
	for i := 0; i < 3; i++ {
		l.MoveDown()
	}
	for _, r := range "extra" {
		l.TypeRune(r)
	}
	answer, ok := l.Confirm()
	if !ok || answer.CustomText != "extra" {
		t.Fatalf("confirm = %+v (ok=%v), want custom text only", answer, ok)
	}
}

func TestChoiceListInsertModeLifecycle(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Items: testChoiceItems(), OtherRow: true})
	for i := 0; i < 3; i++ {
		l.MoveDown()
	}
	if !l.onOtherRow() || l.Editing() {
		t.Fatal("cursor should rest on the other row in normal mode")
	}

	// Normal mode: navigation still works — the row is not a trap.
	l.MoveUp()
	if l.onOtherRow() {
		t.Fatal("j/k-style navigation must leave the other row in normal mode")
	}

	// Enter insert mode, type, then leave with the text preserved.
	l.MoveDown()
	l.BeginEdit()
	if !l.Editing() {
		t.Fatal("BeginEdit must enter insert mode")
	}
	for _, r := range "jk row" {
		l.TypeRune(r)
	}
	l.MoveUp() // moving away ends insert mode but keeps the text
	if l.Editing() {
		t.Fatal("leaving the row must end insert mode")
	}
	l.MoveDown()
	l.BeginEdit()
	answer, ok := l.Confirm()
	if !ok || answer.CustomText != "jk row" {
		t.Fatalf("confirm = %+v (ok=%v), want preserved text", answer, ok)
	}

	// The footer switches while editing.
	out := l.Render(0, 0)
	if !strings.Contains(out, "type your answer") {
		t.Fatalf("editing footer missing:\n%s", out)
	}
	l.EndEdit()
	out = l.Render(0, 0)
	if strings.Contains(out, "type your answer") {
		t.Fatalf("normal footer must return after EndEdit:\n%s", out)
	}
}

func TestChoiceListRender(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{Title: "pick", Items: testChoiceItems(), Multi: true, OtherRow: true})
	l.Toggle()
	out := l.Render(0, 0)
	for _, want := range []string{"pick", "[x] alpha", "[ ] beta", "[ ] gamma", "> Other…", "Space = toggle"} {
		if !strings.Contains(out, want) {
			t.Fatalf("render missing %q:\n%s", want, out)
		}
	}

	// Single-select renders radio marks.
	l2 := NewChoiceList(ChoiceListConfig{Items: testChoiceItems()})
	l2.Toggle()
	out = l2.Render(0, 0)
	if !strings.Contains(out, "(•) alpha") || !strings.Contains(out, "( ) beta") {
		t.Fatalf("single-select marks missing:\n%s", out)
	}
}

func TestChoiceListRenderWindowed(t *testing.T) {
	items := make([]ChoiceItem, 20)
	for i := range items {
		items[i] = ChoiceItem{Label: strings.Repeat("x", i+1)}
	}
	l := NewChoiceList(ChoiceListConfig{Title: "many", Items: items})
	for i := 0; i < 15; i++ {
		l.MoveDown()
	}
	out := l.Render(0, 8)
	if !strings.Contains(out, "↑ more") {
		t.Fatalf("windowed render should hint at rows above:\n%s", out)
	}
	if !strings.Contains(out, "↓ more") {
		t.Fatalf("windowed render should hint at rows below:\n%s", out)
	}
}

func TestChoiceListCustomStyle(t *testing.T) {
	l := NewChoiceList(ChoiceListConfig{
		Items: testChoiceItems(),
		Style: ChoiceStyle{
			CursorPrefix:   "> ",
			SelectedMark:   "[+]",
			UnselectedMark: "[-]",
			Footer:         "custom hints",
		},
	})
	l.Toggle()
	out := l.Render(0, 0)
	for _, want := range []string{"> [+] alpha", "[-] beta", "custom hints"} {
		if !strings.Contains(out, want) {
			t.Fatalf("custom style missing %q:\n%s", want, out)
		}
	}
}
