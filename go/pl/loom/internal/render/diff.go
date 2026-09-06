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
	"fmt"
	"strings"
	"unicode/utf8"
)

// DiffForToolCall renders a compact diff for file-editing tool calls from
// their raw JSON arguments: "edit" diffs old_string against new_string,
// "write" shows the new content as an all-addition diff. Other tools have
// no meaningful argument diff and produce an empty string.
func DiffForToolCall(toolName string, args json.RawMessage, maxLines int) string {
	if len(args) == 0 {
		return ""
	}
	switch toolName {
	case "edit":
		var parsed struct {
			OldString string `json:"old_string"`
			NewString string `json:"new_string"`
		}
		if err := json.Unmarshal(args, &parsed); err != nil {
			return ""
		}
		return DiffTexts(parsed.OldString, parsed.NewString, maxLines)
	case "write":
		var parsed struct {
			Content string `json:"content"`
		}
		if err := json.Unmarshal(args, &parsed); err != nil {
			return ""
		}
		return DiffTexts("", parsed.Content, maxLines)
	}
	return ""
}

// Bounds for diff rendering: bounded outputs cap the O(n·m) LCS inputs and
// the per-line width. Unbounded output (maxLines <= 0) skips both caps.
const (
	diffMaxInputLines = 400
	diffMaxLineWidth  = 200
)

// diffOp is one row of a line-level diff: ' ' context, '-' removal, '+' addition.
type diffOp struct {
	kind byte
	line string
}

// DiffTexts renders a compact line diff between oldText and newText for
// display. Changed regions keep one line of context on each side; unchanged
// runs collapse into a "..." separator. A positive maxLines bounds the output
// (truncation marked with a trailing "…" line) and caps per-line width;
// maxLines <= 0 renders the full diff with untruncated lines. Identical
// inputs produce an empty string.
func DiffTexts(oldText, newText string, maxLines int) string {
	if oldText == newText {
		return ""
	}
	bounded := maxLines > 0
	ops := computeDiffOps(oldText, newText)

	// Keep changed rows plus one context row around each change.
	show := make([]bool, len(ops))
	for i, op := range ops {
		if op.kind == ' ' {
			continue
		}
		show[i] = true
		if i > 0 {
			show[i-1] = true
		}
		if i+1 < len(ops) {
			show[i+1] = true
		}
	}

	var out []string
	skipped := false
	for i, op := range ops {
		if !show[i] {
			skipped = true
			continue
		}
		if skipped && len(out) > 0 {
			out = append(out, "...")
		}
		skipped = false
		prefix := "  "
		switch op.kind {
		case '-':
			prefix = "- "
		case '+':
			prefix = "+ "
		}
		line := op.line
		if bounded {
			line = truncateDiffLine(line)
		}
		out = append(out, prefix+line)
		if bounded && len(out) >= maxLines {
			out = append(out, "…")
			break
		}
	}
	return strings.Join(out, "\n")
}

// computeDiffOps runs the shared capped-LCS pipeline behind DiffTexts:
// empty-side inputs take the trivial pure add/remove path, two-sided
// inputs are capped per side before the O(n·m) dynamic program.
func computeDiffOps(oldText, newText string) []diffOp {
	if oldText == "" || newText == "" {
		// Pure addition/removal (e.g. write creating a file): the diff is
		// trivial and needs neither the LCS nor its input bound.
		return trivialDiff(splitDiffLines(oldText), splitDiffLines(newText))
	}
	oldLines := capLines(splitDiffLines(oldText), diffMaxInputLines)
	newLines := capLines(splitDiffLines(newText), diffMaxInputLines)
	return lcsDiff(oldLines, newLines)
}

// CountLineDiff is the counting sibling of DiffTexts: same capped-LCS
// pipeline, returning addition/removal counts instead of rendered text.
// Counts are exact within the per-side input cap (diffMaxInputLines).
func CountLineDiff(oldText, newText string) (added, removed int) {
	for _, op := range computeDiffOps(oldText, newText) {
		switch op.kind {
		case '+':
			added++
		case '-':
			removed++
		}
	}
	return added, removed
}

// DiffInputCapped reports whether either side exceeds the LCS input cap —
// rendered diffs and counts past this point describe only the leading
// portion of the text.
func DiffInputCapped(oldText, newText string) bool {
	oldLines := 0
	if oldText != "" {
		oldLines = strings.Count(oldText, "\n") + 1
	}
	newLines := 0
	if newText != "" {
		newLines = strings.Count(newText, "\n") + 1
	}
	return oldLines > diffMaxInputLines || newLines > diffMaxInputLines
}

