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
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func TestSubagentBlockLifecycleEvents(t *testing.T) {
	idx := NewBlockIndex()
	callID := domain.NewToolCallID()
	childID := domain.NewSessionID()

	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindToolPrepared, runtimeevent.ToolPreparedPayload{
		CallID:   callID,
		ToolName: "delegate_task",
		Risk:     domain.R1,
		Target:   "research X",
	}))
	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindSubagentStarted, runtimeevent.SubagentStartedPayload{
		CallID:         callID,
		ChildSessionID: childID,
		Task:           "research X",
	}))
	block, ok := idx.Get("tool-" + callID.String())
	if !ok || block.Subagent == nil || block.Subagent.ChildID != childID {
		t.Fatalf("after started: block = %#v", block)
	}
	if block.Subagent.Outcome != "" {
		t.Fatalf("outcome = %q, want empty while running", block.Subagent.Outcome)
	}

	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindSubagentProgress, runtimeevent.SubagentProgressPayload{
		CallID:         callID,
		ChildSessionID: childID,
		ToolCalls:      5,
		InputTokens:    10_000,
		OutputTokens:   2_300,
	}))
	if block.Subagent.ToolCalls != 5 || block.Subagent.InputTokens != 10_000 || block.Subagent.OutputTokens != 2_300 {
		t.Fatalf("after progress: state = %#v", block.Subagent)
	}

	ApplyRuntimeEvent(idx, toolEvent(t, runtimeevent.KindSubagentFinished, runtimeevent.SubagentFinishedPayload{
		CallID:         callID,
		ChildSessionID: childID,
		Outcome:        "succeeded",
		ToolCalls:      7,
		InputTokens:    12_000,
		OutputTokens:   3_000,
	}))
	if block.Subagent.Outcome != "succeeded" || block.Subagent.ToolCalls != 7 {
		t.Fatalf("after finished: state = %#v", block.Subagent)
	}
}

func TestLatestSubagentBlockPrefersActive(t *testing.T) {
	m := Model{theme: NoColorTheme(), blocks: NewBlockIndex()}
	finished := &TranscriptBlock{
		ID: "tool-1", Kind: BlockKindTool, Done: true,
		Subagent: &SubagentBlockState{ChildID: domain.NewSessionID(), Outcome: "succeeded"},
	}
	active := &TranscriptBlock{
		ID: "tool-2", Kind: BlockKindTool,
		Subagent: &SubagentBlockState{ChildID: domain.NewSessionID()},
	}
	plain := &TranscriptBlock{ID: "tool-3", Kind: BlockKindTool}
	m.blocks.Add(finished)
	m.blocks.Add(plain)
	m.blocks.Add(active)

	if got := m.latestSubagentBlock(); got != active {
		t.Fatalf("latest = %#v, want the active delegation", got)
	}
	active.Subagent.Outcome = "failed"
	if got := m.latestSubagentBlock(); got != active {
		t.Fatalf("with none active, latest = %#v, want the most recent delegation", got)
	}
}

func TestRenderSubagentProgress(t *testing.T) {
	m := Model{theme: NoColorTheme()}
	running := &TranscriptBlock{Subagent: &SubagentBlockState{ToolCalls: 5, InputTokens: 10_000, OutputTokens: 2_300}}
	if line := m.renderSubagentProgress(running); !strings.Contains(line, "exploring") ||
		!strings.Contains(line, "5 calls") || !strings.Contains(line, "12k tok") {
		t.Fatalf("running progress = %q", line)
	}
	finished := &TranscriptBlock{Subagent: &SubagentBlockState{
		ToolCalls: 7, InputTokens: 20_000, OutputTokens: 5_000, Outcome: "succeeded"}}
	if line := m.renderSubagentProgress(finished); !strings.Contains(line, "succeeded") ||
		!strings.Contains(line, "Ctrl+G") {
		t.Fatalf("finished progress = %q", line)
	}
	if line := m.renderSubagentProgress(&TranscriptBlock{}); line != "" {
		t.Fatalf("non-delegate block progress = %q, want empty", line)
	}
}

func TestRenderSubagentProgressQueuedHint(t *testing.T) {
	m := Model{theme: NoColorTheme()}
	// Serial execution: a second delegate_task in the same batch sits in
	// "prepared" with no child yet — the block must say so instead of
	// looking broken.
	queued := &TranscriptBlock{Tool: "delegate_task", Status: "prepared"}
	if line := m.renderSubagentProgress(queued); !strings.Contains(line, "queued") {
		t.Fatalf("queued delegate progress = %q, want the queued hint", line)
	}
	starting := &TranscriptBlock{Tool: "delegate_task", Status: "running"}
	if line := m.renderSubagentProgress(starting); !strings.Contains(line, "starting") {
		t.Fatalf("starting delegate progress = %q, want the starting hint", line)
	}
	// A completed or non-delegate block gets no queue hint.
	done := &TranscriptBlock{Tool: "delegate_task", Status: "success", Done: true}
	if line := m.renderSubagentProgress(done); line != "" {
		t.Fatalf("completed delegate without state = %q, want empty", line)
	}
	other := &TranscriptBlock{Tool: "read_file", Status: "prepared"}
	if line := m.renderSubagentProgress(other); line != "" {
		t.Fatalf("non-delegate prepared block = %q, want empty", line)
	}
}

