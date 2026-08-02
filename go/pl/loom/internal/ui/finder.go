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
	"fmt"
	"strings"

	"github.com/charmbracelet/lipgloss"
)

// FinderItem is one selectable record in a Finder: Text is filtered and
// shown as the primary row label, Hint is the dimmed secondary column,
// Badge is a right-aligned marker (e.g. "●" for the active entry).
type FinderItem[T any] struct {
	Value T
	Text  string
	Hint  string
	Badge string
}

// FinderStyles customizes Finder rendering. Zero-value styles render
// plain text (tests, NO_COLOR).
type FinderStyles struct {
	Cursor lipgloss.Style // focused row marker + text
	Hint   lipgloss.Style // secondary column
	Badge  lipgloss.Style // active-entry marker
	Input  lipgloss.Style // the query line
	Footer lipgloss.Style // hints line
}

// FinderConfig configures a Finder.
type FinderConfig[T any] struct {
	Title string
	Items []FinderItem[T]
	// Preview renders the side pane for the highlighted item; nil hides
	// the pane entirely.
	Preview func(T) string
	// CursorAt, when >= 0, places the cursor on the given items index
	// (e.g. the currently active entry).
	CursorAt int
	Styles   FinderStyles
}

// Finder is a snacks.picker-style selector: a fuzzy-filter input on top,
// a result list, and an optional preview pane. It is deliberately
// frontend-agnostic (same style as ChoiceList): the host translates keys
// into method calls and renders the component with Render.
//
// Navigation is modal (docs/VIM_UI_DESIGN.md §6.3): insert mode (the
// default) types into the filter; normal mode navigates the list with
// vim runes. The mode itself is tracked here; which keys switch modes is
// the host's decision.
type Finder[T any] struct {
	title    string
	items    []FinderItem[T]
	filtered []int // indices into items, best match first
	query    string
	cursor   int // index into filtered
	normal   bool
	preview  func(T) string
	styles   FinderStyles

	// Asynchronous sources (sessions) start unloaded; Load delivers
	// items or an error.
	loaded  bool
	loadErr error
}

// NewFinder creates a Finder with its cursor on CursorAt (clamped).
func NewFinder[T any](cfg FinderConfig[T]) *Finder[T] {
	f := &Finder[T]{
		title:    cfg.Title,
		items:    cfg.Items,
		preview:  cfg.Preview,
		styles:   cfg.Styles,
		loaded:   true,
		filtered: identityOrder(len(cfg.Items)),
	}
	if cfg.CursorAt > 0 && cfg.CursorAt < len(cfg.Items) {
		// Items start unfiltered, so the items index is the filtered index.
		f.cursor = cfg.CursorAt
	}
	return f
}

// NewLoadingFinder creates a Finder in the loading state; Load delivers
// the items once the asynchronous source resolves.
func NewLoadingFinder[T any](title string, preview func(T) string, styles FinderStyles) *Finder[T] {
	return &Finder[T]{title: title, preview: preview, styles: styles}
}

// Load delivers the asynchronously loaded items (or the fetch error).
func (f *Finder[T]) Load(items []FinderItem[T], err error) {
	if err != nil {
		f.loadErr = err
		f.loaded = true
		return
	}
	f.items = items
	f.loadErr = nil
	f.loaded = true
	f.refilter()
}

// --- mode ---

// Normal reports whether the list (not the filter input) has focus.
func (f *Finder[T]) Normal() bool { return f.normal }

// EnterNormal moves focus to the list (vim normal mode).
func (f *Finder[T]) EnterNormal() { f.normal = true }

// EnterInsert moves focus back to the filter input (vim insert mode).
func (f *Finder[T]) EnterInsert() { f.normal = false }

// --- filter input ---

// TypeRune appends to the filter query (no-op in normal mode).
func (f *Finder[T]) TypeRune(r rune) {
	if f.normal {
		return
	}
	f.query += string(r)
	f.refilter()
}

