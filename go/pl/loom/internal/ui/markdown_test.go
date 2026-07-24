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
	"regexp"
	"strings"
	"testing"
)

// markdownFixture keeps blank lines around the rule so goldmark parses it as
// a real <hr> instead of a setext heading underline.
const markdownFixture = "## Ad Hoc\n\n**bold** and `code`\n\n---\n\n> quoted\n\n```\nx := 1\n```"

// ansiSGR strips SGR escape sequences: glamour emits per-token styling, so
// adjacent words in the same phrase arrive separated by ANSI codes and
// content assertions must run on the plain text.
var ansiSGR = regexp.MustCompile("\x1b\\[[0-9;]*m")

func stripANSI(s string) string {
	return ansiSGR.ReplaceAllString(s, "")
}

func TestMarkdownRenderingColored(t *testing.T) {
	m := Model{theme: DefaultTheme(), width: 80}
	block := &TranscriptBlock{Kind: BlockKindAssistant, Done: true, Content: markdownFixture}
	view := stripANSI(m.renderBlock(block))

	for _, want := range []string{"Ad Hoc", "bold", "code", "quoted", "x := 1", "--------"} {
		if !strings.Contains(view, want) {
			t.Fatalf("rendered view missing %q:\n%s", want, view)
		}
	}
	// Styled spans replace the raw emphasis markers entirely.
	for _, unwanted := range []string{"**bold**", "`code`"} {
		if strings.Contains(view, unwanted) {
			t.Fatalf("markdown marker %q left in rendered view:\n%s", unwanted, view)
		}
	}
}

func TestMarkdownRenderingNoColor(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 80}
	block := &TranscriptBlock{Kind: BlockKindAssistant, Done: true, Content: markdownFixture}
	view := m.renderBlock(block)

	for _, want := range []string{"Ad Hoc", "bold", "code", "quoted", "x := 1", "--------"} {
		if !strings.Contains(view, want) {
			t.Fatalf("notty rendered view missing %q:\n%s", want, view)
		}
	}
	// NO_COLOR must stay strictly ANSI-free.
	if strings.ContainsRune(view, '\x1b') {
		t.Fatalf("notty rendered view contains ANSI escape:\n%q", view)
	}
}

func TestMarkdownRenderingKeepsReasoningStylingIntact(t *testing.T) {
	m := Model{theme: DefaultTheme(), width: 80}
	block := &TranscriptBlock{
		Kind:            BlockKindAssistant,
		Done:            true,
		Content:         "**bold**",
		StreamReasoning: "some thought",
	}
	view := m.renderBlock(block)
	stripped := stripANSI(view)

	// The reasoning notice is pre-styled with lipgloss; if it were fed
	// through glamour, its ANSI sequences would leak as visible text.
	if strings.Contains(stripped, "[0m") || strings.Contains(stripped, "38;2;") {
		t.Fatalf("styled reasoning leaked ANSI as visible text:\n%q", view)
	}
	if !strings.Contains(stripped, "Thought process hidden") {
		t.Fatalf("reasoning notice missing:\n%q", view)
	}
	if !strings.Contains(stripped, "bold") {
		t.Fatalf("assistant content missing:\n%q", view)
	}
	// The notice and the markdown body must stay on separate lines (lipgloss
	// pads lines to a uniform block width, hence the trimmed comparison).
	lines := strings.Split(stripped, "\n")
	if !strings.Contains(lines[0], "Thought process hidden") {
		t.Fatalf("first line should be the reasoning notice:\n%q", view)
	}
	if !strings.Contains(strings.Join(lines[1:], "\n"), "bold") {
		t.Fatalf("assistant content should follow the reasoning line:\n%q", view)
	}
}

func TestMarkdownRendersTablesAndLists(t *testing.T) {
	m := Model{theme: DefaultTheme(), width: 80}
	content := "| Name | Value |\n|---|---|\n| a | 1 |\n\n- one\n- two"
	block := &TranscriptBlock{Kind: BlockKindAssistant, Done: true, Content: content}
	view := stripANSI(m.renderBlock(block))

	for _, want := range []string{"Name", "Value", "│", "─", "•", "one", "two"} {
		if !strings.Contains(view, want) {
			t.Fatalf("rendered view missing %q:\n%s", want, view)
		}
	}
}

func TestMarkdownRendererCachesPerContentWidthAndStyle(t *testing.T) {
	r := newMarkdownRenderer()
	content := "## Title\n\nsome text"

	first := r.render(content, 72, "dark")
	second := r.render(content, 72, "dark")
	if first != second {
		t.Fatal("same input rendered differently")
	}
	if got := len(r.cache); got != 1 {
		t.Fatalf("cache size = %d, want 1", got)
	}
	if got := len(r.renderers); got != 1 {
		t.Fatalf("renderer count = %d, want 1", got)
	}

	// Width and style variants get isolated cache entries.
	r.render(content, 40, "dark")
	r.render(content, 72, "notty")
	if got := len(r.cache); got != 3 {
		t.Fatalf("cache size = %d, want 3", got)
	}
	if got := len(r.renderers); got != 3 {
		t.Fatalf("renderer count = %d, want 3", got)
	}
}

func TestMarkdownRendererWrapsToWidth(t *testing.T) {
	r := newMarkdownRenderer()
	long := strings.Repeat("word ", 30)
	narrow := r.render(long, 40, "dark")
	wide := r.render(long, 72, "dark")
	narrowLines := strings.Count(narrow, "\n") + 1
	wideLines := strings.Count(wide, "\n") + 1
	if narrowLines <= wideLines {
		t.Fatalf("width 40 should wrap into more lines than width 72: %d vs %d", narrowLines, wideLines)
	}
}

func TestMarkdownRendererHandlesEmptyAndPlainText(t *testing.T) {
	r := newMarkdownRenderer()
	if got := r.render("", 72, "dark"); got != "" {
		t.Fatalf("empty content rendered as %q", got)
	}
	if got := stripANSI(r.render("plain text", 72, "dark")); !strings.Contains(got, "plain text") {
		t.Fatalf("plain text rendered as %q", got)
	}
}
