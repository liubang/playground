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
// Created: 2026/07/31

package ui

import (
	"strings"
	"testing"
)

func TestLineViewScrollSemantics(t *testing.T) {
	v := lineView{Width: 80, Height: 4}
	v.SetContent("1\n2\n3\n4\n5\n6\n7\n8\n9\n10")

	if v.TotalLineCount() != 10 {
		t.Fatalf("lines = %d, want 10", v.TotalLineCount())
	}
	if v.AtBottom() {
		t.Fatal("a fresh view starts at the top")
	}
	v.GotoBottom()
	if v.YOffset != 6 || !v.AtBottom() {
		t.Fatalf("GotoBottom: YOffset = %d, want 6 (at bottom)", v.YOffset)
	}
	v.LineUp(99)
	if v.YOffset != 0 {
		t.Fatalf("LineUp clamps at 0, got %d", v.YOffset)
	}
	v.LineDown(99)
	if v.YOffset != 6 {
		t.Fatalf("LineDown clamps at maxOffset 6, got %d", v.YOffset)
	}
	v.SetYOffset(-5)
	if v.YOffset != 0 {
		t.Fatalf("SetYOffset clamps negative to 0, got %d", v.YOffset)
	}

	// SetContent preserves a legal offset but snaps to the bottom when
	// the offset points past the shrunk content (bubbles semantics).
	v.SetYOffset(3)
	v.SetContent("1\n2\n3\n4\n5")
	if v.YOffset != 3 {
		t.Fatalf("SetContent should preserve legal offset, got %d", v.YOffset)
	}
	v.SetContent("1\n2")
	if !v.AtBottom() {
		t.Fatalf("SetContent past the end must snap to bottom, YOffset = %d", v.YOffset)
	}
}

func TestLineViewViewPadsAndTruncates(t *testing.T) {
	v := lineView{Width: 5, Height: 4}
	v.SetContent("short\nthis line is far too long")
	out := strings.Split(v.View(), "\n")
	if len(out) != 4 {
		t.Fatalf("view lines = %d, want exactly Height 4 (padded)", len(out))
	}
	if out[0] != "short" {
		t.Fatalf("line 0 = %q", out[0])
	}
	if out[1] != "this \x1b[0m" {
		t.Fatalf("line 1 = %q, want truncated to width 5 (reset-terminated)", out[1])
	}
	if out[2] != "" || out[3] != "" {
		t.Fatalf("short content must pad with blanks: %q / %q", out[2], out[3])
	}

	// Scrolling down drops the pad and shows the tail.
	v.SetContent("1\n2\n3\n4\n5\n6")
	v.GotoBottom()
	out = strings.Split(v.View(), "\n")
	if strings.Join(out, "|") != "3|4|5|6" {
		t.Fatalf("bottom view = %q, want 3|4|5|6", strings.Join(out, "|"))
	}
}

func TestLineViewEmptyAndDegenerate(t *testing.T) {
	var v lineView
	if got := v.View(); got != "" {
		t.Fatalf("zero-value view = %q, want empty", got)
	}
	v = lineView{Width: 80, Height: 0}
	v.SetContent("a\nb")
	if got := v.View(); got != "" {
		t.Fatalf("zero-height view = %q, want empty", got)
	}
	// Content shorter than the window: everything is visible, always at
	// the bottom (nothing to scroll).
	v = lineView{Width: 80, Height: 10}
	v.SetContent("only")
	if !v.AtBottom() {
		t.Fatal("content fitting the window is always at the bottom")
	}
	v.LineDown(5)
	if v.YOffset != 0 {
		t.Fatalf("nothing to scroll: YOffset = %d, want 0", v.YOffset)
	}
}
