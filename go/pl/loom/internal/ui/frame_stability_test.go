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

package ui

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func frameLines(s string) int {
	return strings.Count(s, "\n") + 1
}

// Opening and closing the completion popup must leave the frame geometry and
// the composer border intact.
func TestCompletionOpenCloseKeepsFrameStable(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 24
	m.layout()

	before := m.View()

	updated, _ := m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'/'}})
	m = updated.(Model)
	if !m.completionVisible() {
		t.Fatal("completion popup did not open on \"/\"")
	}

	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updated.(Model)
	if m.completionVisible() {
		t.Fatal("completion popup did not close on Esc")
	}

	after := m.View()
	if frameLines(after) != frameLines(before) {
		t.Fatalf("frame height changed: before=%d after=%d", frameLines(before), frameLines(after))
	}
	for _, want := range []string{"╭", "╰"} {
		if !strings.Contains(after, want) {
			t.Fatalf("composer border %q missing after closing popup:\n%s", want, after)
		}
	}
}

// Before the first WindowSizeMsg the terminal size is unknown; the model must
// render a single harmless line instead of a degenerate full frame. A taller
// first frame can scroll the terminal and desynchronize the renderer.
func TestUnknownSizeRendersSingleLine(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws") // width/height are zero here
	view := m.View()
	if frameLines(view) != 1 {
		t.Fatalf("pre-size frame must be one line, got %d:\n%q", frameLines(view), view)
	}
}

// An overlong header title must be truncated to one line: lipgloss would
// otherwise wrap it, making the frame taller than the terminal.
func TestHeaderTruncatesOverlongTitle(t *testing.T) {
	m := Model{
		theme:     NoColorTheme(),
		width:     70,
		modelName: "glm-5.2",
		sessionID: domain.NewSessionID(),
		workspace: "/very/long/workspace/path/that/will/not/fit/either",
	}
	header := m.renderHeader()
	if frameLines(header) != 1 {
		t.Fatalf("header wrapped to %d lines:\n%q", frameLines(header), header)
	}
	if !strings.Contains(header, "...") {
		t.Fatalf("overlong header not truncated: %q", header)
	}
}
