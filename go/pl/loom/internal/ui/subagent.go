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
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// subagent.go implements the read-only drill-in view for delegate_task
// child runs (docs/SUBAGENT_DESIGN.md §10): Ctrl+G opens a full-area
// overlay rendering the child session's transcript — pulled from the
// child checkpoint, refreshed once a second while the child is active.
// The overlay has no composer and no input path; watching never
// interferes with the child run.

// subagentRefreshInterval is the overlay's checkpoint re-pull cadence
// while the viewed child is still running.
const subagentRefreshInterval = time.Second

// subagentOverlay is the drill-in overlay's state.
type subagentOverlay struct {
	childID domain.SessionID
	task    string

	viewport   lineView
	followTail bool

	usage   domain.Usage
	outcome domain.Outcome
	active  bool
	err     error
}

// subagentViewMsg delivers one checkpoint projection of the viewed child.
type subagentViewMsg struct {
	childID domain.SessionID
	view    app.SubagentView
	err     error
}

// subagentTickMsg drives the periodic re-pull while the child is active.
type subagentTickMsg struct{ childID domain.SessionID }

// latestSubagentBlock finds the drill-in target: the in-flight
// delegation if one exists, otherwise the most recent one.
func (m Model) latestSubagentBlock() *TranscriptBlock {
	var latest *TranscriptBlock
	for i := len(m.blocks.Order) - 1; i >= 0; i-- {
		block := m.blocks.ByID[m.blocks.Order[i]]
		if block.Subagent == nil {
			continue
		}
		if block.Subagent.Outcome == "" {
			return block // an active delegation always wins
		}
		if latest == nil {
			latest = block
		}
	}
	return latest
}

// openSubagentOverlay enters the drill-in overlay for the most relevant
// delegation (Ctrl+G, /agent): the in-flight one, else the most recent.
func (m Model) openSubagentOverlay() (tea.Model, tea.Cmd) {
	block := m.latestSubagentBlock()
	if block == nil {
		m.setStatus("No sub-agent run in this session yet — delegate_task spawns one", false)
		return m, nil
	}
	return m.openSubagentOverlayFor(block)
}

// openSubagentOverlayFor enters the drill-in overlay for one specific
// delegation (mouse click on its tool block).
func (m Model) openSubagentOverlayFor(block *TranscriptBlock) (tea.Model, tea.Cmd) {
	if block == nil || block.Subagent == nil {
		return m, nil
	}
	m.subOverlay = &subagentOverlay{
		childID:    block.Subagent.ChildID,
		task:       block.Target,
		viewport:   lineView{Width: max(1, m.width), Height: max(1, m.height-4)},
		followTail: true,
	}
	m.mode = ModeSubagent
	return m, m.fetchSubagentView(block.Subagent.ChildID)
}

// subagentAt returns the delegate tool block occupying the clicked
// screen row, or nil when the row holds anything else. Shares the
// hit-testing geometry with the reasoning click-to-expand.
func (m *Model) subagentAt(screenY int) *TranscriptBlock {
	if hit := m.blockAtRow(screenY); hit != nil && hit.Subagent != nil {
		return hit
	}
	return nil
}

// fetchSubagentView pulls the child's latest checkpoint projection.
func (m Model) fetchSubagentView(childID domain.SessionID) tea.Cmd {
	return func() tea.Msg {
		view, err := m.controller.SubagentView(context.Background(), childID)
		return subagentViewMsg{childID: childID, view: view, err: err}
	}
}

// handleSubagentViewMsg applies a fresh checkpoint projection to the
// overlay. Stale responses (overlay closed, or for a different child)
// are dropped.
func (m Model) handleSubagentViewMsg(msg subagentViewMsg) (tea.Model, tea.Cmd) {
	o := m.subOverlay
	if o == nil || o.childID != msg.childID {
		return m, nil
	}
	if msg.err != nil {
		o.err = msg.err
		return m, nil
	}
	o.err = nil
	o.usage = msg.view.Usage
	o.outcome = msg.view.Outcome
	o.active = msg.view.Active
	o.viewport.SetContent(m.buildSubagentContent(msg.view.Messages))
	if o.followTail {
		o.viewport.GotoBottom()
	}
	if !o.active {
		return m, nil // terminal: stop refreshing
	}
	childID := o.childID
	return m, tea.Tick(subagentRefreshInterval, func(time.Time) tea.Msg {
		return subagentTickMsg{childID: childID}
	})
}

// handleSubagentTick re-pulls the child projection after each interval.
func (m Model) handleSubagentTick(msg subagentTickMsg) (tea.Model, tea.Cmd) {
	o := m.subOverlay
	if o == nil || o.childID != msg.childID || !o.active {
		return m, nil
	}
	return m, m.fetchSubagentView(o.childID)
}

