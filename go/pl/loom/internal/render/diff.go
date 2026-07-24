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
	"strings"
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

// Bounds for diff rendering: inputs are capped before the O(n·m) LCS, and the
// rendered output is capped per line and in total.
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
// terminal display. Changed regions keep one line of context on each side;
// unchanged runs collapse into a "..." separator. The output is bounded to
// maxLines lines; truncation is marked with a trailing "…" line. Identical
// inputs produce an empty string.
func DiffTexts(oldText, newText string, maxLines int) string {
	if oldText == newText {
		return ""
	}
	oldLines := capLines(splitDiffLines(oldText), diffMaxInputLines)
	newLines := capLines(splitDiffLines(newText), diffMaxInputLines)
	ops := lcsDiff(oldLines, newLines)

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
		out = append(out, prefix+truncateDiffLine(op.line))
		if maxLines > 0 && len(out) >= maxLines {
			out = append(out, "…")
			break
		}
	}
	return strings.Join(out, "\n")
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
	if len(line) > diffMaxLineWidth {
		return line[:diffMaxLineWidth] + "…"
	}
	return line
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