// Backspace deletes the last query rune (no-op in normal mode).
func (f *Finder[T]) Backspace() {
	if f.normal || f.query == "" {
		return
	}
	runes := []rune(f.query)
	f.query = string(runes[:len(runes)-1])
	f.refilter()
}

// Query returns the current filter text.
func (f *Finder[T]) Query() string { return f.query }

// refilter recomputes the filtered order and clamps the cursor.
func (f *Finder[T]) refilter() {
	if f.query == "" {
		f.filtered = identityOrder(len(f.items))
	} else {
		type scored struct {
			idx   int
			score int
		}
		var matches []scored
		for i, item := range f.items {
			if score, ok := fuzzyScore(f.query, item.Text); ok {
				matches = append(matches, scored{idx: i, score: score})
			}
		}
		// Best score first; ties keep the source order (selection sort on
		// a stable comparison so the result is deterministic).
		for i := range matches {
			for j := i + 1; j < len(matches); j++ {
				if matches[j].score > matches[i].score ||
					(matches[j].score == matches[i].score && matches[j].idx < matches[i].idx) {
					matches[i], matches[j] = matches[j], matches[i]
				}
			}
		}
		f.filtered = f.filtered[:0]
		for _, m := range matches {
			f.filtered = append(f.filtered, m.idx)
		}
	}
	if f.cursor >= len(f.filtered) {
		f.cursor = max(0, len(f.filtered)-1)
	}
}

func identityOrder(n int) []int {
	out := make([]int, n)
	for i := range out {
		out[i] = i
	}
	return out
}

// --- navigation ---

// MoveUp moves the cursor to the previous match (clamped).
func (f *Finder[T]) MoveUp() {
	if f.cursor > 0 {
		f.cursor--
	}
}

// MoveDown moves the cursor to the next match (clamped).
func (f *Finder[T]) MoveDown() {
	if f.cursor < len(f.filtered)-1 {
		f.cursor++
	}
}

// GotoTop moves the cursor to the first match (vim gg).
func (f *Finder[T]) GotoTop() { f.cursor = 0 }

// GotoBottom moves the cursor to the last match (vim G).
func (f *Finder[T]) GotoBottom() {
	if len(f.filtered) > 0 {
		f.cursor = len(f.filtered) - 1
	}
}

// PageUp moves the cursor up by one page (bodyHeight rows).
func (f *Finder[T]) PageUp(bodyHeight int) {
	f.cursor = max(0, f.cursor-max(bodyHeight, 1))
}

// PageDown moves the cursor down by one page (bodyHeight rows). The
// cursor never goes below zero, even when the filtered list is empty.
func (f *Finder[T]) PageDown(bodyHeight int) {
	f.cursor = max(0, min(len(f.filtered)-1, f.cursor+max(bodyHeight, 1)))
}

// Selected returns the highlighted item, or nil when the filtered list
// is empty.
func (f *Finder[T]) Selected() *T {
	if f.cursor < 0 || f.cursor >= len(f.filtered) {
		return nil
	}
	v := f.items[f.filtered[f.cursor]].Value
	return &v
}

// Len reports the number of items passing the filter.
func (f *Finder[T]) Len() int { return len(f.filtered) }

// --- rendering ---

// finderPreviewMinWidth is the inner width below which the preview pane
// is dropped and the list takes the whole row.
const finderPreviewMinWidth = 60

