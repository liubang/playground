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
)

// mouseWheelDelta is the number of lines one wheel notch scrolls
// (matches the bubbles/viewport default).
const mouseWheelDelta = 3

// lineView is a minimal scrollable window over a pre-split line slice.
// It replaces bubbles/viewport with identical scroll semantics
// (SetContent/SetYOffset/LineUp/LineDown/GotoBottom/AtBottom clamping)
// but a cheap View: bubbles re-renders every visible line through
// lipgloss's Width/Height/MaxWidth reflow machinery on *every frame*,
// which is O(visible lines × cell processing) at up to 60fps. lineView
// splits content once per rebuild (syncTranscript) and View only slices,
// truncates, and pads.
type lineView struct {
	lines   []string
	YOffset int
	Width   int
	Height  int
}

// SetContent replaces the content, normalizing line endings. The scroll
// offset is preserved unless it now points past the end (matching
// bubbles' SetContent).
func (v *lineView) SetContent(s string) {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	v.lines = strings.Split(s, "\n")
	if v.YOffset > len(v.lines)-1 {
		v.GotoBottom()
	}
}

// SetLines replaces the content with a pre-split line slice, preserving
// the scroll offset like SetContent. The incremental transcript sync
// splices line slices directly and hands them over here — no join, no
// re-split.
func (v *lineView) SetLines(lines []string) {
	v.lines = lines
	if v.YOffset > len(v.lines)-1 {
		v.GotoBottom()
	}
}

// TotalLineCount returns the number of content lines.
func (v lineView) TotalLineCount() int { return len(v.lines) }

// maxYOffset is the largest legal scroll offset: the last line at the
// bottom of the window, or 0 when the content fits entirely.
func (v lineView) maxYOffset() int { return max(0, len(v.lines)-v.Height) }

// AtBottom reports whether the view is pinned to the content tail.
func (v lineView) AtBottom() bool { return v.YOffset >= v.maxYOffset() }

// SetYOffset sets the scroll offset, clamped to the legal range.
func (v *lineView) SetYOffset(n int) { v.YOffset = min(max(n, 0), v.maxYOffset()) }

// LineUp scrolls n lines toward the top (clamped).
func (v *lineView) LineUp(n int) { v.SetYOffset(v.YOffset - n) }

// LineDown scrolls n lines toward the bottom (clamped).
func (v *lineView) LineDown(n int) { v.SetYOffset(v.YOffset + n) }

// GotoBottom pins the view to the content tail.
func (v *lineView) GotoBottom() { v.YOffset = v.maxYOffset() }

// View renders the visible window: the Height lines starting at YOffset,
// each truncated to the view width (a long line must never soft-wrap and
// desync the inline renderer), padded with blanks to exactly Height
// lines so the frame height is stable regardless of content length.
func (v lineView) View() string {
	if v.Height <= 0 || len(v.lines) == 0 {
		return ""
	}
	top := min(max(v.YOffset, 0), v.maxYOffset())
	bottom := min(top+v.Height, len(v.lines))
	out := make([]string, 0, v.Height)
	for _, line := range v.lines[top:bottom] {
		out = append(out, truncateANSI(line, v.Width))
	}
	for len(out) < v.Height {
		out = append(out, "")
	}
	return strings.Join(out, "\n")
}
