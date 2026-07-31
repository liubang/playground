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

// newSyncTestModel builds a model with n finalized blocks synced once.
func newSyncTestModel(n int) Model {
	m := Model{theme: NoColorTheme(), width: 80, height: 24, followTail: true}
	m.blocks = NewBlockIndex()
	m.viewport = lineView{Width: 80, Height: 20}
	for i := 0; i < n; i++ {
		id := string(rune('a' + i))
		block := &TranscriptBlock{ID: id, Kind: BlockKindAssistant, Title: "Assistant", Content: "answer " + id, Done: true}
		if id == "c" {
			// A tool block in the middle so expansion changes its height
			// deterministically (assistant text goes through glamour,
			// whose line count is harder to predict).
			block = &TranscriptBlock{ID: id, Kind: BlockKindTool, Title: "run_cmd", Status: "success", Done: true}
		}
		m.blocks.Add(block)
	}
	m.syncTranscript()
	return m
}

func TestSyncTranscriptFullBuildBaseline(t *testing.T) {
	m := newSyncTestModel(5)
	if m.lastSyncRendered != 5 {
		t.Fatalf("first sync rendered %d blocks, want 5", m.lastSyncRendered)
	}
	if got := m.viewport.TotalLineCount(); got != 9 {
		t.Fatalf("lines = %d, want 9 (5 blocks + 4 separators)", got)
	}
	wantOffsets := map[string]int{"a": 0, "b": 2, "c": 4, "d": 6, "e": 8}
	for id, want := range wantOffsets {
		if got := m.blockOffsets[id]; got != want {
			t.Fatalf("offset[%s] = %d, want %d", id, got, want)
		}
	}
}

// The streaming hot path: appending a block and updating the volatile
// tail must re-render only the new/changed blocks, never the stable
// prefix.
func TestSyncTranscriptIncrementalTailAppend(t *testing.T) {
	m := newSyncTestModel(5)

	m.blocks.Add(&TranscriptBlock{ID: "f", Kind: BlockKindAssistant, Title: "Assistant", Content: "partial", Done: false})
	m.syncTranscript()
	if m.lastSyncRendered != 1 {
		t.Fatalf("append rendered %d blocks, want 1 (the new tail)", m.lastSyncRendered)
	}
	if got := m.viewport.TotalLineCount(); got != 11 {
		t.Fatalf("lines = %d, want 11 (9 + separator + new tail)", got)
	}
	// The stable prefix kept its offsets.
	for id, want := range map[string]int{"a": 0, "b": 2, "c": 4, "d": 6, "e": 8} {
		if got := m.blockOffsets[id]; got != want {
			t.Fatalf("offset[%s] = %d, want %d (prefix must be preserved)", id, got, want)
		}
	}

	// A delta on the volatile tail: still just one block re-rendered.
	tail, _ := m.blocks.Get("f")
	tail.Content = "partial plus more"
	m.blocks.touch()
	m.syncTranscript()
	if m.lastSyncRendered != 1 {
		t.Fatalf("delta rendered %d blocks, want 1 (volatile tail only)", m.lastSyncRendered)
	}
	if !strings.Contains(m.viewport.View(), "partial plus more") {
		t.Fatalf("updated tail missing from view:\n%s", m.viewport.View())
	}
}

// Changing a middle block splices from that block onward: the changed
// block re-renders, later blocks reuse their cached lines, offsets shift
// correctly when the changed block's height changes.
func TestSyncTranscriptMiddleBlockChange(t *testing.T) {
	m := newSyncTestModel(5)

	middle, _ := m.blocks.Get("c")
	middle.Expanded = true
	middle.Preview = "extra line one\nextra line two"
	m.blocks.touch()
	m.syncTranscript()

	if m.lastSyncRendered != 1 {
		t.Fatalf("middle change rendered %d blocks, want 1 (later blocks hit the cache)", m.lastSyncRendered)
	}
	// c grew by two preview lines; d and e shifted accordingly.
	wantOffsets := map[string]int{"a": 0, "b": 2, "c": 4, "d": 8, "e": 10}
	for id, want := range wantOffsets {
		if got := m.blockOffsets[id]; got != want {
			t.Fatalf("offset[%s] = %d, want %d", id, got, want)
		}
	}
	if got := m.viewport.TotalLineCount(); got != 11 {
		t.Fatalf("lines = %d, want 11", got)
	}
	if !strings.Contains(m.viewport.View(), "extra line two") {
		t.Fatalf("expanded preview missing from view:\n%s", m.viewport.View())
	}
}

// Removing a block invalidates the positional prefix at the removal
// point; the rebuild must stay consistent (offsets, line count, no
// stale entries).
func TestSyncTranscriptBlockRemoval(t *testing.T) {
	m := newSyncTestModel(5)

	m.blocks.Remove("c")
	m.syncTranscript()

	if got := m.viewport.TotalLineCount(); got != 7 {
		t.Fatalf("lines = %d, want 7 (4 blocks + 3 separators)", got)
	}
	wantOffsets := map[string]int{"a": 0, "b": 2, "d": 4, "e": 6}
	for id, want := range wantOffsets {
		if got := m.blockOffsets[id]; got != want {
			t.Fatalf("offset[%s] = %d, want %d", id, got, want)
		}
	}
	if _, ok := m.blockOffsets["c"]; ok {
		t.Fatal("removed block must not keep an offset entry")
	}
	view := m.viewport.View()
	if strings.Contains(view, "run_cmd") {
		t.Fatalf("removed block still rendered:\n%s", view)
	}
	if !strings.Contains(view, "answer e") {
		t.Fatalf("tail block missing after removal:\n%s", view)
	}
}

// A full invalidation (width change) re-renders every block.
func TestSyncTranscriptWidthChangeRebuildsAll(t *testing.T) {
	m := newSyncTestModel(5)
	m.width = 40
	m.viewport.Width = 40
	m.syncTranscript()
	if m.lastSyncRendered != 5 {
		t.Fatalf("width change rendered %d blocks, want 5 (full rebuild)", m.lastSyncRendered)
	}
}

// The memo gate: with no volatile blocks and no changes, a sync is a
// no-op — the behavior TestFloatsDoNotRebuildTranscript relies on.
func TestSyncTranscriptMemoSkipsUnchanged(t *testing.T) {
	m := newSyncTestModel(3)
	builds := m.transcriptBuilds
	m.syncTranscript()
	m.syncTranscript()
	if m.transcriptBuilds != builds {
		t.Fatalf("unchanged syncs ran %d builds, want %d", m.transcriptBuilds, builds)
	}
}