func TestBuildSubagentContent(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	if out := m.buildSubagentContent(nil); !strings.Contains(out, "not produced any messages") {
		t.Fatalf("empty content = %q", out)
	}
	messages := []domain.Message{
		{
			ID: domain.NewMessageID(), Role: domain.RoleUser,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "research task"}},
			CreatedAt: time.Now(),
		},
		{
			ID: domain.NewMessageID(), Role: domain.RoleAssistant,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "conclusion here"}},
			CreatedAt: time.Now(),
		},
	}
	out := m.buildSubagentContent(messages)
	if !strings.Contains(out, "research task") || !strings.Contains(out, "conclusion here") {
		t.Fatalf("content = %q", out)
	}
}

func TestOpenSubagentOverlayRequiresDelegation(t *testing.T) {
	m := Model{theme: NoColorTheme(), blocks: NewBlockIndex(), width: 100, height: 30}
	next, cmd := m.openSubagentOverlay()
	if cmd != nil {
		t.Fatal("no delegation must not produce a fetch command")
	}
	got := next.(Model)
	if got.mode == ModeSubagent || got.subOverlay != nil {
		t.Fatal("overlay must not open without a delegation")
	}
	if !strings.Contains(got.statusMessage, "No sub-agent run") {
		t.Fatalf("status = %q", got.statusMessage)
	}
}

func TestClickDelegateBlockOpensOverlay(t *testing.T) {
	delegateBlock := &TranscriptBlock{
		ID: "tool-1", Kind: BlockKindTool, Tool: "delegate_task", Target: "research X",
		Subagent: &SubagentBlockState{ChildID: domain.NewSessionID()},
	}
	plainBlock := &TranscriptBlock{ID: "tool-2", Kind: BlockKindTool, Tool: "read_file"}
	m := Model{
		theme: NoColorTheme(), width: 100, height: 30,
		blocks:       NewBlockIndex(),
		blockOffsets: map[string]int{"tool-1": 0, "tool-2": 2},
		mode:         ModeChat,
	}
	m.blocks.Add(delegateBlock)
	m.blocks.Add(plainBlock)

	// A click on the delegate block's row (screen row 1 = header + content 0)
	// opens the drill-in for THAT delegation.
	next, cmd := m.handleMouse(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonLeft, Y: 1})
	got := next.(Model)
	if got.mode != ModeSubagent || got.subOverlay == nil || got.subOverlay.childID != delegateBlock.Subagent.ChildID {
		t.Fatalf("click on delegate block: mode = %s, overlay = %#v", got.mode, got.subOverlay)
	}
	if cmd == nil {
		t.Fatal("opening the overlay must schedule the checkpoint fetch")
	}

	// A click on a non-delegate block does nothing sub-agent related.
	m2 := Model{
		theme: NoColorTheme(), width: 100, height: 30,
		blocks:       m.blocks,
		blockOffsets: map[string]int{"tool-1": 0, "tool-2": 2},
		mode:         ModeChat,
	}
	next, _ = m2.handleMouse(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonLeft, Y: 3})
	got2 := next.(Model)
	if got2.mode == ModeSubagent || got2.subOverlay != nil {
		t.Fatalf("click on plain block must not open the overlay: mode = %s", got2.mode)
	}
}

func TestSubagentOverlayKeyHandling(t *testing.T) {
	m := Model{
		theme: NoColorTheme(), blocks: NewBlockIndex(), width: 100, height: 30,
		subOverlay: &subagentOverlay{childID: domain.NewSessionID()},
		mode:       ModeSubagent,
	}
	// Esc closes back to chat and drops the overlay.
	next, _ := m.handleSubagentKey(tea.KeyMsg{Type: tea.KeyEsc})
	got := next.(Model)
	if got.mode != ModeChat || got.subOverlay != nil {
		t.Fatalf("after Esc: mode = %s, overlay = %#v", got.mode, got.subOverlay)
	}

	// Scrolling does not close; follow-tail disengages on scroll up.
	m2 := Model{
		theme: NoColorTheme(), blocks: NewBlockIndex(), width: 100, height: 30,
		subOverlay: &subagentOverlay{childID: domain.NewSessionID(), followTail: true},
		mode:       ModeSubagent,
	}
	next, _ = m2.handleSubagentKey(tea.KeyMsg{Type: tea.KeyUp})
	got2 := next.(Model)
	if got2.mode != ModeSubagent || got2.subOverlay.followTail {
		t.Fatalf("scroll up must stay open and disengage follow-tail: mode = %s", got2.mode)
	}
}
