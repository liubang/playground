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

func TestSanitizeSlug(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"fix-bazel-build", "fix-bazel-build"},
		{"Fix Bazel Build", "fix-bazel-build"},
		{"../../evil", "evil"},
		{"a---b", "a-b"},
		{"a--b--c", "a-b-c"},
		{"UPPERCASE", "uppercase"},
		{"hello world 123", "hello-world-123"},
		{"-leading-dash-", "leading-dash"},
		{strings.Repeat("a", 100), strings.Repeat("a", 80)},
	}
	for _, tt := range tests {
		t.Run(tt.input, func(t *testing.T) {
			got := sanitizeSlug(tt.input)
			if got != tt.want {
				t.Errorf("sanitizeSlug(%q) = %q, want %q", tt.input, got, tt.want)
			}
		})
	}
}

func TestCapTranscriptNoTruncation(t *testing.T) {
	s := "short content"
	got := capTranscript(s, 1000)
	if got != s {
		t.Errorf("capTranscript of short content = %q, want %q", got, s)
	}
}

func TestCapTranscriptTruncation(t *testing.T) {
	s := "first line\nsecond line\nthird line\nfourth line"
	got := capTranscript(s, 30)
	if !strings.HasPrefix(got, "[... earlier transcript truncated ...]") {
		t.Errorf("capTranscript missing head marker: %q", got)
	}
	// The tail should include the later lines.
	if !strings.Contains(got, "fourth line") {
		t.Errorf("capTranscript lost tail content: %q", got)
	}
}

func TestCapTranscriptUTF8Safe(t *testing.T) {
	// Chinese characters are multi-byte; ensure we don't produce garbled output.
	s := strings.Repeat("你好世界", 1000) // 4 chars * 3 bytes each = 12 bytes per repeat = 12000 bytes
	got := capTranscript(s, 500)
	// Should not contain garbled bytes — just check it doesn't panic and
	// the head marker is present.
	if !strings.HasPrefix(got, "[... earlier transcript truncated ...]") {
		t.Errorf("capTranscript missing head marker for UTF-8 content")
	}
	// Verify all runes in the tail are valid.
	tail := strings.TrimPrefix(got, "[... earlier transcript truncated ...]\n")
	for _, r := range tail {
		if r == 0xFFFD {
			t.Error("capTranscript produced invalid UTF-8 replacement character")
			break
		}
	}
}

func TestExtractJSONFromCodeBlock(t *testing.T) {
	input := "```json\n{\"key\": \"value\"}\n```"
	got := extractJSON(input)
	want := `{"key": "value"}`
	if got != want {
		t.Errorf("extractJSON(%q) = %q, want %q", input, got, want)
	}
}

func TestExtractJSONRaw(t *testing.T) {
	input := `Here is the result: {"key": "value"} and some text`
	got := extractJSON(input)
	want := `{"key": "value"}`
	if got != want {
		t.Errorf("extractJSON(%q) = %q, want %q", input, got, want)
	}
}

func TestExtractJSONGenericCodeBlock(t *testing.T) {
	input := "```\n{\"key\": \"value\"}\n```"
	got := extractJSON(input)
	want := `{"key": "value"}`
	if got != want {
		t.Errorf("extractJSON(%q) = %q, want %q", input, got, want)
	}
}

func TestBuildStageOneInput(t *testing.T) {
	got := buildStageOneInput("sess-123", "/workspace", "transcript content")
	if !strings.Contains(got, "sess-123") {
		t.Error("buildStageOneInput missing session ID")
	}
	if !strings.Contains(got, "/workspace") {
		t.Error("buildStageOneInput missing workspace")
	}
	if !strings.Contains(got, "transcript content") {
		t.Error("buildStageOneInput missing transcript")
	}
}