// UnifiedTexts renders old→new as unified-diff hunks:
// "@@ -oldStart,oldCount +newStart,newCount @@" headers with ctxLines of
// context around each change run (runs within 2·ctxLines merge into one
// hunk). Zero-count sides follow the unified convention (a created file
// starts at "@@ -0,0 +1,N @@"). maxLines caps emitted lines including
// headers; truncation appends a trailing "…" line. Identical inputs
// produce "". Unlike DiffTexts this is for REAL reviews (turn-change
// summaries): line numbers make hunks clickable in review tooling.
func UnifiedTexts(oldText, newText string, ctxLines, maxLines int) string {
	if oldText == newText {
		return ""
	}
	if ctxLines < 0 {
		ctxLines = 0
	}
	ops := computeDiffOps(oldText, newText)

	// Annotate each op with its 1-based line number on BOTH sides (0 when
	// the op does not consume that side).
	type numberedOp struct {
		diffOp
		oldLine, newLine int
	}
	nops := make([]numberedOp, len(ops))
	o, n := 1, 1
	for k, op := range ops {
		switch op.kind {
		case ' ':
			nops[k] = numberedOp{op, o, n}
			o++
			n++
		case '-':
			nops[k] = numberedOp{op, o, 0}
			o++
		case '+':
			nops[k] = numberedOp{op, 0, n}
			n++
		}
	}

	// show[]: change ops plus ctxLines of context on each side.
	show := make([]bool, len(nops))
	for k, op := range nops {
		if op.kind == ' ' {
			continue
		}
		for d := -ctxLines; d <= ctxLines; d++ {
			if k+d >= 0 && k+d < len(show) {
				show[k+d] = true
			}
		}
	}

	bounded := maxLines > 0
	out := make([]string, 0, len(ops))
	truncated := false
emit:
	for k := 0; k < len(nops); {
		if !show[k] {
			k++
			continue
		}
		// hunk: maximal run of shown ops
		s := k
		for k < len(nops) && show[k] {
			k++
		}
		e := k

		oldStart, newStart, oldCount, newCount := 0, 0, 0, 0
		for _, op := range nops[s:e] {
			switch op.kind {
			case ' ':
				if oldStart == 0 {
					oldStart = op.oldLine
				}
				if newStart == 0 {
					newStart = op.newLine
				}
				oldCount++
				newCount++
			case '-':
				if oldStart == 0 {
					oldStart = op.oldLine
				}
				oldCount++
			case '+':
				if newStart == 0 {
					newStart = op.newLine
				}
				newCount++
			}
		}
		if oldCount == 0 {
			oldStart = max(0, oldStart-1)
		}
		if newCount == 0 {
			newStart = max(0, newStart-1)
		}
		out = append(out, fmt.Sprintf("@@ -%d,%d +%d,%d @@", oldStart, oldCount, newStart, newCount))
		if bounded && len(out) >= maxLines {
			truncated = true
			break emit
		}
		for _, op := range nops[s:e] {
			var line string
			switch op.kind {
			case ' ':
				line = " " + op.line
			case '-':
				line = "-" + op.line
			case '+':
				line = "+" + op.line
			}
			if bounded {
				line = truncateDiffLine(line)
			}
			out = append(out, line)
			if bounded && len(out) >= maxLines {
				truncated = true
				break emit
			}
		}
	}
	if truncated {
		out = append(out, "…")
	}
	return strings.Join(out, "\n")
}

// trivialDiff diffs two texts where at least one side is empty: every line
// is a removal or an addition, so no LCS computation is required.
func trivialDiff(oldLines, newLines []string) []diffOp {
	ops := make([]diffOp, 0, len(oldLines)+len(newLines))
	for _, l := range oldLines {
		ops = append(ops, diffOp{'-', l})
	}
	for _, l := range newLines {
		ops = append(ops, diffOp{'+', l})
	}
	return ops
}

// splitDiffLines splits text into lines without keeping a trailing empty row
// after a final newline.
func splitDiffLines(text string) []string {
	if text == "" {
		return nil
	}
	lines := strings.Split(strings.TrimSuffix(text, "\n"), "\n")
	return lines
}

func capLines(lines []string, max int) []string {
	if len(lines) > max {
		return lines[:max]
	}
	return lines
}

func truncateDiffLine(line string) string {
	if len(line) <= diffMaxLineWidth {
		return line
	}
	// Back off to a rune boundary: a byte cut can split a multi-byte
	// UTF-8 character.
	cut := diffMaxLineWidth
	for cut > 0 && !utf8.ValidString(line[:cut]) {
		cut--
	}
	return line[:cut] + "…"
}

// lcsDiff computes the line-level diff via a longest-common-subsequence
// dynamic program. Inputs are expected to be pre-bounded (see capLines).
func lcsDiff(oldLines, newLines []string) []diffOp {
	n, m := len(oldLines), len(newLines)
	// dp[i][j] = LCS length of oldLines[i:], newLines[j:].
	dp := make([][]int, n+1)
	for i := range dp {
		dp[i] = make([]int, m+1)
	}
	for i := n - 1; i >= 0; i-- {
		for j := m - 1; j >= 0; j-- {
			if oldLines[i] == newLines[j] {
				dp[i][j] = dp[i+1][j+1] + 1
			} else if dp[i+1][j] >= dp[i][j+1] {
				dp[i][j] = dp[i+1][j]
			} else {
				dp[i][j] = dp[i][j+1]
			}
		}
	}

	var ops []diffOp
	i, j := 0, 0
	for i < n && j < m {
		switch {
		case oldLines[i] == newLines[j]:
			ops = append(ops, diffOp{' ', oldLines[i]})
			i++
			j++
		case dp[i+1][j] >= dp[i][j+1]:
			ops = append(ops, diffOp{'-', oldLines[i]})
			i++
		default:
			ops = append(ops, diffOp{'+', newLines[j]})
			j++
		}
	}
	for ; i < n; i++ {
		ops = append(ops, diffOp{'-', oldLines[i]})
	}
	for ; j < m; j++ {
		ops = append(ops, diffOp{'+', newLines[j]})
	}
	return ops
}
