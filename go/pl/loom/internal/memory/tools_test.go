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
// Created: 2026/08/02

package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestMemoryToolDefinition(t *testing.T) {
	s := newTestStore(t)
	tool, err := NewMemoryTool(s)
	if err != nil {
		t.Fatalf("NewMemoryTool: %v", err)
	}
	def := tool.Definition()
	if def.Name != ToolMemory {
		t.Errorf("Name = %q, want %q", def.Name, ToolMemory)
	}
	if got := def.Risk(); got != domain.R0 {
		t.Errorf("definition Risk() = %v, want R0 (capability-free; per-action grading)", got)
	}
	if !tool.ConcurrentSafe() {
		t.Error("MemoryTool should be concurrent-safe")
	}
}

func TestNewMemoryToolNilStore(t *testing.T) {
	if _, err := NewMemoryTool(nil); err == nil {
		t.Error("expected error for nil store")
	}
}

func prepareMemory(t *testing.T, tool *MemoryTool, args string) domain.PreparedCall {
	t.Helper()
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolMemory,
		Arguments: json.RawMessage(args),
	})
	if err != nil {
		t.Fatalf("Prepare(%s): %v", args, err)
	}
	return prepared
}

func TestMemoryToolRiskPerAction(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	for action, args := range map[string]string{
		"list":   `{"action":"list"}`,
		"read":   `{"action":"read","path":"MEMORY.md"}`,
		"search": `{"action":"search","query":"x"}`,
	} {
		if prepared := prepareMemory(t, tool, args); prepared.Risk != domain.R1 {
			t.Errorf("%s: Risk = %v, want R1", action, prepared.Risk)
		}
	}
	addNote := prepareMemory(t, tool,
		`{"action":"add_note","filename":"2026-08-02T12-00-00-x.md","note":"n"}`)
	if addNote.Risk != domain.R2 {
		t.Errorf("add_note: Risk = %v, want R2", addNote.Risk)
	}
}

func TestMemoryToolPrepareValidation(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	for name, args := range map[string]string{
		"invalid json":          `{invalid}`,
		"missing action":        `{}`,
		"unknown action":        `{"action":"delete"}`,
		"read missing path":     `{"action":"read"}`,
		"search empty query":    `{"action":"search","query":"  "}`,
		"add_note bad filename": `{"action":"add_note","filename":"Bad Slug.md","note":"User prefers Go"}`,
		"add_note traversal":    `{"action":"add_note","filename":"../escape.md","note":"User prefers Go"}`,
		"add_note empty note":   `{"action":"add_note","filename":"2026-08-02T12-00-00-test.md","note":"  "}`,
		"unknown field":         `{"action":"list","extra":true}`,
	} {
		call := domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      ToolMemory,
			Arguments: json.RawMessage(args),
		}
		if _, err := tool.Prepare(context.Background(), call); err == nil {
			t.Errorf("%s: expected prepare error", name)
		}
	}
}

