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

	b.WriteString("\nEsc = back   Enter = select")
	return b.String()
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
