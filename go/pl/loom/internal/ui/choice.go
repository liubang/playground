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
// Created: 2026/07/27

package ui

import (
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ChoiceItem is one selectable row in a ChoiceList.
type ChoiceItem struct {
	Label string
	Desc  string
}

// ChoiceStyle customizes the row markers and hints of a ChoiceList. Empty
// fields fall back to the defaults documented per field.
type ChoiceStyle struct {
	// CursorPrefix marks the focused row (default "▶ "); ItemPrefix pads
	// the rest (default "  ").
	CursorPrefix string
	ItemPrefix   string
	// SelectedMark/UnselectedMark default to "[x]"/"[ ]" in multi mode
	// and "(•)"/"( )" in single mode.
	SelectedMark   string
	UnselectedMark string
	// OtherLabel names the free-text row (default "Other…").
	OtherLabel string
	// Footer overrides the hints line; empty selects the mode default.
	Footer string
	// EditingFooter overrides the hints line shown while the free-text row
	// is being edited (default "type your answer · Enter = confirm · Esc =
	// back to list").
	EditingFooter string
}

func (s ChoiceStyle) withDefaults(multi bool) ChoiceStyle {
	if s.CursorPrefix == "" {
		s.CursorPrefix = "▶ "
	}
	if s.ItemPrefix == "" {
		s.ItemPrefix = "  "
	}
	if s.SelectedMark == "" {
		if multi {
			s.SelectedMark = "[x]"
		} else {
			s.SelectedMark = "(•)"
		}
	}
	if s.UnselectedMark == "" {
		if multi {
			s.UnselectedMark = "[ ]"
		} else {
			s.UnselectedMark = "( )"
		}
	}
	if s.OtherLabel == "" {
		s.OtherLabel = "Other…"
	}
	if s.Footer == "" {
		if multi {
			s.Footer = "j/k or ↑/↓ = move · Space = toggle · Enter = confirm · Esc = skip"
		} else {
			s.Footer = "j/k or ↑/↓ = move · Enter = select · Esc = skip"
		}
	}
	if s.EditingFooter == "" {
		s.EditingFooter = "type your answer · Enter = confirm · Esc = back to list"
	}
	return s
}

// ChoiceListConfig configures a ChoiceList.
type ChoiceListConfig struct {
	Title string
	Items []ChoiceItem
	// Multi selects checkbox semantics (toggle + confirm); otherwise the
	// list is a radio group (confirm picks the focused row).
	Multi bool
	// OtherRow appends a free-text entry row after the items.
	OtherRow bool
	Style    ChoiceStyle
}

// maxChoiceTitleLines bounds the rendered title: the question text is
// model-generated and unbounded, the overlay is not.
const maxChoiceTitleLines = 4

// ChoiceList is a generic single/multi-select overlay with an optional
// free-text row. It is deliberately frontend-agnostic: the host routes keys
// to the mutation methods and renders the overlay with Render.
type ChoiceList struct {
	title    string
	items    []ChoiceItem
	multi    bool
	style    ChoiceStyle
	cursor   int
	selected []bool
	other    bool
	otherBuf strings.Builder
	// editing marks the free-text row's insert mode: while set, printable
	// keys (including j/k and space) are text input; while clear, they
	// keep their navigation meaning so the row is never a trap.
	editing bool
}

// NewChoiceList creates a ChoiceList.
func NewChoiceList(cfg ChoiceListConfig) *ChoiceList {
	return &ChoiceList{
		title:    cfg.Title,
		items:    cfg.Items,
		multi:    cfg.Multi,
		style:    cfg.Style.withDefaults(cfg.Multi),
		selected: make([]bool, len(cfg.Items)),
		other:    cfg.OtherRow,
	}
}

// rows is the total row count including the optional free-text row.
func (l *ChoiceList) rows() int {
	n := len(l.items)
	if l.other {
		n++
	}
	return n
}

// onOtherRow reports whether the cursor sits on the free-text row.
func (l *ChoiceList) onOtherRow() bool {
	return l.other && l.cursor == len(l.items)
}

// MoveUp moves the cursor up. Moving off the free-text row ends its
// insert mode (the text is preserved).
func (l *ChoiceList) MoveUp() {
	if l.cursor > 0 {
		l.cursor--
	}
	if !l.onOtherRow() {
		l.editing = false
	}
}

// MoveDown moves the cursor down. Moving off the free-text row ends its
// insert mode (the text is preserved).
func (l *ChoiceList) MoveDown() {
	if l.cursor < l.rows()-1 {
		l.cursor++
	}
	if !l.onOtherRow() {
		l.editing = false
	}
}

// BeginEdit enters the free-text row's insert mode.
func (l *ChoiceList) BeginEdit() {
	if l.onOtherRow() {
		l.editing = true
	}
}

// EndEdit leaves insert mode, keeping the typed text.
func (l *ChoiceList) EndEdit() {
	l.editing = false
}

// Editing reports whether the free-text row is in insert mode.
func (l *ChoiceList) Editing() bool {
	return l.editing
}

// Toggle flips the focused item. In single mode it acts as a radio button:
// the focused item becomes the only selection. The free-text row is not
// toggleable.
func (l *ChoiceList) Toggle() {
	if l.onOtherRow() || l.cursor >= len(l.items) {
		return
	}
	if l.multi {
		l.selected[l.cursor] = !l.selected[l.cursor]
		return
	}
	for i := range l.selected {
		l.selected[i] = i == l.cursor
	}
}

// TypeRune appends to the free-text row (no-op elsewhere).
func (l *ChoiceList) TypeRune(r rune) {
	if l.onOtherRow() {
		l.otherBuf.WriteRune(r)
	}
}

// Backspace deletes the last rune of the free-text row (no-op elsewhere).
func (l *ChoiceList) Backspace() {
	if !l.onOtherRow() {
		return
	}
	text := l.otherBuf.String()
	if text == "" {
		return
	}
	r := []rune(text)
	l.otherBuf.Reset()
	l.otherBuf.WriteString(string(r[:len(r)-1]))
}

// Confirm collects the answer. It reports ok=false when nothing was chosen:
// multi mode requires at least one toggled item or free text; single mode
// answers with the toggled item (or free text when on the other row).
func (l *ChoiceList) Confirm() (domain.QuestionAnswer, bool) {
	custom := strings.TrimSpace(l.otherBuf.String())
	if l.multi {
		answer := domain.QuestionAnswer{CustomText: custom}
		for i, on := range l.selected {
			if on {
				answer.Selected = append(answer.Selected, l.items[i].Label)
			}
		}
		if len(answer.Selected) == 0 && answer.CustomText == "" {
			return domain.QuestionAnswer{}, false
		}
		return answer, true
	}
	if l.onOtherRow() {
		if custom == "" {
			return domain.QuestionAnswer{}, false
		}
		return domain.QuestionAnswer{CustomText: custom}, true
	}
	// Single mode answers with the toggled item — the same one the radio
	// mark displays; the cursor alone never contradicts it.
	for i, on := range l.selected {
		if on {
			return domain.QuestionAnswer{Selected: []string{l.items[i].Label}}, true
		}
	}
	if l.cursor < len(l.items) {
		return domain.QuestionAnswer{Selected: []string{l.items[l.cursor].Label}}, true
	}
	return domain.QuestionAnswer{}, false
}

// Render renders the overlay as a string for viewport display, windowed
// around the cursor when the list exceeds the height budget.
func (l *ChoiceList) Render(width, height int) string {
	labelWidth := 0
	for _, item := range l.items {
		if len(item.Label) > labelWidth {
			labelWidth = len(item.Label)
		}
	}

	rows := make([]string, 0, l.rows())
	for i, item := range l.items {
		prefix := l.style.ItemPrefix
		if i == l.cursor {
			prefix = l.style.CursorPrefix
		}
		mark := l.style.UnselectedMark
		if l.selected[i] {
			mark = l.style.SelectedMark
		}
		row := fmt.Sprintf("%s%s %-*s", prefix, mark, labelWidth, item.Label)
		if item.Desc != "" {
			row += "  " + item.Desc
		}
		rows = append(rows, row)
	}
	if l.other {
		prefix := l.style.ItemPrefix
		if l.onOtherRow() {
			prefix = l.style.CursorPrefix
		}
		text := l.otherBuf.String()
		if text == "" {
			text = l.style.OtherLabel
		} else if l.editing {
			text += "▏"
		}
		rows = append(rows, fmt.Sprintf("%s> %s", prefix, text))
	}

	start, end := 0, len(rows)
	if height > 0 {
		visible := height - 4 // heading, blank line, scroll hints, footer
		if visible < 1 {
			visible = 1
		}
		if l.cursor >= visible {
			start = l.cursor - visible + 1
		}
		end = min(start+visible, len(rows))
	}

	var b strings.Builder
	if l.title != "" {
		// The title must respect the same width budget as the rows: an
		// overlong question would otherwise be soft-wrapped by the dialog
		// border and overflow the height the layout reserved for this
		// overlay. Cap it at a few lines, truncated per line.
		titleLines := strings.Split(l.title, "\n")
		if len(titleLines) > maxChoiceTitleLines {
			titleLines = append(titleLines[:maxChoiceTitleLines-1], "…")
		}
		for _, line := range titleLines {
			if width > 0 {
				line = truncateDisplayWidth(line, width)
			}
			b.WriteString(line)
			b.WriteString("\n")
		}
		b.WriteString("\n")
	}
	if start > 0 {
		b.WriteString("↑ more\n")
	}
	for i := start; i < end; i++ {
		line := rows[i]
		if width > 0 {
			line = truncateDisplayWidth(line, width)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}
	if end < len(rows) {
		b.WriteString("↓ more\n")
	}
	b.WriteString("\n")
	footer := l.style.Footer
	if l.editing {
		footer = l.style.EditingFooter
	}
	if width > 0 {
		footer = truncateDisplayWidth(footer, width)
	}
	b.WriteString(footer)
	return b.String()
}
