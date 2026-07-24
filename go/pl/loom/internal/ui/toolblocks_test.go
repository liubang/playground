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
// Created: 2026/07/24

package ui

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/charmbracelet/bubbles/textarea"
	"github.com/charmbracelet/bubbles/viewport"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func toolEvent(t *testing.T, kind runtimeevent.RuntimeEventKind, payload any) runtimeevent.RuntimeEvent {
	t.Helper()
	raw, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	return runtimeevent.RuntimeEvent{Kind: kind, Payload: raw}
}

func TestToolPreparedSetsTarget(t *testing.T) {
	idx := NewBlockIndex()
	callID := domain.NewToolCallID()
	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindToolPrepared, runtimeevent.ToolPreparedPayload{
		CallID:   callID,
		ToolName: "read_file",
		Risk:     domain.R0,
		Target:   "internal/ui/view.go",
	}))
	block, ok := idx.Get("tool-" + callID.String())
	if !ok {
		t.Fatal("tool block missing")
	}
	if block.Target != "internal/ui/view.go" {
		t.Fatalf("Target = %q, want internal/ui/view.go", block.Target)
	}
}

func TestApprovalRequestedSetsTargetFromPaths(t *testing.T) {
	idx := NewBlockIndex()
	callID := domain.NewToolCallID()
	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindApprovalRequested, runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		CallID:     callID,
		ToolName:   "replace_text",
		Risk:       domain.R2,
		WritePaths: []string{"a.go", "b.go"},
	}))
	block, ok := idx.Get("tool-" + callID.String())
	if !ok {
		t.Fatal("approval block missing")
	}
	if block.Target != "a.go" {
		t.Fatalf("Target = %q, want first write path a.go", block.Target)
	}
}

func TestToolCompletedSetsPreviewAndDetail(t *testing.T) {
	idx := NewBlockIndex()
	callID := domain.NewToolCallID()
	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindToolPrepared, runtimeevent.ToolPreparedPayload{
		CallID: callID, ToolName: "run_cmd", Risk: domain.R1, Target: "go test ./...",
	}))
	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindToolCompleted, runtimeevent.ToolCompletedPayload{
		CallID:   callID,
		ToolName: "run_cmd",
		Status:   domain.ToolStatusError,
		Error:    "exit_code:1",
		Preview:  "ok\tpkg/a\nFAIL\tpkg/b\nexit status 1",
	}))
	block, _ := idx.Get("tool-" + callID.String())
	if block.Status != "error" {
		t.Fatalf("Status = %q, want error", block.Status)
	}
	if !strings.Contains(block.Preview, "FAIL") {
		t.Fatalf("Preview = %q, want failure output", block.Preview)
	}
	if !strings.Contains(block.Detail, "exit_code:1") {
		t.Fatalf("Detail = %q, want error code", block.Detail)
	}
}

func TestToggleLatestToolOutput(t *testing.T) {
	idx := NewBlockIndex()
	idx.Add(&TranscriptBlock{ID: "tool-1", Kind: BlockKindTool, Title: "a", Status: "success"})
	idx.Add(&TranscriptBlock{ID: "tool-2", Kind: BlockKindTool, Title: "b", Status: "success", Preview: "out"})

	if !idx.ToggleLatestToolOutput() {
		t.Fatal("toggle should succeed")
	}
	older, _ := idx.Get("tool-1")
	if older.Expanded {
		t.Fatal("block without preview must not expand")
	}
	latest, _ := idx.Get("tool-2")
	if !latest.Expanded {
		t.Fatal("latest block with preview should be expanded")
	}
	if !idx.ToggleLatestToolOutput() || latest.Expanded {
		t.Fatal("second toggle should collapse")
	}
}

func TestToolSummaryShowsHierarchyTargetAndPreview(t *testing.T) {
	m := Model{theme: NoColorTheme()}
	m.SetIcons(PlainIcons())
	block := &TranscriptBlock{
		Kind:    BlockKindTool,
		Title:   "read_file",
		Status:  "success",
		Target:  "internal/ui/view.go",
		Detail:  "1ms",
		Preview: "line1\nline2",
	}

	summary := m.renderToolSummary(block)
	// Order: name first, target second, detail last.
	nameIdx := strings.Index(summary, "read_file")
	targetIdx := strings.Index(summary, "internal/ui/view.go")
	detailIdx := strings.Index(summary, "1ms")
	if nameIdx < 0 || targetIdx <= nameIdx || detailIdx <= targetIdx {
		t.Fatalf("summary parts out of order: %q", summary)
	}

	// Collapsed: preview hidden. Expanded: preview shown.
	if view := m.renderBlock(block); strings.Contains(view, "line2") {
		t.Fatalf("collapsed tool block should hide preview:\n%s", view)
	}
	block.Expanded = true
	view := m.renderBlock(block)
	if !strings.Contains(view, "line1") || !strings.Contains(view, "line2") {
		t.Fatalf("expanded tool block should show preview:\n%s", view)
	}
}

