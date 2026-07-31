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
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/charmbracelet/lipgloss"
)

func finderTestItems(names ...string) []FinderItem[string] {
	items := make([]FinderItem[string], 0, len(names))
	for _, n := range names {
		items = append(items, FinderItem[string]{Value: n, Text: n})
	}
	return items
}

func newTestFinder(items []FinderItem[string]) *Finder[string] {
	return NewFinder(FinderConfig[string]{Title: "Test", Items: items})
}

// --- fuzzy scoring ---

func TestFuzzyScoreSubsequenceRequired(t *testing.T) {
	if _, ok := fuzzyScore("mdl", "model"); !ok {
		t.Fatal("mdl should match model as a subsequence")
	}
	if _, ok := fuzzyScore("xyz", "model"); ok {
		t.Fatal("xyz must not match model")
	}
	if _, ok := fuzzyScore("GPT", "openai/gpt-5"); !ok {
		t.Fatal("matching is case-insensitive")
	}
}

func TestFuzzyScoreBonuses(t *testing.T) {
	consecutive, _ := fuzzyScore("mod", "model")
	scattered, _ := fuzzyScore("mdl", "model")
	if consecutive <= scattered {
		t.Fatalf("consecutive run (%d) should outscore scattered hits (%d)", consecutive, scattered)
	}
	boundary, _ := fuzzyScore("gv", "git-vcs")
	plain, _ := fuzzyScore("gv", "grove")
	if boundary <= plain {
		t.Fatalf("word-boundary hit (%d) should outscore a plain hit (%d)", boundary, plain)
	}
	early, _ := fuzzyScore("ab", "abzzz")
	late, _ := fuzzyScore("ab", "zzabz")
	if early <= late {
		t.Fatalf("early first hit (%d) should outscore a late one (%d)", early, late)
	}
}

// --- filtering ---

func TestFinderFilterRanksBestMatchFirst(t *testing.T) {
	f := newTestFinder(finderTestItems("openai/gpt-5", "aigc/glm-5.2", "aigc/deepseek-v4-pro"))
	for _, r := range "glm" {
		f.TypeRune(r)
	}
	if f.Len() != 1 {
		t.Fatalf("len = %d, want 1", f.Len())
	}
	if got := *f.Selected(); got != "aigc/glm-5.2" {
		t.Fatalf("selected = %q, want aigc/glm-5.2", got)
	}
}

func TestFinderEmptyQueryKeepsSourceOrder(t *testing.T) {
	f := newTestFinder(finderTestItems("c", "a", "b"))
	f.TypeRune('x') // no match anywhere
	f.Backspace()
	order := []string{*f.Selected()}
	f.MoveDown()
	order = append(order, *f.Selected())
	f.MoveDown()
	order = append(order, *f.Selected())
	if strings.Join(order, ",") != "c,a,b" {
		t.Fatalf("order = %v, want source order c,a,b", order)
	}
}

func TestFinderCursorClampsWhenFilterShrinks(t *testing.T) {
	f := newTestFinder(finderTestItems("alpha", "alps", "beta", "gamma"))
	f.GotoBottom()
	if got := *f.Selected(); got != "gamma" {
		t.Fatalf("selected = %q, want gamma", got)
	}
	for _, r := range "alp" {
		f.TypeRune(r)
	}
	if f.Len() != 2 {
		t.Fatalf("len = %d, want 2", f.Len())
	}
	// The cursor was on row 3 of 4; the filter left 2 rows, so it must
	// clamp back into range instead of pointing past the end.
	if got := *f.Selected(); got != "alps" {
		t.Fatalf("selected = %q, want alps (clamped)", got)
	}
}

func TestFinderNoMatches(t *testing.T) {
	f := newTestFinder(finderTestItems("alpha", "beta"))
	for _, r := range "zzz" {
		f.TypeRune(r)
	}
	if f.Selected() != nil {
		t.Fatal("Selected() must be nil with no matches")
	}
	out := f.Render(60, 0)
	if !strings.Contains(out, `No matches for "zzz"`) {
		t.Fatalf("render should explain the empty result:\n%s", out)
	}
}

// --- modes ---

