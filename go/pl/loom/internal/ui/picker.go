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
// Created: 2026/07/23

package ui

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// SessionPicker manages the state for picking a session to resume.
type SessionPicker struct {
	Summaries []app.SessionSummary
	Cursor    int
	Loaded    bool
	Error     error
}

// NewSessionPicker creates a new session picker.
func NewSessionPicker() *SessionPicker {
	return &SessionPicker{
		Cursor: 0,
		Loaded: false,
	}
}

// Load loads the session summaries from the store.
func (p *SessionPicker) Load(summaries []app.SessionSummary, err error) {
	if err != nil {
		p.Error = err
		p.Loaded = true
		return
	}
	p.Summaries = summaries
	p.Loaded = true
}

// MoveUp moves the cursor up.
func (p *SessionPicker) MoveUp() {
	if p.Cursor > 0 {
		p.Cursor--
	}
}

// MoveDown moves the cursor down.
func (p *SessionPicker) MoveDown() {
	if p.Cursor < len(p.Summaries)-1 {
		p.Cursor++
	}
}

// Selected returns the currently selected session ID, or zero if none.
func (p *SessionPicker) Selected() domain.SessionID {
	if p.Cursor < 0 || p.Cursor >= len(p.Summaries) {
		return domain.SessionID{}
	}
	return p.Summaries[p.Cursor].ID
}