// add_note accepts a bare slug (or no filename at all) and prepends the
// current UTC timestamp itself, so the model never burns a run_cmd date
// call to satisfy the filename convention.
func TestMemoryToolAddNoteAutoTimestamp(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"add_note","filename":"prefer-go.md","note":"User prefers Go"}`)
	var args memoryArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		t.Fatalf("canonical args undecodable: %v", err)
	}
	if !noteFilePattern.MatchString(args.Filename) {
		t.Fatalf("auto-timestamped filename %q does not match the canonical pattern", args.Filename)
	}
	if !strings.HasSuffix(args.Filename, "-prefer-go.md") {
		t.Fatalf("slug lost in filename %q", args.Filename)
	}
	if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute status = %s: %+v", result.Status, result.Error)
	}

	// No filename at all falls back to the default slug.
	prepared = prepareMemory(t, tool, `{"action":"add_note","note":"another note"}`)
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		t.Fatalf("canonical args undecodable: %v", err)
	}
	if !strings.HasSuffix(args.Filename, "-note.md") {
		t.Fatalf("default filename = %q, want *-note.md", args.Filename)
	}

	// A fully timestamped name passes through untouched.
	prepared = prepareMemory(t, tool, `{"action":"add_note","filename":"2026-08-02T12-00-00-x.md","note":"n"}`)
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		t.Fatalf("canonical args undecodable: %v", err)
	}
	if args.Filename != "2026-08-02T12-00-00-x.md" {
		t.Fatalf("timestamped filename rewritten: %q", args.Filename)
	}
}

// Reading a memory file that does not exist must fail with actionable
// guidance, not a raw OS error leaking the store's absolute root.
func TestMemoryToolReadNotFoundGuidance(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"read","path":"MEMORY.md"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("Status = %s, want error", result.Status)
	}
	if result.Error == nil || !strings.Contains(result.Error.Message, "memory file not found") ||
		!strings.Contains(result.Error.Message, "action=list") {
		t.Fatalf("error = %+v, want not-found guidance pointing at list", result.Error)
	}
	if strings.Contains(result.Error.Message, s.root) {
		t.Fatalf("error leaks the store root: %q", result.Error.Message)
	}
	if result.Error.Retryable {
		t.Fatal("not-found must not be retryable")
	}
}

func TestMemoryToolListExecute(t *testing.T) {
	s := newTestStore(t)
	s.WriteSummary("test summary")
	s.WriteMain("test main")
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"list"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s, error = %+v", result.Status, result.Error)
	}
}

func TestMemoryToolListWithMaxResults(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"list","max_results":1}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s", result.Status)
	}
	var output struct {
		Entries []FileEntry `json:"entries"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &output); err != nil {
		t.Fatalf("parse output: %v", err)
	}
	if len(output.Entries) > 1 {
		t.Errorf("expected at most 1 entry, got %d", len(output.Entries))
	}
}

func TestMemoryToolReadExecute(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("line1\nline2\nline3")
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"read","path":"MEMORY.md"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s", result.Status)
	}
	var output struct {
		Content    string `json:"content"`
		TotalLines int    `json:"total_lines"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &output); err != nil {
		t.Fatalf("parse output: %v", err)
	}
	if output.Content != "line1\nline2\nline3" {
		t.Errorf("Content = %q", output.Content)
	}
}

func TestMemoryToolReadWithOffset(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("line1\nline2\nline3")
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool, `{"action":"read","path":"MEMORY.md","line_offset":2,"max_lines":1}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s", result.Status)
	}
	var output struct {
		Content    string `json:"content"`
		TotalLines int    `json:"total_lines"`
	}
	json.Unmarshal([]byte(result.Content[0].Text), &output)
	if output.Content != "line2" {
		t.Errorf("Content = %q, want 'line2'", output.Content)
	}
}

func TestMemoryToolSearchExecute(t *testing.T) {
	s := newTestStore(t)
	content := "# Preferences\n\nUser prefers Go over Python"
	if err := s.WriteMain(content); err != nil {
		t.Fatalf("WriteMain: %v", err)
	}

	tool, _ := NewMemoryTool(s)
	prepared := prepareMemory(t, tool, `{"action":"search","query":"prefers Go"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s", result.Status)
	}
	var output struct {
		Matches []SearchMatch `json:"matches"`
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), &output); err != nil {
		t.Fatalf("parse output: %v, raw: %s", err, result.Content[0].Text)
	}
	if len(output.Matches) == 0 {
		t.Errorf("expected at least 1 match, raw output: %s", result.Content[0].Text)
	}
}

func TestMemoryToolAddNoteExecute(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewMemoryTool(s)

	prepared := prepareMemory(t, tool,
		`{"action":"add_note","filename":"2026-08-02T12-00-00-prefer-go.md","note":"User prefers Go"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Status = %s", result.Status)
	}
	// Verify the note was actually written.
	notes, _ := s.ListNotes()
	if len(notes) != 1 {
		t.Errorf("expected 1 note, got %d", len(notes))
	}
}