func TestRebuildTranscriptExtractsTargetAndPreview(t *testing.T) {
	callID := domain.NewToolCallID()
	messages := []domain.Message{
		{
			ID: domain.NewMessageID(), Role: domain.RoleAssistant, Sequence: 1,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{
					ID: callID, Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`),
				}},
			},
			CreatedAt: time.Now().UTC(),
		},
		{
			ID: domain.NewMessageID(), Role: domain.RoleAssistant, Sequence: 2,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
					CallID:     callID,
					Status:     domain.ToolStatusSuccess,
					Content:    []domain.ContentPart{{Kind: domain.PartText, Text: "package ui\n\nimport ..."}},
					StartedAt:  time.Now().UTC(),
					FinishedAt: time.Now().UTC(),
				}},
			},
			CreatedAt: time.Now().UTC(),
		},
	}
	idx := RebuildTranscript(messages)
	block, ok := idx.Get("tool-" + callID.String())
	if !ok {
		t.Fatal("rebuilt tool block missing")
	}
	if block.Target != "a.go" {
		t.Fatalf("Target = %q, want a.go", block.Target)
	}
	if !strings.Contains(block.Preview, "package ui") {
		t.Fatalf("Preview = %q, want result content", block.Preview)
	}
}

func TestToolTargetFromArgs(t *testing.T) {
	tests := []struct {
		name string
		args string
		want string
	}{
		{"path wins over pattern", `{"path":"a.go","pattern":"*.go"}`, "a.go"},
		{"command", `{"command":"go test ./..."}`, "go test ./..."},
		{"pattern", `{"pattern":"**/*.go"}`, "**/*.go"},
		{"non-string ignored", `{"path":42,"cmd":"ls"}`, "ls"},
		{"empty", `{}`, ""},
		{"invalid json", `{`, ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := toolTargetFromArgs(json.RawMessage(tt.args)); got != tt.want {
				t.Fatalf("toolTargetFromArgs(%s) = %q, want %q", tt.args, got, tt.want)
			}
		})
	}
}

func TestToggleAllToolOutputs(t *testing.T) {
	idx := NewBlockIndex()
	idx.Add(&TranscriptBlock{ID: "tool-1", Kind: BlockKindTool, Title: "a", Status: "success", Preview: "out1"})
	idx.Add(&TranscriptBlock{ID: "tool-2", Kind: BlockKindTool, Title: "b", Status: "success", Diff: "+ x"})
	idx.Add(&TranscriptBlock{ID: "tool-3", Kind: BlockKindTool, Title: "c", Status: "success"}) // no content

	if !idx.ToggleAllToolOutputs() {
		t.Fatal("expand-all should report a change")
	}
	for _, id := range []string{"tool-1", "tool-2"} {
		if b, _ := idx.Get(id); !b.Expanded {
			t.Fatalf("%s should be expanded", id)
		}
	}
	if b, _ := idx.Get("tool-3"); b.Expanded {
		t.Fatal("block without content must stay collapsed")
	}

	// Second toggle collapses everything.
	if !idx.ToggleAllToolOutputs() {
		t.Fatal("collapse-all should report a change")
	}
	for _, id := range []string{"tool-1", "tool-2"} {
		if b, _ := idx.Get(id); b.Expanded {
			t.Fatalf("%s should be collapsed", id)
		}
	}
}

func TestLatestFinalAssistantText(t *testing.T) {
	idx := NewBlockIndex()
	if got := idx.LatestFinalAssistantText(); got != "" {
		t.Fatalf("empty index = %q, want empty", got)
	}
	idx.Add(&TranscriptBlock{ID: "a1", Kind: BlockKindAssistant, Content: "first", Done: true})
	idx.Add(&TranscriptBlock{ID: "a2", Kind: BlockKindAssistant, Content: "streaming…"}) // not done
	if got := idx.LatestFinalAssistantText(); got != "first" {
		t.Fatalf("latest final = %q, want first", got)
	}
}

func TestToolBlockExpandedPrefersDiff(t *testing.T) {
	m := Model{theme: NoColorTheme()}
	m.SetIcons(PlainIcons())
	block := &TranscriptBlock{
		Kind:     BlockKindTool,
		Title:    "edit",
		Status:   "success",
		Target:   "a.go",
		Diff:     "- old line\n+ new line",
		Preview:  `{"path":"a.go"}`,
		Expanded: true,
	}
	view := m.renderBlock(block)
	if !strings.Contains(view, "- old line") || !strings.Contains(view, "+ new line") {
		t.Fatalf("expanded edit block should show diff:\n%s", view)
	}
	if strings.Contains(view, "old_hash") {
		t.Fatalf("diff view must not fall back to JSON preview:\n%s", view)
	}
}

func TestSearchFlowMatchesAndJumps(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 80, height: 24}
	m.blocks = NewBlockIndex()
	m.viewport = viewport.New(80, 2) // short viewport so the transcript scrolls
	m.textArea = textarea.New()
	m.blocks.Add(&TranscriptBlock{ID: "u1", Kind: BlockKindUser, Title: "You", Content: "hello world", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "a1", Kind: BlockKindAssistant, Title: "Assistant", Content: "hi there", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "t1", Kind: BlockKindTool, Title: "run_cmd", Status: "success", Preview: "FAIL pkg/b", Done: true})
	m.syncTranscript()

	m.enterSearch()
	if m.mode != ModeSearch {
		t.Fatal("enterSearch should switch mode")
	}
	m.searchQuery = "fail"
	m.updateSearch()
	if len(m.searchMatches) != 1 || m.searchMatches[0] != "t1" {
		t.Fatalf("matches = %v, want [t1]", m.searchMatches)
	}
	if m.viewport.YOffset == 0 {
		t.Fatal("jump should scroll to the matching block")
	}

	// Case-insensitive multi-match cycling.
	m.searchQuery = "H"
	m.updateSearch()
	if len(m.searchMatches) != 2 {
		t.Fatalf("matches = %v, want 2 hits (hello/hi)", m.searchMatches)
	}
	first := m.searchIndex
	m.nextSearchMatch()
	if m.searchIndex == first {
		t.Fatal("nextSearchMatch should advance")
	}

	m.exitSearch()
	if m.mode != ModeChat || m.searchQuery != "" {
		t.Fatal("exitSearch should reset search state")
	}
}

func TestRenderWelcomeFitsWidth(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100, modelName: "test-model", workspace: "/tmp/ws"}
	wide := m.renderWelcome()
	if !strings.Contains(wide, "test-model") || !strings.Contains(wide, "/tmp/ws") {
		t.Fatalf("welcome should show model and workspace:\n%s", wide)
	}
	if !strings.Contains(wide, "██") {
		t.Fatalf("wide welcome should include the logo:\n%s", wide)
	}

	m.width = 30
	narrow := m.renderWelcome()
	if strings.Contains(narrow, "██") {
		t.Fatalf("narrow welcome should drop the logo:\n%s", narrow)
	}
	if !strings.Contains(narrow, "Loom") {
		t.Fatalf("narrow welcome should keep the wordmark:\n%s", narrow)
	}
}

func TestLightThemeProfile(t *testing.T) {
	dark := DefaultTheme()
	light := LightTheme()
	if dark.MarkdownProfile != "dark" || light.MarkdownProfile != "light" {
		t.Fatalf("profiles = %q/%q, want dark/light", dark.MarkdownProfile, light.MarkdownProfile)
	}
	if dark.Primary == light.Primary {
		t.Fatal("dark and light themes must use different accent colors")
	}
	if NoColorTheme().MarkdownProfile != "notty" {
		t.Fatal("no-color theme must use the notty profile")
	}
}

func TestToolResultPreviewTextBoundsOutput(t *testing.T) {
	long := strings.Repeat("line\n", 50)
	result := domain.ToolResult{
		Status:  domain.ToolStatusSuccess,
		Content: []domain.ContentPart{{Kind: domain.PartText, Text: long}},
	}
	preview := toolResultPreviewText(result)
	lines := strings.Split(preview, "\n")
	// 12 bounded lines + ellipsis marker.
	if len(lines) > toolPreviewMaxLines+1 {
		t.Fatalf("preview lines = %d, want <= %d", len(lines), toolPreviewMaxLines+1)
	}
	if !strings.HasSuffix(preview, "…") {
		t.Fatalf("truncated preview should end with ellipsis: %q", preview[len(preview)-20:])
	}

	errResult := domain.ToolResult{
		Status: domain.ToolStatusError,
		Error:  &domain.ToolError{Code: "x", Message: "something failed"},
	}
	if got := toolResultPreviewText(errResult); got != "something failed" {
		t.Fatalf("error preview = %q, want error message", got)
	}
}
