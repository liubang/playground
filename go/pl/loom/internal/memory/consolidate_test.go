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
	"strings"
	"testing"
)

func TestExtractAddedLinesBasic(t *testing.T) {
	diff := `diff --git a/raw_memories.md b/raw_memories.md
--- a/raw_memories.md
+++ b/raw_memories.md
@@ -0,0 +1,3 @@
+User prefers Go
+Dislikes Java
+Likes Vim`
	got := extractAddedLines(diff)
	wantLines := []string{"User prefers Go", "Dislikes Java", "Likes Vim"}
	for _, w := range wantLines {
		if !strings.Contains(got, w) {
			t.Errorf("extractAddedLines(%q): missing %q", diff, w)
		}
	}
}

func TestExtractAddedLinesPreservesDashes(t *testing.T) {
	// This is the critical bug: lines like "+---" (horizontal rule) or
	// "+--- " in YAML front-matter were incorrectly stripped because
	// the old code used HasPrefix(line, "---") which matched both the
	// diff header "--- a/file" and content lines "+---".
	diff := `--- a/raw_memories.md
+++ b/raw_memories.md
@@ -0,0 +1,4 @@
+---
+## Rollout: fix-build
+
+Some content`
	got := extractAddedLines(diff)
	// The content line "+---" must be preserved as "---" (without the "+" prefix).
	if !strings.Contains(got, "---\n") {
		t.Errorf("extractAddedLines: lost '---' separator, got:\n%s", got)
	}
	if !strings.Contains(got, "## Rollout: fix-build") {
		t.Errorf("extractAddedLines: lost content after '---', got:\n%s", got)
	}
	// The diff headers "--- a/..." and "+++ b/..." must NOT appear.
	if strings.Contains(got, "a/raw_memories.md") {
		t.Errorf("extractAddedLines: diff header '--- a/' leaked through, got:\n%s", got)
	}
	if strings.Contains(got, "b/raw_memories.md") {
		t.Errorf("extractAddedLines: diff header '+++' leaked through, got:\n%s", got)
	}
}

func TestStripMarkdownFences(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{"plain text", "hello world", "hello world"},
		{"markdown fence", "```markdown\nhello\n```", "hello"},
		{"md fence", "```md\nhello\n```", "hello"},
		{"generic fence", "```\nhello\n```", "hello"},
		{"generic with lang", "```go\nhello\n```", "hello"},
		{"empty", "", ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := stripMarkdownFences(tt.input)
			if got != tt.want {
				t.Errorf("stripMarkdownFences(%q) = %q, want %q", tt.input, got, tt.want)
			}
		})
	}
}

func TestAppendMemory(t *testing.T) {
	current := "# MEMORY.md\n\n## Preferences\n- Go"
	newContent := "User prefers dark theme"
	got := appendMemory(current, newContent)
	if !strings.Contains(got, current) {
		t.Error("appendMemory lost existing content")
	}
	if !strings.Contains(got, newContent) {
		t.Error("appendMemory lost new content")
	}
	if !strings.Contains(got, "---") {
		t.Error("appendMemory missing separator")
	}
}

func TestAppendMemoryEmptyCurrent(t *testing.T) {
	got := appendMemory("", "new content")
	if !strings.Contains(got, "new content") {
		t.Error("appendMemory lost new content with empty current")
	}
}

func TestTruncateToSummaryExact100(t *testing.T) {
	// Build a content with exactly 105 lines.
	var lines []string
	for i := 0; i < 105; i++ {
		lines = append(lines, "line")
	}
	content := strings.Join(lines, "\n")
	got := truncateToSummary(content)
	// Should have exactly 100 lines of content + 1 truncation notice.
	resultLines := strings.Split(got, "\n")
	contentLines := 0
	for _, l := range resultLines {
		if l == "line" {
			contentLines++
		}
	}
	if contentLines != 100 {
		t.Errorf("truncateToSummary kept %d content lines, want 100", contentLines)
	}
	if !strings.Contains(got, "truncated") {
		t.Error("truncateToSummary missing truncation notice")
	}
}

func TestTruncateToSummaryShort(t *testing.T) {
	content := "short\ncontent"
	got := truncateToSummary(content)
	if got != content {
		t.Errorf("truncateToSummary of short content = %q, want %q", got, content)
	}
}

func TestTruncateToSummaryEmpty(t *testing.T) {
	got := truncateToSummary("")
	if got != "" {
		t.Errorf("truncateToSummary('') = %q, want empty", got)
	}
}
