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

package render

import (
	"encoding/json"
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"
)

// Regression (REVIEW R10): truncateDiffLine cut at a byte offset, which
// could split a multi-byte rune (200 % 3 = 2 lands inside a 3-byte char).
func TestTruncateDiffLineKeepsUTF8(t *testing.T) {
	line := strings.Repeat("中", 67) // 201 bytes > diffMaxLineWidth
	got := truncateDiffLine(line)
	if !utf8.ValidString(got) {
		t.Fatalf("truncateDiffLine split a rune: %q", got)
	}
	if !strings.HasSuffix(got, "…") {
		t.Fatalf("truncation marker missing: %q", got)
	}
}

func TestDiffTextsIdentical(t *testing.T) {
	if got := DiffTexts("same\n", "same\n", 40); got != "" {
		t.Fatalf("identical inputs should produce empty diff, got %q", got)
	}
}

func TestDiffTextsReplacement(t *testing.T) {
	oldText := "package a\n\nfunc old() {}\n"
	newText := "package a\n\nfunc new() {}\n"
	got := DiffTexts(oldText, newText, 40)
	if !strings.Contains(got, "- func old() {}") {
		t.Fatalf("missing removal:\n%s", got)
	}
	if !strings.Contains(got, "+ func new() {}") {
		t.Fatalf("missing addition:\n%s", got)
	}
	// One context line above the change.
	if !strings.Contains(got, "  package a") && !strings.Contains(got, "  \n") {
		t.Fatalf("missing context line:\n%s", got)
	}
}

func TestDiffTextsNewFile(t *testing.T) {
	got := DiffTexts("", "line1\nline2\n", 40)
	if got != "+ line1\n+ line2" {
		t.Fatalf("all-plus diff = %q", got)
	}
}

func TestDiffTextsCollapsesUnchangedRuns(t *testing.T) {
	oldText := "a\nb\nc\nd\ne\nf\ng\nh\n"
	newText := "A\nb\nc\nd\ne\nf\ng\nH\n"
	got := DiffTexts(oldText, newText, 40)
	if !strings.Contains(got, "...") {
		t.Fatalf("unchanged middle run should collapse into separator:\n%s", got)
	}
	if strings.Contains(got, "  d") {
		t.Fatalf("far-away context must not be shown:\n%s", got)
	}
}

func TestDiffForToolCall(t *testing.T) {
	tests := []struct {
		name     string
		tool     string
		args     string
		contains string
	}{
		{"edit diffs old and new", "edit", `{"path":"a.go","old_string":"foo()","new_string":"bar()"}`, "+ bar()"},
		{"edit removal side", "edit", `{"old_string":"foo()","new_string":"bar()"}`, "- foo()"},
		{"write shows new content", "write", `{"path":"b.go","content":"package b"}`, "+ package b"},
		{"read-only tool", "read_file", `{"path":"a.go"}`, ""},
		{"invalid json", "edit", `{`, ""},
		{"empty args", "edit", ``, ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := DiffForToolCall(tt.tool, json.RawMessage(tt.args), 40)
			if tt.contains == "" {
				if got != "" {
					t.Fatalf("DiffForToolCall(%s) = %q, want empty", tt.tool, got)
				}
				return
			}
			if !strings.Contains(got, tt.contains) {
				t.Fatalf("DiffForToolCall(%s) = %q, want substring %q", tt.tool, got, tt.contains)
			}
		})
	}
}

func TestDiffTextsBoundsOutput(t *testing.T) {
	var oldB, newB strings.Builder
	for i := 0; i < 100; i++ {
		oldB.WriteString("old\n")
		newB.WriteString("new\n")
	}
	got := DiffTexts(oldB.String(), newB.String(), 10)
	lines := strings.Split(got, "\n")
	if len(lines) > 11 { // 10 content lines + ellipsis
		t.Fatalf("diff lines = %d, want <= 11", len(lines))
	}
	if !strings.HasSuffix(got, "…") {
		t.Fatalf("truncated diff should end with ellipsis")
	}
}

// Unbounded mode (web frontend): the full diff must render — no line cap,
// no ellipsis, no input bound for the pure-addition (write) path.
func TestDiffTextsUnboundedNewFile(t *testing.T) {
	var newB strings.Builder
	for i := 0; i < diffMaxInputLines+50; i++ {
		newB.WriteString("line\n")
	}
	got := DiffTexts("", newB.String(), 0)
	lines := strings.Split(got, "\n")
	if len(lines) != diffMaxInputLines+50 {
		t.Fatalf("unbounded diff lines = %d, want %d", len(lines), diffMaxInputLines+50)
	}
	if strings.Contains(got, "…") {
		t.Fatalf("unbounded diff must not carry a truncation marker")
	}
}

// Unbounded mode keeps full-width lines (bounded mode cuts at
// diffMaxLineWidth for terminal display).
func TestDiffTextsUnboundedKeepsLongLines(t *testing.T) {
	long := strings.Repeat("x", diffMaxLineWidth+100)
	got := DiffTexts("a\n", "a\n"+long+"\n", 0)
	if !strings.Contains(got, long) {
		t.Fatalf("unbounded diff must keep full-width lines")
	}
}

// DiffForToolCall with ToolDiffUnbounded (the web event payloads) renders a
// write call's full content.
func TestDiffForToolCallUnboundedWrite(t *testing.T) {
	var content strings.Builder
	for i := 0; i < 60; i++ {
		content.WriteString("line\n")
	}
	args := json.RawMessage(`{"path":"x.txt","content":` + strconv.Quote(content.String()) + `}`)
	got := DiffForToolCall("write", args, 0)
	if n := strings.Count(got, "\n") + 1; n != 60 {
		t.Fatalf("unbounded write diff lines = %d, want 60", n)
	}
	if strings.Contains(got, "…") {
		t.Fatalf("unbounded write diff must not carry a truncation marker")
	}
}