// Render renders the finder for the given inner width and height (the
// dialog border and padding are the host's concern).
func (f *Finder[T]) Render(width, height int) string {
	if !f.loaded {
		return "Loading..."
	}
	if f.loadErr != nil {
		return fmt.Sprintf("Error loading items: %v", f.loadErr)
	}
	if len(f.items) == 0 {
		return "No items found.\nPress Esc to go back."
	}

	// Layout: input, blank, body..., blank, footer.
	bodyHeight := height - 4
	if height <= 0 || bodyHeight < 1 {
		bodyHeight = max(len(f.filtered), 1)
	}

	var b strings.Builder
	b.WriteString(f.renderInputLine(width))
	b.WriteString("\n\n")

	if len(f.filtered) == 0 {
		b.WriteString(fmt.Sprintf("No matches for %q.", f.query))
		b.WriteString("\n\n")
		b.WriteString(f.renderFooter())
		return b.String()
	}

	rows := f.renderRows(width)
	body := f.windowRows(rows, bodyHeight)

	preview := f.renderPreview(width)
	if preview == nil {
		for i, row := range body {
			if i > 0 {
				b.WriteString("\n")
			}
			b.WriteString(row)
		}
	} else {
		b.WriteString(joinColumns(body, preview, finderListWidth(width)+2))
	}

	b.WriteString("\n\n")
	b.WriteString(f.renderFooter())
	return b.String()
}

// windowRows returns the visible slice of rows around the cursor,
// fitting scroll hints ("↑ more"/"↓ more") inside the height budget:
// each hint replaces one result row, so the total never exceeds height.
func (f *Finder[T]) windowRows(rows []string, height int) []string {
	window := func(visible int) (int, int) {
		if visible < 1 {
			visible = 1
		}
		start := 0
		if f.cursor >= visible {
			start = f.cursor - visible + 1
		}
		return start, min(start+visible, len(rows))
	}
	visible := max(height, 1)
	start, end := window(visible)
	// Reserve a row for each hint the tentative window would need, then
	// recompute. Undershooting the budget is safe; overshooting would
	// desync the inline renderer's line tracking.
	if start > 0 {
		visible--
	}
	if end < len(rows) {
		visible--
	}
	start, end = window(max(visible, 1))

	var out []string
	if start > 0 {
		out = append(out, f.styles.Hint.Render("↑ more"))
	}
	out = append(out, rows[start:end]...)
	if end < len(rows) {
		out = append(out, f.styles.Hint.Render("↓ more"))
	}
	return out
}

// renderInputLine renders the filter input with the match counter on the
// right: "❯ query▏                                12/42".
func (f *Finder[T]) renderInputLine(width int) string {
	query := f.query
	caret := "▏"
	if f.normal {
		caret = " " // no caret while the list owns the focus
	}
	counter := fmt.Sprintf("%d/%d", len(f.filtered), len(f.items))
	left := "❯ " + query + caret
	if width > 0 {
		left = truncateDisplayWidth(left, max(width-len(counter)-1, 1))
		gap := width - lipgloss.Width(left) - lipgloss.Width(counter)
		if gap > 0 {
			return f.styles.Input.Render(left) + strings.Repeat(" ", gap) + f.styles.Hint.Render(counter)
		}
	}
	return f.styles.Input.Render(left) + "  " + f.styles.Hint.Render(counter)
}

// finderListWidth computes the list pane width: half the row when the
// preview pane is visible, the full width otherwise.
func finderListWidth(width int) int {
	if width >= finderPreviewMinWidth {
		return (width - 2) / 2
	}
	return width
}

// renderRows renders the filtered items as row strings, aligned on the
// widest Text in the whole result set so columns stay put while
// scrolling.
func (f *Finder[T]) renderRows(width int) []string {
	listWidth := width
	if f.preview != nil {
		listWidth = finderListWidth(width)
	}
	textWidth := 0
	for _, idx := range f.filtered {
		if n := lipgloss.Width(f.items[idx].Text); n > textWidth {
			textWidth = n
		}
	}
	rows := make([]string, len(f.filtered))
	for i, idx := range f.filtered {
		item := f.items[idx]
		marker := "  "
		textStyle := lipgloss.NewStyle()
		if i == f.cursor {
			marker = "▶ "
			textStyle = f.styles.Cursor
		}
		row := marker + textStyle.Render(fmt.Sprintf("%-*s", textWidth, item.Text))
		// The badge rides next to the item text so a long hint can never
		// truncate it away.
		if item.Badge != "" {
			row += " " + f.styles.Badge.Render(item.Badge)
		}
		if item.Hint != "" {
			row += "  " + f.styles.Hint.Render(item.Hint)
		}
		rows[i] = truncateANSI(row, listWidth)
	}
	return rows
}