// handleSubagentProgressOverlay keeps the open overlay's footer counters
// fresh between checkpoint pulls (progress events arrive once a second
// from the bridge's ticker; the block-side state is updated separately
// by ApplyRuntimeEvent).
func (m *Model) handleSubagentProgressOverlay(evt runtimeevent.RuntimeEvent) {
	o := m.subOverlay
	if o == nil {
		return
	}
	var payload runtimeevent.SubagentProgressPayload
	if err := json.Unmarshal(evt.Payload, &payload); err != nil || payload.ChildSessionID != o.childID {
		return
	}
	o.usage.ToolCalls = payload.ToolCalls
	o.usage.InputTokens = payload.InputTokens
	o.usage.OutputTokens = payload.OutputTokens
}

// handleSubagentKey routes keys while the drill-in overlay is active:
// scroll and close only — the overlay is strictly read-only.
func (m Model) handleSubagentKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	o := m.subOverlay
	if o == nil {
		m.mode = ModeChat
		return m, nil
	}
	switch msg.Type {
	case tea.KeyEsc:
		m.subOverlay = nil
		m.mode = ModeChat
		return m, nil
	case tea.KeyRunes:
		if msg.String() == "q" {
			m.subOverlay = nil
			m.mode = ModeChat
		}
		return m, nil
	case tea.KeyUp:
		o.followTail = false
		o.viewport.LineUp(1)
		return m, nil
	case tea.KeyDown:
		o.viewport.LineDown(1)
		o.followTail = o.viewport.AtBottom()
		return m, nil
	case tea.KeyPgUp:
		o.followTail = false
		o.viewport.LineUp(o.viewport.Height)
		return m, nil
	case tea.KeyPgDown:
		o.viewport.LineDown(o.viewport.Height)
		o.followTail = o.viewport.AtBottom()
		return m, nil
	case tea.KeyCtrlEnd:
		o.followTail = true
		o.viewport.GotoBottom()
		return m, nil
	case tea.KeyCtrlC:
		// Keep Ctrl+C's global meaning: closing the overlay is passive,
		// the cancel belongs to the turn.
		m.subOverlay = nil
		m.mode = ModeChat
		return m.handleCtrlC()
	}
	return m, nil
}

// buildSubagentContent renders the child's messages with the same block
// renderers as the main transcript — the overlay reads like a loom
// conversation, minus the composer.
func (m Model) buildSubagentContent(messages []domain.Message) string {
	idx := RebuildTranscript(messages)
	if len(idx.Order) == 0 {
		return m.theme.Dim.Render("  Sub-agent has not produced any messages yet…")
	}
	var b strings.Builder
	for i, id := range idx.Order {
		if i > 0 {
			b.WriteString("\n\n")
		}
		b.WriteString(m.renderBlock(idx.ByID[id]))
	}
	return b.String()
}

// renderSubagentOverlay renders the drill-in view: a title band with the
// task and live status, the child transcript, and a footer with counters
// and the read-only key hints.
func (m Model) renderSubagentOverlay() string {
	o := m.subOverlay
	if o == nil {
		return ""
	}
	status := "running"
	switch {
	case o.err != nil:
		status = "checkpoint unavailable"
	case !o.active && o.outcome != "":
		status = string(o.outcome)
	case !o.active:
		status = "finished"
	}
	idLabel := o.childID.String()
	if len(idLabel) > 8 {
		idLabel = idLabel[:8]
	}
	title := m.theme.DialogTitle.Render(fmt.Sprintf("Sub-agent · %s · %s", idLabel, status))
	task := ""
	if o.task != "" {
		task = m.theme.Dim.Render("  " + truncateDisplayWidth(o.task, max(20, m.width-4)))
	}

	height := max(1, m.height-6)
	if o.viewport.Height != height || o.viewport.Width != max(1, m.width) {
		o.viewport.Width = max(1, m.width)
		o.viewport.Height = height
	}

	var footerLeft string
	if o.err != nil {
		footerLeft = fmt.Sprintf("read-only · %v", o.err)
	} else {
		footerLeft = fmt.Sprintf("read-only · %d calls · in %s · out %s",
			o.usage.ToolCalls, humanizeTokens(o.usage.InputTokens), humanizeTokens(o.usage.OutputTokens))
	}
	footer := m.theme.Dim.Render(footerLeft + " · Esc/q back · ↑↓ scroll")

	var b strings.Builder
	b.WriteString(title)
	if task != "" {
		b.WriteString("\n")
		b.WriteString(task)
	}
	b.WriteString("\n")
	b.WriteString(o.viewport.View())
	b.WriteString("\n")
	b.WriteString(footer)
	return b.String()
}

// renderSubagentProgress renders the live progress line beneath a
// delegate_task block's one-line summary.
func (m Model) renderSubagentProgress(block *TranscriptBlock) string {
	state := block.Subagent
	if state == nil {
		return ""
	}
	var line string
	tokens := humanizeTokens(state.InputTokens + state.OutputTokens)
	if state.Outcome == "" {
		line = fmt.Sprintf("↳ exploring… %d calls · %s tok · click or Ctrl+G to watch", state.ToolCalls, tokens)
	} else {
		line = fmt.Sprintf("↳ %d calls · %s tok · %s · click or Ctrl+G to view", state.ToolCalls, tokens, state.Outcome)
	}
	return m.theme.Dim.Render(indentLines(line, "  "))
}
