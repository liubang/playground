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
	"github.com/charmbracelet/lipgloss"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
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

// The approval band has a variable line count (metadata, note, paths and
// diff rows come and go). The layout must reserve its actual height so the
// status bar stays glued to the bottom for compact and rich prompts alike.
func TestApprovalBandKeepsStatusBarAtBottom(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 100, 30
	m.mode = ModeApproval
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID:  domain.NewEventID(),
		CallID:      domain.NewToolCallID(),
		ToolName:    "web_fetch",
		Risk:        domain.R3,
		Description: "Fetch https://example.com/x (GET)",
	}
	m.layout()
	if got := frameLines(m.View()); got != m.height {
		t.Fatalf("compact approval frame = %d lines, want %d (status bar must hug the bottom):\n%s", got, m.height, m.View())
	}

	// A richer prompt (metadata + note + paths) must fit exactly as well.
	m.pendingApproval.ToolName = "run_cmd"
	m.pendingApproval.Description = "Run; 'sh' '-c' 'ls'; env[none]; cwd='.'; timeout=120000ms; " +
		"network=loopback-only; shell=R3; note[check the layout]; args_hash=abc"
	m.pendingApproval.ReadPaths = []string{"/ws"}
	m.pendingApproval.WritePaths = []string{"/ws"}
	m.layout()
	if got := frameLines(m.View()); got != m.height {
		t.Fatalf("rich approval frame = %d lines, want %d:\n%s", got, m.height, m.View())
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
		width:     34,
		modelName: "a-very-long-model-name-that-overflows-the-band",
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
	if w := lipgloss.Width(stripANSI(header)); w > 34 {
		t.Fatalf("header wider than terminal (%d > 34): %q", w, header)
	}
}

// The header band splits into brand (left) and working context (right):
// git branch plus workspace path. The session id no longer appears here.
func TestHeaderSplitsBrandAndContext(t *testing.T) {
	m := Model{
		theme:     NoColorTheme(),
		icons:     PlainIcons(),
		width:     100,
		modelName: "glm-5.2",
		sessionID: domain.NewSessionID(),
		workspace: "/ws/playground",
		gitBranch: "main",
	}
	header := stripANSI(m.renderHeader())
	if frameLines(header) != 1 {
		t.Fatalf("header wrapped to %d lines:\n%q", frameLines(header), header)
	}
	if !strings.Contains(header, "Loom · glm-5.2") {
		t.Fatalf("header missing brand/model: %q", header)
	}
	// The no-color theme has no edge padding, so the context string sits
	// flush against the right edge of the band.
	if !strings.HasSuffix(header, "main · /ws/playground") {
		t.Fatalf("context not right-aligned in band: %q", header)
	}
	if strings.Contains(header, "sess_") {
		t.Fatalf("session id must not occupy the header band: %q", header)
	}
	if w := lipgloss.Width(header); w != 100 {
		t.Fatalf("header width = %d cells, want exactly the terminal width 100", w)
	}
}

// Narrow terminals degrade the context gracefully: the path shrinks
// fish-style first, then to its basename, then the branch drops, until
// only the bare wordmark survives below 30 columns.
func TestHeaderDegradesContextWhenNarrow(t *testing.T) {
	base := Model{
		theme:     NoColorTheme(),
		icons:     PlainIcons(),
		modelName: "glm-5.2",
		workspace: "/very/long/workspace/path/that/will/not/fit/either",
		gitBranch: "feature/a-rather-long-branch-name",
	}

	narrow := base
	narrow.width = 46
	header := stripANSI(narrow.renderHeader())
	if !strings.Contains(header, "either") {
		t.Fatalf("directory name should survive narrow widths: %q", header)
	}
	if strings.Contains(header, "/very/long") || strings.Contains(header, "feature/") {
		t.Fatalf("full path and branch must shrink before the basename: %q", header)
	}

	tighter := base
	tighter.width = 36
	header = stripANSI(tighter.renderHeader())
	if !strings.Contains(header, "either") {
		t.Fatalf("basename should be the last context to go: %q", header)
	}
	if strings.Contains(header, "/") {
		t.Fatalf("only the bare basename fits at width 36: %q", header)
	}

	tiny := base
	tiny.width = 29
	header = stripANSI(tiny.renderHeader())
	if strings.TrimSpace(header) != "Loom" {
		t.Fatalf("width 29 must render the bare wordmark, got %q", header)
	}
}

// Branch detection stays silent outside git repositories and must never
// fail the frame.
func TestDetectGitBranchOutsideRepo(t *testing.T) {
	if got := detectGitBranch(t.TempDir()); got != "" {
		t.Fatalf("detectGitBranch(non-repo) = %q, want empty", got)
	}
	if got := detectGitBranch(""); got != "" {
		t.Fatalf("detectGitBranch(empty) = %q, want empty", got)
	}
}