// renderPreview renders the preview pane lines for the highlighted item,
// or nil when the pane is hidden or there is nothing to show.
func (f *Finder[T]) renderPreview(width int) []string {
	if f.preview == nil || width < finderPreviewMinWidth {
		return nil
	}
	sel := f.Selected()
	if sel == nil {
		return nil
	}
	paneWidth := width - finderListWidth(width) - 2
	var lines []string
	for _, line := range strings.Split(strings.TrimRight(f.preview(*sel), "\n"), "\n") {
		lines = append(lines, f.styles.Hint.Render(truncateDisplayWidth(line, paneWidth)))
	}
	return lines
}

// joinColumns places list rows and preview lines side by side: every
// list row is padded to colWidth display cells, then the preview line of
// the same index is appended.
func joinColumns(list, preview []string, colWidth int) string {
	n := max(len(list), len(preview))
	var b strings.Builder
	for i := 0; i < n; i++ {
		if i > 0 {
			b.WriteString("\n")
		}
		var row string
		if i < len(list) {
			row = list[i]
		}
		if i < len(preview) {
			row += strings.Repeat(" ", max(colWidth-lipgloss.Width(row), 0)) + preview[i]
		}
		b.WriteString(row)
	}
	return b.String()
}

// renderFooter renders the mode-aware hints line.
func (f *Finder[T]) renderFooter() string {
	var hints string
	if f.normal {
		hints = "NORMAL · j/k = move · g/G = top/bottom · i = filter · Enter = select · q/Esc = close"
	} else {
		hints = "INSERT · type to filter · ↑/↓ or ctrl+j/k = move · Enter = select · Esc = normal mode"
	}
	return f.styles.Footer.Render(hints)
}

// --- fuzzy matching ---

// fuzzyScore scores text against query with a simplified fzf algorithm
// (docs/VIM_UI_DESIGN.md §6.2): every query rune must appear in order
// (subsequence, case-insensitive); consecutive runs, word-boundary hits
// (start of string or after space, '/', '-', '_') and early first hits
// raise the score. ok=false means no subsequence match.
func fuzzyScore(query, text string) (score int, ok bool) {
	q := []rune(strings.ToLower(query))
	t := []rune(strings.ToLower(text))
	if len(q) == 0 {
		return 0, true
	}
	ti := 0
	firstHit := -1
	prevHit := -2
	for _, qr := range q {
		hit := -1
		for ti < len(t) {
			if t[ti] == qr {
				hit = ti
				ti++
				break
			}
			ti++
		}
		if hit < 0 {
			return 0, false
		}
		if firstHit < 0 {
			firstHit = hit
		}
		score += 1
		if hit == prevHit+1 {
			score += 4 // consecutive run
		}
		if hit == 0 || isWordBoundary(t[hit-1]) {
			score += 3 // word boundary
		}
		prevHit = hit
	}
	// Earlier first hits rank higher, bounded so run/boundary bonuses
	// dominate the tie-break.
	score += max(0, 8-firstHit)
	return score, true
}

func isWordBoundary(r rune) bool {
	switch r {
	case ' ', '/', '-', '_', '.', ':':
		return true
	}
	return false
}

// truncateANSI shortens a styled string to at most width display cells.
// Unlike truncateDisplayWidth (which assumes plain text), it walks ANSI
// escape sequences through and cuts on display width, always appending a
// reset so styles never leak past the cut.
func truncateANSI(s string, width int) string {
	if width <= 0 || lipgloss.Width(s) <= width {
		return s
	}
	var b strings.Builder
	used := 0
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
		w := lipgloss.Width(string(r))
		if used+w > width {
			break
		}
		b.WriteRune(r)
		used += w
	}
	b.WriteString("\x1b[0m")
	return b.String()
}
