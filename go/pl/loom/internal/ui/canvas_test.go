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

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

func TestDropANSI(t *testing.T) {
	if got := dropANSI("hello world", 6); got != "\x1b[0mworld" {
		t.Fatalf("dropANSI plain = %q, want reset+world", got)
	}
	// Styled text: the escape sequences inside the dropped prefix are
	// replayed so mid-line styling survives the cut.
	styled := lipgloss.NewStyle().Bold(true).Render("hello") + " plain"
	got := dropANSI(styled, 3)
	if !strings.Contains(got, "lo plain") {
		t.Fatalf("dropANSI styled = %q, want remainder %q", got, "lo plain")
	}
	if !strings.HasPrefix(got, "\x1b[0m") {
		t.Fatalf("dropANSI must open with a reset: %q", got)
	}
	// Dropping past the end leaves only the reset.
	if got := dropANSI("short", 99); got != "\x1b[0m" {
		t.Fatalf("dropANSI overflow = %q, want reset only", got)
	}
	// Width zero is a no-op.
	if got := dropANSI(styled, 0); got != styled {
		t.Fatalf("dropANSI(…, 0) modified the input: %q", got)
	}
}

func TestComposeFloatsCoversBackground(t *testing.T) {
	frame := strings.Join([]string{
		"aaaaaaaaaa",
		"bbbbbbbbbb",
		"cccccccccc",
		"dddddddddd",
	}, "\n")
	float := Float{Content: "XY\nZW", X: 2, Y: 1}
	out := ComposeFloats(frame, 10, 4, float)
	lines := strings.Split(out, "\n")
	if len(lines) != 4 {
		t.Fatalf("line count = %d, want 4", len(lines))
	}
	if lines[0] != "aaaaaaaaaa" {
		t.Fatalf("row 0 untouched: %q", lines[0])
	}
	if got := lipgloss.Width(lines[1]); got != 10 {
		t.Fatalf("row 1 width = %d, want 10", got)
	}
	if !strings.Contains(lines[1], "bb") || !strings.Contains(lines[1], "XY") {
		t.Fatalf("row 1 = %q, want left bb + float XY", lines[1])
	}
	// The covered remainder resumes after the float's width.
	if !strings.HasSuffix(lines[1], "bbbb") {
		t.Fatalf("row 1 = %q, want the right remainder bbbb", lines[1])
	}
	if !strings.Contains(lines[2], "ZW") {
		t.Fatalf("row 2 = %q, want float line ZW", lines[2])
	}
	if lines[3] != "dddddddddd" {
		t.Fatalf("row 3 untouched: %q", lines[3])
	}
}

func TestComposeFloatsPadsShortBackground(t *testing.T) {
	// A background line shorter than the float's x offset must be padded,
	// not drag the float leftwards.
	out := ComposeFloats("ab\ncd", 10, 2, Float{Content: "Q", X: 6, Y: 0})
	lines := strings.Split(out, "\n")
	if !strings.HasPrefix(lipgloss.NewStyle().Render(lines[0]), "ab") {
		t.Fatalf("row 0 = %q", lines[0])
	}
	if idx := strings.Index(lines[0], "Q"); idx != 6 {
		t.Fatalf("float column = %d, want 6 (padding missing): %q", idx, lines[0])
	}
}

func TestComposeFloatsNormalizesHeight(t *testing.T) {
	// Short frame: padded with blanks. Long frame: truncated at the bottom.
	out := ComposeFloats("one\ntwo", 10, 4)
	if lines := strings.Count(out, "\n") + 1; lines != 4 {
		t.Fatalf("padded frame lines = %d, want 4", lines)
	}
	out = ComposeFloats("1\n2\n3\n4\n5", 10, 3)
	if lines := strings.Count(out, "\n") + 1; lines != 3 {
		t.Fatalf("truncated frame lines = %d, want 3", lines)
	}
}

func TestComposeFloatsClampsOverflow(t *testing.T) {
	// A float larger than the screen must not panic, wrap, or grow the
	// frame: rows outside the screen are dropped, wide lines are cut.
	big := strings.Repeat(strings.Repeat("x", 30)+"\n", 12)
	out := ComposeFloats("background", 10, 4, Float{Content: big, X: 5, Y: 2})
	lines := strings.Split(out, "\n")
	if len(lines) != 4 {
		t.Fatalf("lines = %d, want 4", len(lines))
	}
	for i, line := range lines {
		if w := lipgloss.Width(line); w > 10 {
			t.Fatalf("line %d width = %d, exceeds 10: %q", i, w, line)
		}
	}
}

func TestCenteredFloat(t *testing.T) {
	f := centeredFloat(strings.Repeat("x", 20)+"\n"+strings.Repeat("x", 20), 100, 40)
	if f.X != 40 || f.Y != 19 {
		t.Fatalf("position = (%d, %d), want (40, 19)", f.X, f.Y)
	}
	// Oversized content clamps to the origin instead of going negative.
	f = centeredFloat(strings.Repeat("x", 200)+"\n"+strings.Repeat("x", 200), 100, 1)
	if f.X != 0 || f.Y != 0 {
		t.Fatalf("oversized position = (%d, %d), want (0, 0)", f.X, f.Y)
	}
}

