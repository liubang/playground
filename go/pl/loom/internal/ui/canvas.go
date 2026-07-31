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

	"github.com/charmbracelet/lipgloss"
)

// canvas.go is the layer compositor for floating windows
// (docs/VIM_UI_DESIGN.md §7.1). Bubble Tea's render model is "one string
// per frame", so a float is pure string geometry: the base frame is
// rendered first, then each float's lines are spliced over the rows it
// covers. The compositor only cuts and pastes already-rendered strings —
// it never calls block or transcript renderers, so covering content with
// a float cannot invalidate any render cache.
//
// Only safe under the alt-screen renderer (full-frame repaints); the
// inline renderer's line tracking cannot tolerate overlays.
//
// Known limitation: background lines are assumed self-contained (every
// line's styling ends with a reset — true for all lipgloss/glamour
// output in this codebase). A style spanning a whole background line
// (e.g. the header's full-width fill) is not re-established on the right
// side of a float covering it.

// Float is one floating layer: Content (already rendered, border
// included) placed with its top-left corner at (X, Y) in display cells.
type Float struct {
	Content string
	X, Y    int
}

// centeredFloat places content in the middle of the screen, clamped to
// the top-left corner when it exceeds the screen in either dimension
// (overflow rows are dropped during composition).
func centeredFloat(content string, screenW, screenH int) Float {
	w, h := 0, 0
	for line := range strings.Lines(content) {
		if n := lipgloss.Width(line); n > w {
			w = n
		}
		h++
	}
	return Float{
		Content: content,
		X:       max((screenW-w)/2, 0),
		Y:       max((screenH-h)/2, 0),
	}
}

// ComposeFloats splices floats over the base frame. The frame is first
// normalized to exactly height lines (padded with blanks, truncated at
// the bottom) so the result always fits the terminal — a hard
// requirement for both renderers. Float lines that fall outside the
// screen are skipped; lines wider than the space left of the screen edge
// are cut.
func ComposeFloats(frame string, width, height int, floats ...Float) string {
	if height <= 0 {
		return frame
	}
	lines := strings.Split(frame, "\n")
	for len(lines) < height {
		lines = append(lines, "")
	}
	if len(lines) > height {
		lines = lines[:height]
	}
	for _, f := range floats {
		x := max(f.X, 0)
		for i, fl := range strings.Split(f.Content, "\n") {
			row := f.Y + i
			if row < 0 || row >= len(lines) {
				continue
			}
			budget := width - x
			if budget <= 0 {
				continue
			}
			fl = truncateANSI(fl, budget)
			w := lipgloss.Width(fl)
			// Left of the float (padded when the background line runs
			// short), the float itself, a reset so the float's styling
			// never bleeds, then the covered remainder of the line.
			left := truncateANSI(lines[row], x)
			if lw := lipgloss.Width(left); lw < x {
				left += strings.Repeat(" ", x-lw)
			}
			lines[row] = left + fl + "\x1b[0m" + dropANSI(lines[row], x+w)
		}
	}
	return strings.Join(lines, "\n")
}

// dropANSI removes the first width display cells of s, keeping every
// ANSI escape sequence it passes (they are replayed so the remainder's
// mid-line styling stays correct) and appending a reset at the cut so a
// style opened in the dropped prefix cannot bleed into the remainder.
// It is the mirror of truncateANSI.
func dropANSI(s string, width int) string {
	if width <= 0 {
		return s
	}
	var b strings.Builder
	b.WriteString("\x1b[0m")
	skipped := 0
	inEsc := false
	for _, r := range s {
		if inEsc {
			b.WriteRune(r)
			if r == 'm' {
				inEsc = false
			}
			continue
		}
		if r == '\x1b' {
			inEsc = true
			b.WriteRune(r)
			continue
		}
		if skipped < width {
			skipped += lipgloss.Width(string(r))
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}