func TestFinderModes(t *testing.T) {
	f := newTestFinder(finderTestItems("alpha", "beta"))
	if f.Normal() {
		t.Fatal("a fresh finder starts in insert mode")
	}
	f.EnterNormal()
	f.TypeRune('x') // no-op in normal mode
	f.Backspace()   // no-op too
	if f.Query() != "" {
		t.Fatalf("query = %q, want empty (normal mode ignores text input)", f.Query())
	}
	f.EnterInsert()
	f.TypeRune('a')
	if f.Query() != "a" {
		t.Fatalf("query = %q, want a", f.Query())
	}
}

func TestFinderGotoTopBottom(t *testing.T) {
	f := newTestFinder(finderTestItems("a", "b", "c"))
	f.GotoBottom()
	if got := *f.Selected(); got != "c" {
		t.Fatalf("G = %q, want c", got)
	}
	f.GotoTop()
	if got := *f.Selected(); got != "a" {
		t.Fatalf("gg = %q, want a", got)
	}
}

// --- async loading ---

func TestFinderLoadingStates(t *testing.T) {
	f := NewLoadingFinder[string]("Test", nil, FinderStyles{})
	if out := f.Render(60, 0); !strings.Contains(out, "Loading") {
		t.Fatalf("loading state:\n%s", out)
	}
	f.Load(nil, errors.New("boom"))
	if out := f.Render(60, 0); !strings.Contains(out, "boom") {
		t.Fatalf("error state:\n%s", out)
	}
	f.Load(finderTestItems("a"), nil)
	if f.Len() != 1 {
		t.Fatalf("len = %d, want 1 after Load", f.Len())
	}
}

// --- rendering ---

func TestFinderRenderLayout(t *testing.T) {
	f := NewFinder(FinderConfig[string]{
		Title: "Test",
		Items: []FinderItem[string]{
			{Value: "one", Text: "one", Hint: "first", Badge: "●"},
			{Value: "two", Text: "two", Hint: "second"},
		},
		Preview: func(v string) string { return "preview of " + v },
	})
	out := f.Render(80, 0)
	for _, want := range []string{
		"❯", "2/2", "▶ one", "first", "●", "preview of one", "INSERT",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("render missing %q:\n%s", want, out)
		}
	}
	// The counter tracks the filter: 1/2 while one row matches.
	f.TypeRune('o')
	f.TypeRune('n')
	f.TypeRune('e')
	out = f.Render(80, 0)
	if !strings.Contains(out, "1/2") {
		t.Errorf("filtered render missing counter:\n%s", out)
	}
	// Normal mode swaps the footer hints.
	f.EnterNormal()
	out = f.Render(80, 0)
	if !strings.Contains(out, "NORMAL") {
		t.Errorf("normal-mode render missing mode hint:\n%s", out)
	}
}

func TestFinderRenderDropsPreviewOnNarrowWidth(t *testing.T) {
	f := NewFinder(FinderConfig[string]{
		Title:   "Test",
		Items:   finderTestItems("one"),
		Preview: func(v string) string { return "preview" },
	})
	out := f.Render(finderPreviewMinWidth-1, 0)
	if strings.Contains(out, "preview") {
		t.Errorf("narrow render must drop the preview pane:\n%s", out)
	}
}

func TestFinderRenderRespectsHeightBudget(t *testing.T) {
	var items []FinderItem[string]
	for i := 0; i < 30; i++ {
		items = append(items, FinderItem[string]{Value: fmt.Sprintf("item-%02d", i), Text: fmt.Sprintf("item-%02d", i)})
	}
	f := newTestFinder(items)
	for i := 0; i < 20; i++ {
		f.MoveDown()
	}
	out := f.Render(40, 12)
	if lines := strings.Count(out, "\n") + 1; lines > 12 {
		t.Fatalf("render used %d lines, budget 12:\n%s", lines, out)
	}
	if !strings.Contains(out, "▶ item-20") {
		t.Fatalf("cursor row must stay visible:\n%s", out)
	}
}

func TestTruncateANSI(t *testing.T) {
	styled := lipgloss.NewStyle().Bold(true).Render("hello world")
	out := truncateANSI(styled, 5)
	if got := lipgloss.Width(out); got > 5 {
		t.Fatalf("width = %d, want <= 5", got)
	}
	if !strings.HasSuffix(out, "\x1b[0m") {
		t.Fatalf("truncated output must end with a reset: %q", out)
	}
	// Short strings pass through untouched.
	if got := truncateANSI(styled, 80); got != styled {
		t.Fatalf("short string modified: %q", got)
	}
}