// --- floats in the full frame ---

// newFloatTestModel builds a sized model with deterministic (color-free)
// styling and one transcript block.
func newFloatTestModel(t *testing.T, altScreen bool) Model {
	t.Helper()
	m := NewModel(newTestController(t), "test/model-a", "/ws")
	m.SetTheme(NoColorTheme())
	m.altScreen = altScreen
	updated, _ := m.Update(tea.WindowSizeMsg{Width: 100, Height: 40})
	m = updated.(Model)
	// A finalized block: pending blocks are volatile by design and force
	// a rebuild on every sync, which is not what these tests measure.
	m.blocks.Add(&TranscriptBlock{ID: "b1", Kind: BlockKindUser, Content: "hello transcript", Done: true})
	m.syncTranscript()
	return m
}

func TestHelpFloatsOverChatFrame(t *testing.T) {
	m := newFloatTestModel(t, true)
	m.mode = ModeHelp
	out := m.View()

	if lines := strings.Count(out, "\n") + 1; lines != 40 {
		t.Fatalf("frame lines = %d, want exactly the screen height 40", lines)
	}
	// The float and the base frame beneath it are both visible.
	for _, want := range []string{"Loom TUI Help", "hello transcript", "Type your message"} {
		if !strings.Contains(out, want) {
			t.Errorf("floating frame missing %q", want)
		}
	}
	// Every line fits the screen width (no wrap-induced corruption).
	for i, line := range strings.Split(out, "\n") {
		if w := lipgloss.Width(line); w > 100 {
			t.Fatalf("line %d width = %d, exceeds 100", i, w)
		}
	}
}

func TestHelpStaysInFlowInline(t *testing.T) {
	m := newFloatTestModel(t, false)
	m.mode = ModeHelp
	out := m.View()
	if !strings.Contains(out, "Loom TUI Help") {
		t.Fatal("inline help should render in-flow")
	}
	if strings.Contains(out, "Type your message") {
		t.Fatal("inline help replaces the composer area; the composer must not render")
	}
}

// TestFloatsDoNotRebuildTranscript locks the cache discipline of the
// compositor (docs/VIM_UI_DESIGN.md §3): opening, rendering and closing
// floats must never invalidate the transcript's render memo.
func TestFloatsDoNotRebuildTranscript(t *testing.T) {
	m := newFloatTestModel(t, true)
	buildsBefore := m.transcriptBuilds
	for i := 0; i < 10; i++ {
		// Each pass simulates a full open/render/close cycle: layout is
		// what Update drives on every state transition.
		m.mode = ModeHelp
		m.layout()
		_ = m.View()
		m.mode = ModeChat
		m.layout()
		_ = m.View()
	}
	if m.transcriptBuilds != buildsBefore {
		t.Fatalf("transcriptBuilds = %d, want %d (floats must not trigger rebuilds)",
			m.transcriptBuilds, buildsBefore)
	}
}

func TestPickerFloatsOverChatFrame(t *testing.T) {
	m := newFloatTestModel(t, true)
	m.SetModels([]ModelOption{
		{Provider: "test", Name: "model-a", ContextWindow: 128000},
		{Provider: "test", Name: "model-b", ContextWindow: 64000},
	})
	updated, _ := m.handleSlashCommand("/model")
	m = updated.(Model)
	out := m.View()

	// The snacks-style finder floats over the live chat frame: filter
	// input, rows and footer from the picker, transcript and composer
	// from the base frame.
	for _, want := range []string{"❯", "test/model-a", "INSERT", "hello transcript", "Type your message"} {
		if !strings.Contains(out, want) {
			t.Errorf("floating picker frame missing %q", want)
		}
	}
	if lines := strings.Count(out, "\n") + 1; lines != 40 {
		t.Fatalf("frame lines = %d, want 40", lines)
	}
	for i, line := range strings.Split(out, "\n") {
		if w := lipgloss.Width(line); w > 100 {
			t.Fatalf("line %d width = %d, exceeds 100", i, w)
		}
	}
}

func TestPickerStaysFullAreaInline(t *testing.T) {
	m := newFloatTestModel(t, false)
	m.SetModels([]ModelOption{{Provider: "test", Name: "model-a"}})
	updated, _ := m.handleSlashCommand("/model")
	m = updated.(Model)
	out := m.View()
	if !strings.Contains(out, "test/model-a") {
		t.Fatal("inline picker should render in the main area")
	}
	if strings.Contains(out, "Type your message") {
		t.Fatal("inline picker owns the main area; the composer must not render")
	}
}

func TestQuestionFloatsOverChatFrame(t *testing.T) {
	m := newFloatTestModel(t, true)
	m.choiceList = NewChoiceList(ChoiceListConfig{
		Title: "Model asks:\nproceed?",
		Items: []ChoiceItem{{Label: "yes"}, {Label: "no"}},
	})
	m.mode = ModeQuestion
	out := m.View()
	for _, want := range []string{"proceed?", "hello transcript", "Type your message"} {
		if !strings.Contains(out, want) {
			t.Errorf("floating question frame missing %q", want)
		}
	}
	if lines := strings.Count(out, "\n") + 1; lines != 40 {
		t.Fatalf("frame lines = %d, want 40", lines)
	}
}
