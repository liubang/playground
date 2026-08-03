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
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestListToolDefinition(t *testing.T) {
	s := newTestStore(t)
	tool, err := NewListTool(s)
	if err != nil {
		t.Fatalf("NewListTool: %v", err)
	}
	def := tool.Definition()
	if def.Name != ToolList {
		t.Errorf("Name = %q, want %q", def.Name, ToolList)
	}
	if !tool.ConcurrentSafe() {
		t.Error("ListTool should be concurrent-safe")
	}
}

func TestListToolExecute(t *testing.T) {
	s := newTestStore(t)
	s.WriteSummary("test summary")
	s.WriteMain("test main")
	tool, _ := NewListTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolList,
		Arguments: json.RawMessage(`{}`),
	}
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Errorf("Status = %s, want %s", result.Status, domain.ToolStatusSuccess)
		if result.Error != nil {
			t.Logf("Error: %s: %s", result.Error.Code, result.Error.Message)
		}
	}
}

func TestListToolWithMaxResults(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewListTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolList,
		Arguments: json.RawMessage(`{"max_results":1}`),
	}
	prepared, _ := tool.Prepare(context.Background(), call)
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

func TestListToolInvalidArgs(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewListTool(s)
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolList,
		Arguments: json.RawMessage(`{invalid}`),
	}
	_, err := tool.Prepare(context.Background(), call)
	if err == nil {
		t.Error("expected error for invalid JSON")
	}
}

func TestReadToolDefinition(t *testing.T) {
	s := newTestStore(t)
	tool, err := NewReadTool(s)
	if err != nil {
		t.Fatalf("NewReadTool: %v", err)
	}
	if tool.Definition().Name != ToolRead {
		t.Errorf("Name = %q, want %q", tool.Definition().Name, ToolRead)
	}
}

func TestReadToolExecute(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("line1\nline2\nline3")
	tool, _ := NewReadTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolRead,
		Arguments: json.RawMessage(`{"path":"MEMORY.md"}`),
	}
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
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

func TestReadToolMissingPath(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewReadTool(s)
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolRead,
		Arguments: json.RawMessage(`{}`),
	}
	_, err := tool.Prepare(context.Background(), call)
	if err == nil {
		t.Error("expected error for missing path")
	}
}

func TestReadToolWithOffset(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("line1\nline2\nline3")
	tool, _ := NewReadTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolRead,
		Arguments: json.RawMessage(`{"path":"MEMORY.md","line_offset":2,"max_lines":1}`),
	}
	prepared, _ := tool.Prepare(context.Background(), call)
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

func TestSearchToolDefinition(t *testing.T) {
	s := newTestStore(t)
	tool, err := NewSearchTool(s)
	if err != nil {
		t.Fatalf("NewSearchTool: %v", err)
	}
	if tool.Definition().Name != ToolSearch {
		t.Errorf("Name = %q", tool.Definition().Name)
	}
}

func TestSearchToolExecute(t *testing.T) {
	s := newTestStore(t)
	content := "# Preferences\n\nUser prefers Go over Python"
	if err := s.WriteMain(content); err != nil {
		t.Fatalf("WriteMain: %v", err)
	}

	tool, _ := NewSearchTool(s)
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolSearch,
		Arguments: json.RawMessage(`{"query":"prefers Go"}`),
	}
	prepared, _ := tool.Prepare(context.Background(), call)
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

func TestSearchToolEmptyQuery(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewSearchTool(s)
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolSearch,
		Arguments: json.RawMessage(`{"query":"  "}`),
	}
	_, err := tool.Prepare(context.Background(), call)
	if err == nil {
		t.Error("expected error for empty query")
	}
}

func TestAddNoteToolDefinition(t *testing.T) {
	s := newTestStore(t)
	tool, err := NewAddNoteTool(s)
	if err != nil {
		t.Fatalf("NewAddNoteTool: %v", err)
	}
	if tool.Definition().Name != ToolAddNote {
		t.Errorf("Name = %q", tool.Definition().Name)
	}
}

func TestAddNoteToolExecute(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewAddNoteTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolAddNote,
		Arguments: json.RawMessage(`{"filename":"2026-08-02T12-00-00-prefer-go.md","note":"User prefers Go"}`),
	}
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
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

func TestAddNoteToolInvalidFilename(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewAddNoteTool(s)

	// Missing timestamp prefix.
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolAddNote,
		Arguments: json.RawMessage(`{"filename":"prefer-go.md","note":"User prefers Go"}`),
	}
	_, err := tool.Prepare(context.Background(), call)
	if err == nil {
		t.Error("expected error for invalid filename")
	}
}

func TestAddNoteToolEmptyNote(t *testing.T) {
	s := newTestStore(t)
	tool, _ := NewAddNoteTool(s)

	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      ToolAddNote,
		Arguments: json.RawMessage(`{"filename":"2026-08-02T12-00-00-test.md","note":"  "}`),
	}
	_, err := tool.Prepare(context.Background(), call)
	if err == nil {
		t.Error("expected error for empty note")
	}
}

func TestNewListToolNilStore(t *testing.T) {
	_, err := NewListTool(nil)
	if err == nil {
		t.Error("expected error for nil store")
	}
}

func TestNewReadToolNilStore(t *testing.T) {
	_, err := NewReadTool(nil)
	if err == nil {
		t.Error("expected error for nil store")
	}
}

func TestNewSearchToolNilStore(t *testing.T) {
	_, err := NewSearchTool(nil)
	if err == nil {
		t.Error("expected error for nil store")
	}
}

func TestNewAddNoteToolNilStore(t *testing.T) {
	_, err := NewAddNoteTool(nil)
	if err == nil {
		t.Error("expected error for nil store")
	}
}