// Render renders the session picker as a string for viewport display. When
// height is positive, the list is windowed around the cursor so sessions
// beyond one screen remain reachable.
func (p *SessionPicker) Render(width, height int) string {
	if !p.Loaded {
		return "Loading sessions..."
	}
	if p.Error != nil {
		return fmt.Sprintf("Error loading sessions: %v", p.Error)
	}
	if len(p.Summaries) == 0 {
		return "No existing sessions found.\nPress Esc to go back."
	}

	start, end := 0, len(p.Summaries)
	if height > 0 {
		visible := height - 4 // heading, blank line, scroll hints, footer
		if visible < 1 {
			visible = 1
		}
		if p.Cursor >= visible {
			start = p.Cursor - visible + 1
		}
		end = min(start+visible, len(p.Summaries))
	}

	var b strings.Builder
	b.WriteString("Select a session to resume:\n\n")
	if start > 0 {
		b.WriteString("↑ more\n")
	}

	for i := start; i < end; i++ {
		s := p.Summaries[i]
		prefix := "  "
		if i == p.Cursor {
			prefix = "▶ "
		}
		line := fmt.Sprintf("%s%s  (v%d, updated %s)",
			prefix, s.ID, s.Version,
			formatTimeAgo(s.UpdatedAt))
		if width > 0 {
			line = truncateDisplayWidth(line, width)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}
	if end < len(p.Summaries) {
		b.WriteString("↓ more\n")
	}

	b.WriteString("\nj/k or ↑/↓ = move   Enter = select   Esc = back")
	return b.String()
}

// ModelOption is one selectable provider/model entry in the /model picker.
// The catalog is static for the process lifetime (the config file loads
// once at startup), so it is handed to the TUI via InitOptions.
type ModelOption struct {
	Provider      string
	Name          string
	ContextWindow int64
	WireAPI       string
}

// Ref returns the canonical "provider/model" reference accepted by /model.
func (o ModelOption) Ref() string { return o.Provider + "/" + o.Name }

// ModelPicker manages the state for picking a model. Unlike SessionPicker
// there is no loading or error state: the catalog is known up front.
type ModelPicker struct {
	Options []ModelOption
	Cursor  int
	current string // ref of the active model, marked in the list
}

// NewModelPicker creates a picker with the cursor on the active model, so
// the common "open and confirm" flow costs zero keystrokes.
func NewModelPicker(options []ModelOption, currentRef string) *ModelPicker {
	p := &ModelPicker{Options: options, current: currentRef}
	for i, o := range options {
		if o.Ref() == currentRef {
			p.Cursor = i
			break
		}
	}
	return p
}

// MoveUp moves the cursor up.
func (p *ModelPicker) MoveUp() {
	if p.Cursor > 0 {
		p.Cursor--
	}
}

// MoveDown moves the cursor down.
func (p *ModelPicker) MoveDown() {
	if p.Cursor < len(p.Options)-1 {
		p.Cursor++
	}
}

// Selected returns the highlighted option, or nil when the list is empty.
func (p *ModelPicker) Selected() *ModelOption {
	if p.Cursor < 0 || p.Cursor >= len(p.Options) {
		return nil
	}
	return &p.Options[p.Cursor]
}

// Render renders the model picker as a string for viewport display,
// windowed around the cursor like the session picker.
func (p *ModelPicker) Render(width, height int) string {
	if len(p.Options) == 0 {
		return "No models configured.\nPress Esc to go back."
	}

	start, end := 0, len(p.Options)
	if height > 0 {
		visible := height - 4 // heading, blank line, scroll hints, footer
		if visible < 1 {
			visible = 1
		}
		if p.Cursor >= visible {
			start = p.Cursor - visible + 1
		}
		end = min(start+visible, len(p.Options))
	}

	// Align the metadata column to the widest reference in the whole list
	// (not just the window) so columns stay put while scrolling.
	refWidth := 0
	for _, o := range p.Options {
		if n := len(o.Ref()); n > refWidth {
			refWidth = n
		}
	}

	var b strings.Builder
	b.WriteString("Select a model:\n\n")
	if start > 0 {
		b.WriteString("↑ more\n")
	}
	for i := start; i < end; i++ {
		o := p.Options[i]
		prefix := "  "
		if i == p.Cursor {
			prefix = "▶ "
		}
		marker := ""
		if o.Ref() == p.current {
			marker = " ●"
		}
		line := fmt.Sprintf("%s%-*s %s%s", prefix, refWidth, o.Ref(), modelOptionMeta(o), marker)
		if width > 0 {
			line = truncateDisplayWidth(line, width)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}
	if end < len(p.Options) {
		b.WriteString("↓ more\n")
	}

	b.WriteString("\nj/k or ↑/↓ = move   Enter = select   Esc = cancel")
	return b.String()
}

// modelOptionMeta renders the trailing metadata column: "200k ctx ·
// responses", with absent parts omitted.
func modelOptionMeta(o ModelOption) string {
	var parts []string
	if o.ContextWindow > 0 {
		parts = append(parts, formatTokens(o.ContextWindow)+" ctx")
	}
	if o.WireAPI != "" {
		parts = append(parts, o.WireAPI)
	}
	return strings.Join(parts, " · ")
}

// ReasoningLevel is one selectable dial in the /reasoning picker. Arg is
// the SetReasoning argument.
type ReasoningLevel struct {
	Arg   string
	Label string
	Desc  string
}

// ReasoningLevels is the fixed dial catalog in display order: the config
// fallback first, then the four override levels from least to most thinking.
var ReasoningLevels = []ReasoningLevel{
	{Arg: "default", Label: "default", Desc: "follow the model's configured reasoning"},
	{Arg: "off", Label: "off", Desc: "no thinking; fastest and cheapest"},
	{Arg: "low", Label: "low", Desc: "light thinking (≈1/8 of the output budget on Anthropic)"},
	{Arg: "medium", Label: "medium", Desc: "moderate thinking (≈1/3 of the output budget)"},
	{Arg: "high", Label: "high", Desc: "deep thinking (≈2/3 of the output budget)"},
}

// ReasoningPicker manages the state for picking a reasoning level. Like the
// model picker there is no loading state: the catalog is fixed.
type ReasoningPicker struct {
	Cursor int
	// currentArg is the level Arg currently in effect, marked with ●.
	currentArg string
}

// NewReasoningPicker creates a picker with the cursor on the active dial:
// the session-override level when one is set, "default" otherwise.
func NewReasoningPicker(effort string, overridden bool) *ReasoningPicker {
	current := "default"
	if overridden && effort != "" {
		current = effort
	}
	p := &ReasoningPicker{currentArg: current}
	for i, l := range ReasoningLevels {
		if l.Arg == current {
			p.Cursor = i
			break
		}
	}
	return p
}

// MoveUp moves the cursor up.
func (p *ReasoningPicker) MoveUp() {
	if p.Cursor > 0 {
		p.Cursor--
	}
}

// MoveDown moves the cursor down.
func (p *ReasoningPicker) MoveDown() {
	if p.Cursor < len(ReasoningLevels)-1 {
		p.Cursor++
	}
}

// Selected returns the highlighted level.
func (p *ReasoningPicker) Selected() *ReasoningLevel {
	if p.Cursor < 0 || p.Cursor >= len(ReasoningLevels) {
		return nil
	}
	return &ReasoningLevels[p.Cursor]
}

// Render renders the reasoning picker as a string for viewport display.
func (p *ReasoningPicker) Render(width, height int) string {
	labelWidth := 0
	for _, l := range ReasoningLevels {
		if len(l.Label) > labelWidth {
			labelWidth = len(l.Label)
		}
	}

	var b strings.Builder
	b.WriteString("Select a reasoning level:\n\n")
	for i, l := range ReasoningLevels {
		prefix := "  "
		if i == p.Cursor {
			prefix = "▶ "
		}
		marker := ""
		if l.Arg == p.currentArg {
			marker = " ●"
		}
		line := fmt.Sprintf("%s%-*s  %s%s", prefix, labelWidth, l.Label, l.Desc, marker)
		if width > 0 {
			line = truncateDisplayWidth(line, width)
		}
		b.WriteString(line)
		b.WriteString("\n")
	}

	b.WriteString("\nj/k or ↑/↓ = move   Enter = select   Esc = cancel")
	return b.String()
}

// formatTokens renders a token count in compact decimal form (200k, 1.0M).
func formatTokens(n int64) string {
	switch {
	case n < 1000:
		return strconv.FormatInt(n, 10)
	case n < 1_000_000:
		return fmt.Sprintf("%dk", n/1000)
	default:
		return fmt.Sprintf("%.1fM", float64(n)/1_000_000)
	}
}

// formatTimeAgo returns a short relative time description.
func formatTimeAgo(t time.Time) string {
	d := time.Since(t)
	if d < time.Minute {
		return "just now"
	}
	if d < time.Hour {
		return fmt.Sprintf("%dm ago", int(d.Minutes()))
	}
	if d < 24*time.Hour {
		return fmt.Sprintf("%dh ago", int(d.Hours()))
	}
	if d < 30*24*time.Hour {
		return fmt.Sprintf("%dd ago", int(d.Hours()/24))
	}
	return t.Format("2006-01-02")
}
