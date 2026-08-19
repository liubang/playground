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

package ui

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// maxEventResubscribes bounds how many times the UI re-attaches to the event
// stream after the broker disconnects it before locking input.
const maxEventResubscribes = 3

// maxCompletionRows bounds the visible completion candidates. The command
// registry is small enough that every candidate fits — the popup shows them
// all. The cap (and the cursor windowing in renderCompletion) only starts
// to matter if the registry ever outgrows one screen.
var maxCompletionRows = len(slashCommands)

// slashCommand describes one slash command for help and completion.
type slashCommand struct {
	name  string // canonical command, e.g. "/resume"
	usage string // display form, e.g. "/resume <id>"
	desc  string
}

// slashCommands is the registry used by /help and command completion.
var slashCommands = []slashCommand{
	{name: "/help", usage: "/help", desc: "Show key bindings and commands"},
	{name: "/new", usage: "/new", desc: "Start a new session"},
	{name: "/sessions", usage: "/sessions", desc: "Pick a session to resume"},
	{name: "/resume", usage: "/resume <id>", desc: "Resume a session by ID"},
	{name: "/clear", usage: "/clear", desc: "Clear transcript view (history retained)"},
	{name: "/compact", usage: "/compact", desc: "Compact context before the next model call"},
	{name: "/rewind", usage: "/rewind [seq]", desc: "List checkpoints, or rewind session and files to one"},
	{name: "/agent", usage: "/agent", desc: "View the sub-agent run (read-only)"},
	{name: "/model", usage: "/model [name]", desc: "Show or switch the active model"},
	{name: "/reasoning", usage: "/reasoning [level]", desc: "Show or adjust the reasoning level"},
	{name: "/skill", usage: "/skill", desc: "List discovered skills"},
	{name: "/mcp", usage: "/mcp", desc: "List MCP servers and their status"},
	{name: "/doctor", usage: "/doctor", desc: "Show the toolchain/PATH environment report"},
	{name: "/rules", usage: "/rules", desc: "View and manage permission rules"},
	{name: "/exit", usage: "/exit", desc: "Exit"},
}

// --- async controller command results ---

// promptSubmittedMsg reports the controller's ack for a submitted prompt.
type promptSubmittedMsg struct {
	prompt     string
	result     app.SubmitResult
	err        error
	imageCount int
}

// sessionAction describes an asynchronous new/resume session operation.
type sessionAction struct {
	name    string // display name used in status messages
	command string // original composer input, restored on failure when non-empty
	success string // status message on success
	run     func(context.Context) error
}

// sessionSwitchedMsg reports the result of a sessionAction.
type sessionSwitchedMsg struct {
	action sessionAction
	err    error
}

// modelChangedMsg reports the result of a /model switch request. command
// carries the original composer input so a failure can restore the draft.
type modelChangedMsg struct {
	command string
	result  app.SetModelResult
	err     error
}

// reasoningChangedMsg reports the result of a /reasoning request. command
// carries the original composer input so a failure can restore the draft.
type reasoningChangedMsg struct {
	command string
	result  app.SetReasoningResult
	err     error
}

// compactRequestedMsg reports the result of a /compact request.
type compactRequestedMsg struct {
	result app.RequestCompactionResult
	err    error
}

// checkpointsListedMsg reports the result of a bare /rewind listing.
type checkpointsListedMsg struct {
	checkpoints []app.CheckpointInfo
	err         error
}

// rewindFinishedMsg reports the result of a /rewind <seq> execution.
// command carries the original composer input so a failure can restore
// the draft.
type rewindFinishedMsg struct {
	command string
	outcome app.RewindOutcome
	err     error
}

// skillsLoadedMsg reports the result of a /skill listing request.
type skillsLoadedMsg struct {
	listing app.SkillsListing
	err     error
}

// mcpServersLoadedMsg reports the result of a /mcp listing request.
type mcpServersLoadedMsg struct {
	servers []app.MCPServerInfo
	err     error
}

// envLoadedMsg reports the result of a /doctor report request.
type envLoadedMsg struct {
	report *app.ToolchainReport
	err    error
}

// questionAnsweredMsg reports the result of answering an ask_user question.
type questionAnsweredMsg struct{ err error }

// turnCancelRequestedMsg reports the result of a cancel request.
type turnCancelRequestedMsg struct{ err error }

// approvalResolvedMsg reports the result of an approval resolution.
type approvalResolvedMsg struct {
	err      error
	ruleNote string
}

// Update is the single-threaded reducer for the TUI.
func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var next tea.Model
	var cmd tea.Cmd
	switch msg := msg.(type) {
	case tea.KeyMsg:
		next, cmd = m.handleKey(msg)
	case tea.MouseMsg:
		next, cmd = m.handleMouse(msg)
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		next = m
	case runtimeEventMsg:
		next, cmd = m.handleRuntimeEvent(runtimeevent.RuntimeEvent(msg))
	case runtimeEventsClosedMsg:
		next, cmd = m.handleEventsClosed(msg)
	case snapshotMsg:
		next, cmd = m.handleSnapshot(msg)
	case searchDebounceMsg:
		// A stale tick (newer keystrokes arrived, or search was exited)
		// must not rescan: only the latest generation in search mode runs.
		if msg.gen == m.searchGen && m.mode == ModeSearch {
			m.updateSearch()
		}
		next = m
	case subagentViewMsg:
		next, cmd = m.handleSubagentViewMsg(msg)
	case subagentTickMsg:
		next, cmd = m.handleSubagentTick(msg)
	case sessionsLoadedMsg:
		if m.sessionFinder != nil {
			m.sessionFinder.Load(sessionFinderItems(msg.sessions), msg.err)
		}
		next = m
	case rulesLoadedMsg:
		if m.rulesFinder != nil {
			// The picker is open (initial load or post-delete refresh):
			// reload items in place so deletions become visible.
			m.rulesFinder.Load(rulesFinderItems(msg.rules), msg.err)
		} else if msg.err != nil {
			m.setStatus(fmt.Sprintf("Load rules: %v", msg.err), true)
			m.mode = ModeChat
		} else {
			m.rulesFinder = m.NewRulesFinder(msg.rules)
		}
		next = m
	case ruleForgottenMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Delete failed: %v", msg.err), true)
		} else {
			m.setStatus(fmt.Sprintf("Deleted %s rule %q", msg.entry.Kind, msg.entry.Label), false)
		}
		m.rulesDeletePending = nil
		// Refresh the rules list after deletion.
		next = m
		cmd = m.requestRules()
	case promptSubmittedMsg:
		next = m.handlePromptSubmitted(msg)
	case sessionSwitchedMsg:
		next, cmd = m.handleSessionSwitched(msg)
	case modelChangedMsg:
		next = m.handleModelChanged(msg)
	case reasoningChangedMsg:
		next = m.handleReasoningChanged(msg)
	case compactRequestedMsg:
		next = m.handleCompactRequested(msg)
	case checkpointsListedMsg:
		next = m.handleCheckpointsListed(msg)
	case rewindFinishedMsg:
		next, cmd = m.handleRewindFinished(msg)
	case skillsLoadedMsg:
		next = m.handleSkillsLoaded(msg)
	case mcpServersLoadedMsg:
		next = m.handleMCPServersLoaded(msg)
	case envLoadedMsg:
		next = m.handleEnvLoaded(msg)
	case questionAnsweredMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Answer delivery failed: %v", msg.err), true)
		}
		next = m
	case turnCancelRequestedMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Cancel failed: %v", msg.err), true)
		}
		next = m
	case approvalResolvedMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Approval resolution rejected: %v", msg.err), true)
		} else if msg.ruleNote != "" {
			m.setStatus(fmt.Sprintf("Allowed; future %q commands auto-approved this session", msg.ruleNote), false)
		}
		next = m
	case imageAttachedMsg:
		next, cmd = m.handleImageAttached(msg)
	case clipboardImageMsg:
		next, cmd = m.handleClipboardImage(msg)
	case clipboardCopiedMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Copy failed: %v", msg.err), true)
		} else {
			m.setStatus(fmt.Sprintf("Copied last reply (%d chars)", msg.chars), false)
		}
		next = m
	case spinner.TickMsg:
		// Spinner frames drive all busy-time redraws (activity timers, tool
		// elapsed durations); the chain stops as soon as the turn idles.
		m.spinner, cmd = m.spinner.Update(msg)
		if m.isBusy() {
			next = m
		} else {
			m.spinning = false
			next, cmd = m, nil
		}
	default:
		next = m
	}
	// Keep derived layout (viewport/composer geometry) consistent after any
	// state transition, including mode switches that hide the composer.
	if updated, ok := next.(Model); ok {
		updated.layout()
		return updated, cmd
	}
	return next, cmd
}

func (m *Model) layout() {
	// Until the first WindowSizeMsg arrives the terminal size is unknown.
	// Keep the sane NewModel defaults (80x20) instead of collapsing the
	// viewport to 1x1 — a degenerate first frame desynchronizes the
	// renderer's line tracking and corrupts every repaint afterwards.
	if m.width <= 0 || m.height <= 0 {
		return
	}
	m.viewport.Width = max(1, m.width)
	m.textArea.SetWidth(max(1, m.width-4))
	// The composer grows with the draft (1..8 lines, per the design), capped
	// by what the terminal can spare, so an empty input never walls off the
	// transcript behind a tall empty box.
	contentLines := min(max(m.textArea.LineCount(), 1), 8)
	m.textArea.SetHeight(min(ComposerHeight(m.height-4), contentLines))
	m.viewport.Height = m.visibleTranscriptHeight()
	m.syncTranscript()
}

// syncTranscript rebuilds the viewport content from the block index. It must
// run on the model owned by Update — not on a View-time copy — so scrolling
// state (YOffset, AtBottom, follow-tail) operates on real content. Without
// this the Update-side viewport stays empty and every scroll is a no-op.
//
// Rendering is incremental: blocks are memoized by a fingerprint of their
// render inputs, so long sessions do not re-render every block per event.
func (m *Model) syncTranscript() {
	// Skip the rebuild when nothing render-relevant changed (REVIEW M14).
	// Volatile blocks (in-progress, live timers/spinners) always force a
	// sync so animations keep advancing — but a volatile-forced sync is
	// cheap: only the changed suffix is re-rendered (see below).
	sameIdx := m.blocks == m.lastSyncIdx
	if sameIdx && m.blocks.Version() == m.lastSyncVersion &&
		m.width == m.lastSyncWidth && m.theme.NoColor == m.lastSyncNoColor &&
		!m.hasVolatileBlocks() {
		return
	}
	m.transcriptBuilds++
	m.lastSyncIdx = m.blocks
	m.lastSyncVersion = m.blocks.Version()
	m.lastSyncWidth = m.width
	m.lastSyncNoColor = m.theme.NoColor
	if len(m.blocks.Order) == 0 {
		m.viewport.SetContent(m.renderWelcome())
		m.lastOrder = nil
		return
	}
	if m.renderCache == nil {
		m.renderCache = make(map[string]cachedRender)
	}
	if m.blockOffsets == nil {
		m.blockOffsets = make(map[string]int, len(m.blocks.Order))
	}

	order := m.blocks.Order
	// Longest positionally-stable, cache-valid prefix: these blocks keep
	// their rendered lines and offsets untouched, so a sync splices only
	// the changed suffix instead of concatenating and re-splitting the
	// whole transcript. Streaming is the hot path: the tail block is
	// volatile, everything before it survives the prefix check.
	prefix := 0
	if sameIdx {
		for prefix < len(order) {
			if prefix >= len(m.lastOrder) || m.lastOrder[prefix] != order[prefix] {
				break
			}
			id := order[prefix]
			key := m.blockRenderKey(m.blocks.ByID[id])
			entry, ok := m.renderCache[id]
			if key == "" || !ok || entry.key != key {
				break
			}
			if _, ok := m.blockOffsets[id]; !ok {
				break
			}
			prefix++
		}
	}
	if prefix == len(order) {
		// Forced here by volatility yet nothing re-renderable changed;
		// keep the previous frame.
		return
	}

	// Offsets past the splice point are recomputed below.
	for _, id := range m.lastOrder[min(prefix, len(m.lastOrder)):] {
		delete(m.blockOffsets, id)
	}

	var lines []string
	if prefix > 0 {
		lastKept := order[prefix-1]
		keepEnd := m.blockOffsets[lastKept] + len(m.renderCache[lastKept].lines)
		keepEnd = min(keepEnd, len(m.viewport.lines))
		lines = append([]string(nil), m.viewport.lines[:keepEnd]...)
	}
	offset := len(lines)

	m.lastSyncRendered = 0
	var prev *TranscriptBlock
	if prefix > 0 {
		prev = m.blocks.ByID[order[prefix-1]]
	}
	for i := prefix; i < len(order); i++ {
		id := order[i]
		block := m.blocks.ByID[id]
		if prev != nil {
			// A blank row separates logical sections (Claude Code's airy
			// layout); consecutive tool calls stay packed so a retry burst
			// still reads as one list instead of scattered rows.
			sep := 1
			if prev.Kind == BlockKindTool && block.Kind == BlockKindTool {
				sep = 0
			}
			for j := 0; j < sep; j++ {
				lines = append(lines, "")
				offset++
			}
		}
		m.blockOffsets[id] = offset
		key := m.blockRenderKey(block)
		var blockLines []string
		if key != "" {
			if entry, ok := m.renderCache[id]; ok && entry.key == key {
				blockLines = entry.lines
			}
		}
		if blockLines == nil {
			out := m.renderBlock(block)
			blockLines = strings.Split(out, "\n")
			m.lastSyncRendered++
			if key != "" {
				m.renderCache[id] = cachedRender{key: key, out: out, lines: blockLines}
			}
		}
		lines = append(lines, blockLines...)
		offset += len(blockLines)
		prev = block
	}
	m.viewport.SetLines(lines)
	if m.followTail {
		m.viewport.GotoBottom()
	}
	m.lastOrder = append(m.lastOrder[:0], order...)
}

// hasVolatileBlocks reports whether any block embeds live state (spinner
// frames, elapsed timers) whose rendering must advance on every tick.
func (m *Model) hasVolatileBlocks() bool {
	for _, id := range m.blocks.Order {
		block := m.blocks.ByID[id]
		if !block.Done {
			return true
		}
		switch block.Status {
		case "running", "prepared", "approval", "pending":
			return true
		}
	}
	return false
}

// cachedRender is a memoized renderBlock output: the joined string and
// its pre-split lines (the incremental sync splices line slices, so a
// cache hit must not pay a re-split).
type cachedRender struct {
	key   string
	out   string
	lines []string
}

// blockRenderKey fingerprints every input that affects a block's rendered
// output. Blocks whose rendering embeds live state (spinners, elapsed
// timers) return an empty key and are never cached.
func (m Model) blockRenderKey(block *TranscriptBlock) string {
	// In-progress blocks embed spinner frames and a live elapsed timer;
	// caching them would freeze the animation between events.
	if !block.Done {
		return ""
	}
	switch block.Status {
	case "running", "prepared", "approval", "pending":
		return ""
	}
	var sb strings.Builder
	sb.Grow(len(block.Content) + len(block.Preview) + len(block.Diff) + 160)
	fmt.Fprintf(&sb, "%s|%s|%s|%s|%s|%s|%t|%t|%s|%s|%d|%t\n%s",
		block.Kind, block.Status, block.Title, block.Detail, block.Target,
		block.Diff, block.Expanded, block.ReasoningExpanded, block.StreamReasoning,
		block.PreparingTool, m.width, m.theme.NoColor, block.Content)
	if state := block.Subagent; state != nil {
		fmt.Fprintf(&sb, "|%s|%d|%d|%s", state.ChildID, state.ToolCalls,
			state.InputTokens+state.OutputTokens, state.Outcome)
	}
	return sb.String()
}

// visibleTranscriptHeight computes available height for the transcript
// viewport, reserving space for whatever occupies the composer area.
// Floating overlays (alt-screen help/question) reserve nothing: they
// draw over the chat frame, so the base layout applies (m.baseMode).
func (m Model) visibleTranscriptHeight() int {
	reserved := 1 + 1 // header + status bar
	switch m.baseMode() {
	case ModeChat:
		reserved++                          // the spacer row above the composer area
		reserved += m.textArea.Height() + 2 // composer border
		reserved += m.steerPanelHeight()
		reserved += m.completionHeight()
		reserved += m.planPanelHeight()
		reserved += m.attachmentIndicatorHeight()
	case ModeSearch:
		reserved++    // spacer row
		reserved += 3 // one-line search bar + border
	case ModeApproval:
		// Reserve the band's actual height: the prompt is line-count
		// variable (metadata, note, paths and diff rows come and go), so a
		// fixed reservation would strand the status bar above the bottom.
		reserved++ // spacer row
		reserved += len(m.approvalOverlayLines())
	case ModeHelp:
		reserved++ // spacer row
		reserved += helpOverlayHeight
	case ModeListing:
		// Like the approval band, the listing dialog is line-count variable.
		reserved++ // spacer row
		reserved += m.listingOverlayHeight()
	case ModeQuestion:
		// Like the approval band, the question overlay is line-count
		// variable (question text wraps, option list varies).
		reserved++ // spacer row
		reserved += m.questionOverlayHeight()
	}
	if m.height > reserved {
		return m.height - reserved
	}
	return 1
}

// --- slash command completion ---

// completionCandidates returns registry entries matching the current
// slash-prefixed draft. Completion applies only while typing the command name
// itself (no space or newline yet).
func (m Model) completionCandidates() []slashCommand {
	value := m.textArea.Value()
	if !strings.HasPrefix(value, "/") || strings.ContainsAny(value, " \n") {
		return nil
	}
	var out []slashCommand
	for _, cmd := range slashCommands {
		if strings.HasPrefix(cmd.name, value) {
			out = append(out, cmd)
		}
	}
	return out
}

// completionVisible reports whether the completion popup should be shown.
// A dismissal is tied to the draft it was made on: once the draft changes
// (typing, reset, applying a completion), the popup re-arms automatically.
func (m Model) completionVisible() bool {
	if m.mode != ModeChat || len(m.completionCandidates()) == 0 {
		return false
	}
	return m.completionDismissedFor != m.textArea.Value()
}

// completionHeight returns the rows reserved above the composer for the popup.
func (m Model) completionHeight() int {
	if !m.completionVisible() {
		return 0
	}
	return min(len(m.completionCandidates()), maxCompletionRows) + 2
}

func (m *Model) moveCompletionCursor(delta int) {
	candidates := m.completionCandidates()
	if len(candidates) == 0 {
		m.completionCursor = 0
		return
	}
	m.completionCursor = (m.completionCursor + delta + len(candidates)) % len(candidates)
}

func (m Model) currentCompletion() (slashCommand, bool) {
	candidates := m.completionCandidates()
	if len(candidates) == 0 {
		return slashCommand{}, false
	}
	return candidates[min(m.completionCursor, len(candidates)-1)], true
}

// applyCompletion replaces the draft with the selected command (plus a space
// when the command takes an argument).
func (m *Model) applyCompletion(cmd slashCommand) {
	m.textArea.SetValue(cmd.name)
	if strings.Contains(cmd.usage, "<") {
		m.textArea.InsertString(" ")
	}
	m.completionDismissedFor = m.textArea.Value()
}

// isExecutableCommand reports whether the draft names a known command and may
// be submitted directly instead of completed first.
func (m Model) isExecutableCommand(value string) bool {
	fields := strings.Fields(value)
	if len(fields) == 0 {
		return false
	}
	for _, cmd := range slashCommands {
		if cmd.name == fields[0] {
			return true
		}
	}
	return false
}

// --- keyboard ---

func (m Model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch m.mode {
	case ModeApproval:
		return m.handleApprovalKey(msg)
	case ModeHelp:
		m.mode = ModeChat
		return m, nil
	case ModeListing:
		return m.handleListingKey(msg)
	case ModeSessionPicker:
		return m.handleSessionFinderKey(msg)
	case ModeRules:
		return m.handleRulesFinderKey(msg)
	case ModeModelPicker:
		return m.handleModelFinderKey(msg)
	case ModeReasoningPicker:
		return m.handleReasoningFinderKey(msg)
	case ModeQuestion:
		return m.handleQuestionKey(msg)
	case ModeSearch:
		return m.handleSearchKey(msg)
	case ModeSubagent:
		return m.handleSubagentKey(msg)
	}

	switch msg.Type {
	case tea.KeyCtrlC:
		return m.handleCtrlC()
	case tea.KeyCtrlD:
		return m.handleCtrlD()
	}
	// Bindable view actions resolve through the keymap (docs/
	// VIM_UI_DESIGN.md §5); structural keys interleaved with composer
	// editing stay in the switch below.
	if action, ok := m.keymap.Lookup(ContextChat, msg); ok {
		return m.runChatAction(action)
	}
	switch msg.Type {
	case tea.KeyEsc:
		if m.quitConfirm {
			return m, tea.Quit
		}
		if m.completionVisible() {
			m.completionDismissedFor = m.textArea.Value()
			return m, nil
		}
		if st := m.controller.State(); st == app.ControllerStateRunning || st == app.ControllerStateAwaitingApproval {
			m.setStatus("Cancelling...", false)
			return m, m.cancelTurnCmd()
		}
		return m, nil
	case tea.KeyUp:
		if m.completionVisible() {
			m.moveCompletionCursor(-1)
			return m, nil
		}
		// The composer consumes vertical movement while the cursor can travel
		// inside a multi-line draft; the transcript scrolls only at its edge.
		if m.textArea.Line() > 0 {
			var cmd tea.Cmd
			m.textArea, cmd = m.textArea.Update(msg)
			return m, cmd
		}
		m.pauseFollowTail()
		m.viewport.LineUp(1)
		return m, nil
	case tea.KeyDown:
		if m.completionVisible() {
			m.moveCompletionCursor(1)
			return m, nil
		}
		if m.textArea.Line() < m.textArea.LineCount()-1 {
			var cmd tea.Cmd
			m.textArea, cmd = m.textArea.Update(msg)
			return m, cmd
		}
		m.viewport.LineDown(1)
		m.updateFollowTailAtBottom()
		return m, nil
	case tea.KeyTab:
		if m.completionVisible() {
			if cmd, ok := m.currentCompletion(); ok {
				m.applyCompletion(cmd)
			}
			return m, nil
		}
		var cmd tea.Cmd
		m.textArea, cmd = m.textArea.Update(msg)
		return m, cmd
	case tea.KeyPgUp:
		m.pauseFollowTail()
		m.viewport.LineUp(m.viewport.Height)
		return m, nil
	case tea.KeyPgDown:
		m.viewport.LineDown(m.viewport.Height)
		m.updateFollowTailAtBottom()
		return m, nil
	case tea.KeyEnter:
		if !msg.Alt {
			value := m.textArea.Value()
			// A partial command name is completed rather than submitted as an
			// unknown command.
			if m.completionVisible() && !m.isExecutableCommand(value) {
				if cmd, ok := m.currentCompletion(); ok {
					m.applyCompletion(cmd)
				}
				return m, nil
			}
			return m.submitUserInput(value)
		}
		// Alt+Enter falls through: the composer's InsertNewline binding
		// ("alt+enter") turns it into a newline instead of a submission.
	}

	// Any other key is composer input and cancels a pending quit confirm.
	m.quitConfirm = false
	var cmd tea.Cmd
	m.textArea, cmd = m.textArea.Update(msg)
	return m, cmd
}

func (m Model) handleMouse(msg tea.MouseMsg) (tea.Model, tea.Cmd) {
	if m.mode == ModeListing {
		// The wheel scrolls the listing dialog, not the transcript beneath.
		if msg.Action == tea.MouseActionPress {
			switch msg.Button {
			case tea.MouseButtonWheelUp:
				m.scrollListing(-mouseWheelDelta)
			case tea.MouseButtonWheelDown:
				m.scrollListing(mouseWheelDelta)
			}
		}
		return m, nil
	}
	if m.mode != ModeChat {
		return m, nil
	}
	if msg.Action == tea.MouseActionPress && msg.Button == tea.MouseButtonWheelUp {
		m.pauseFollowTail()
	}
	if msg.Action == tea.MouseActionPress && msg.Button == tea.MouseButtonLeft {
		if m.toggleReasoningAt(msg.Y) {
			m.syncTranscript()
			return m, nil
		}
		// Clicking a delegate tool block opens its read-only sub-agent view.
		if block := m.subagentAt(msg.Y); block != nil {
			return m.openSubagentOverlayFor(block)
		}
	}
	// Wheel scrolling (bubbles/viewport handled this internally; lineView
	// keeps it explicit). One notch scrolls mouseWheelDelta lines.
	if msg.Action == tea.MouseActionPress {
		switch msg.Button {
		case tea.MouseButtonWheelUp:
			m.viewport.LineUp(mouseWheelDelta)
		case tea.MouseButtonWheelDown:
			m.viewport.LineDown(mouseWheelDelta)
		}
	}
	m.updateFollowTailAtBottom()
	return m, nil
}

// blockAtRow returns the transcript block occupying the clicked screen
// row: the last block whose first line is at or above the click
// (offsets ascend along the order). Nil when the row is above the
// transcript or holds no block.
func (m *Model) blockAtRow(screenY int) *TranscriptBlock {
	// The header occupies screen row 0; the transcript starts at row 1.
	contentLine := screenY - 1 + m.viewport.YOffset
	if contentLine < 0 {
		return nil
	}
	var hit *TranscriptBlock
	for _, id := range m.blocks.Order {
		offset, ok := m.blockOffsets[id]
		if !ok || offset > contentLine {
			continue
		}
		hit = m.blocks.ByID[id]
	}
	return hit
}

// toggleReasoningAt flips the reasoning visibility of the assistant block
// under the clicked screen row — the same affordance as Claude Code's
// click-to-expand "Thought for Ns". It returns false when the row holds no
// block with hidden or shown reasoning, so the click can fall through.
func (m *Model) toggleReasoningAt(screenY int) bool {
	hit := m.blockAtRow(screenY)
	if hit == nil || hit.Kind != BlockKindAssistant || hit.StreamReasoning == "" {
		return false
	}
	hit.ReasoningExpanded = !hit.ReasoningExpanded
	return true
}

// runChatAction executes a keymap-resolved global view action.
func (m Model) runChatAction(action Action) (tea.Model, tea.Cmd) {
	switch action {
	case ActionToggleReasoning:
		m.blocks.ToggleLatestReasoning()
	case ActionToggleToolOutput:
		m.blocks.ToggleLatestToolOutput()
	case ActionToggleAllTools:
		m.blocks.ToggleAllToolOutputs()
	case ActionTogglePlan:
		m.planHidden = !m.planHidden
	case ActionSearchTranscript:
		m.enterSearch()
	case ActionViewSubagent:
		return m.openSubagentOverlay()
	case ActionCopyLastReply:
		text := m.blocks.LatestFinalAssistantText()
		if text == "" {
			m.setStatus("Nothing to copy", true)
			return m, nil
		}
		return m, copyToClipboard(text)
	case ActionPasteImage:
		return m, m.pasteClipboardImageCmd()
	case ActionJumpToBottom:
		m.resumeFollowTail()
	case ActionQueueFollowup:
		return m.queueFollowup()
	}
	return m, nil
}

// queueFollowup submits the composer draft into the next-turn queue: it
// runs as its own turn after the busy one instead of steering into it.
// Text-only, like steers; no optimistic echo — the followup panel entry
// (fed by the steer.queued event) is the pending indicator.
func (m Model) queueFollowup() (tea.Model, tea.Cmd) {
	value := strings.TrimSpace(m.textArea.Value())
	if value == "" {
		return m, nil
	}
	if strings.HasPrefix(value, "/") {
		m.setStatus("Slash commands cannot be queued as followups", true)
		return m, nil
	}
	if m.eventsDead {
		m.setStatus("Cannot submit: runtime event stream is down. Press Ctrl+C to exit.", true)
		return m, nil
	}
	m.textArea.Reset()
	m.quitConfirm = false
	// An idle followup starts its turn immediately: echo the user block
	// optimistically like a normal submit. While busy the followup panel
	// entry (fed by steer.queued) is the pending indicator instead.
	if m.controllerState == app.ControllerStateIdle {
		m.pendingSubmitID = m.blocks.AddPendingUserBlock(value)
		m.pendingSubmitPrompt = value
	}
	return m, m.submitFollowupCmd(value)
}

// submitFollowupCmd delivers a followup submission; the ack reuses
// promptSubmittedMsg (no pending echo was created, so the handler's
// block bookkeeping is a no-op).
func (m Model) submitFollowupCmd(prompt string) tea.Cmd {
	return func() tea.Msg {
		result, err := m.controller.SubmitFollowup(context.Background(), prompt)
		return promptSubmittedMsg{prompt: prompt, result: result, err: err}
	}
}

// routeFinderKey translates keys for every finder mode (docs/
// VIM_UI_DESIGN.md §6.3): arrows/ctrl+j/k/enter/esc resolve through the
// picker keymap; normal-mode runes (j/k/g/G/i/q) are hardcoded vim
// semantics. confirmed=true means the host should take f.Selected().
func routeFinderKey[T any](m Model, msg tea.KeyMsg, f *Finder[T]) (Model, bool) {
	if action, ok := m.keymap.Lookup(ContextPicker, msg); ok {
		switch action {
		case ActionCursorUp:
			f.MoveUp()
		case ActionCursorDown:
			f.MoveDown()
		case ActionConfirm:
			return m, true
		case ActionClose:
			// Esc steps out one level at a time: insert → normal → closed.
			if f.Normal() {
				m.mode = ModeChat
			} else {
				f.EnterNormal()
			}
		}
		return m, false
	}
	switch msg.Type {
	case tea.KeyRunes:
		if f.Normal() {
			switch msg.String() {
			case "k":
				f.MoveUp()
			case "j":
				f.MoveDown()
			case "g":
				f.GotoTop()
			case "G":
				f.GotoBottom()
			case "i", "a":
				f.EnterInsert()
			case "q":
				m.mode = ModeChat
			}
			return m, false
		}
		for _, r := range msg.Runes {
			f.TypeRune(r)
		}
	case tea.KeyBackspace:
		f.Backspace()
	case tea.KeyCtrlC:
		m.mode = ModeChat
	}
	return m, false
}

// handleQuestionKey routes keys while the ask_user overlay is active. The
// free-text row captures printable input (including j/k and space); every
// other row uses the shared picker navigation.
func (m Model) handleQuestionKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.pendingQuestion == nil || m.choiceList == nil {
		m.mode = ModeChat
		return m, nil
	}
	if !m.questionShownAt.IsZero() && time.Since(m.questionShownAt) < approvalDecisionGuard {
		switch msg.Type {
		case tea.KeyEnter, tea.KeyRunes, tea.KeyEsc, tea.KeyCtrlC, tea.KeySpace:
			return m, nil
		}
	}
	// The free-text row has a light insert mode: printable keys navigate
	// until i/Enter starts editing, so j/k never traps the user.
	switch msg.Type {
	case tea.KeyUp:
		m.choiceList.MoveUp()
	case tea.KeyDown:
		m.choiceList.MoveDown()
	case tea.KeyRunes:
		if m.choiceList.onOtherRow() {
			if m.choiceList.Editing() {
				for _, r := range msg.String() {
					m.choiceList.TypeRune(r)
				}
				return m, nil
			}
			switch msg.String() {
			case "k":
				m.choiceList.MoveUp()
			case "j":
				m.choiceList.MoveDown()
			case "i":
				m.choiceList.BeginEdit()
			}
			return m, nil
		}
		switch msg.String() {
		case "k":
			m.choiceList.MoveUp()
		case "j":
			m.choiceList.MoveDown()
		}
	case tea.KeySpace:
		if m.choiceList.onOtherRow() {
			if m.choiceList.Editing() {
				m.choiceList.TypeRune(' ')
			}
		} else {
			m.choiceList.Toggle()
		}
	case tea.KeyBackspace:
		if m.choiceList.Editing() {
			m.choiceList.Backspace()
		}
	case tea.KeyEnter:
		if m.choiceList.onOtherRow() && !m.choiceList.Editing() {
			m.choiceList.BeginEdit()
			return m, nil
		}
		answer, ok := m.choiceList.Confirm()
		if !ok {
			m.setStatus("Select at least one option, or type a custom answer", true)
			return m, nil
		}
		return m.submitQuestionAnswer(answer)
	case tea.KeyEsc:
		if m.choiceList.onOtherRow() && m.choiceList.Editing() {
			m.choiceList.EndEdit()
			return m, nil
		}
		return m.submitQuestionAnswer(domain.QuestionAnswer{Skipped: true})
	case tea.KeyCtrlC:
		payload := m.pendingQuestion
		m.pendingQuestion = nil
		m.choiceList = nil
		m.mode = ModeChat
		m.setStatus("Skipping question and cancelling turn...", false)
		return m, tea.Batch(m.answerQuestionCmd(payload.QuestionID, domain.QuestionAnswer{Skipped: true}), m.cancelTurnCmd())
	}
	return m, nil
}

func (m Model) submitQuestionAnswer(answer domain.QuestionAnswer) (tea.Model, tea.Cmd) {
	payload := m.pendingQuestion
	m.pendingQuestion = nil
	m.choiceList = nil
	m.mode = ModeChat
	if answer.Skipped {
		m.setStatus("Question skipped — the model will proceed with its best judgment", false)
	} else {
		m.setStatus("Answer sent", false)
	}
	return m, m.answerQuestionCmd(payload.QuestionID, answer)
}

func (m Model) answerQuestionCmd(id domain.EventID, answer domain.QuestionAnswer) tea.Cmd {
	return func() tea.Msg {
		_, err := m.controller.AnswerQuestion(context.Background(), id, answer)
		return questionAnsweredMsg{err: err}
	}
}

func (m Model) handleSessionFinderKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.sessionFinder == nil {
		m.mode = ModeChat
		return m, nil
	}
	m2, confirmed := routeFinderKey(m, msg, m.sessionFinder)
	if !confirmed {
		return m2, nil
	}
	sel := m2.sessionFinder.Selected()
	if sel == nil || sel.ID.IsZero() {
		return m2, nil
	}
	sessionID := sel.ID
	m2.setStatus("Resuming session...", false)
	return m2, m2.sessionCmd(sessionAction{
		name:    "Resume",
		success: "Session resumed",
		run:     func(ctx context.Context) error { return m2.controller.ResumeSession(ctx, sessionID) },
	})
}

func (m Model) handleModelFinderKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.modelFinder == nil {
		m.mode = ModeChat
		return m, nil
	}
	m2, confirmed := routeFinderKey(m, msg, m.modelFinder)
	if !confirmed {
		return m2, nil
	}
	opt := m2.modelFinder.Selected()
	if opt == nil {
		return m2, nil
	}
	m2.mode = ModeChat
	if opt.Ref() == m2.modelName {
		m2.setStatus(fmt.Sprintf("Model unchanged: %s", opt.Ref()), false)
		return m2, nil
	}
	m2.setStatus("Switching model...", false)
	return m2, m2.setModelCmd(opt.Ref(), "/model "+opt.Ref())
}

// currentReasoningArg reports the dial the /reasoning picker marks as
// active: the session override when set, "default" otherwise.
func (m Model) currentReasoningArg() string {
	if m.reasoningOverridden && m.reasoningEffort != "" {
		return m.reasoningEffort
	}
	return "default"
}

func (m Model) handleReasoningFinderKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.reasoningFinder == nil {
		m.mode = ModeChat
		return m, nil
	}
	m2, confirmed := routeFinderKey(m, msg, m.reasoningFinder)
	if !confirmed {
		return m2, nil
	}
	level := m2.reasoningFinder.Selected()
	if level == nil {
		return m2, nil
	}
	m2.mode = ModeChat
	if level.Arg == m2.currentReasoningArg() {
		m2.setStatus(fmt.Sprintf("Reasoning unchanged: %s", reasoningStatusText(m2.reasoningEffort, m2.reasoningOverridden)), false)
		return m2, nil
	}
	m2.setStatus("Updating reasoning...", false)
	return m2, m2.setReasoningCmd(level.Arg, "/reasoning "+level.Arg)
}

func (m Model) handleRulesFinderKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.rulesFinder == nil {
		m.mode = ModeChat
		return m, nil
	}
	// Delete confirmation sub-mode: only y/n/Esc are accepted.
	if m.rulesDeletePending != nil {
		switch msg.Type {
		case tea.KeyRunes:
			switch msg.String() {
			case "y", "Y":
				entry := *m.rulesDeletePending
				return m, m.forgetRuleCmd(entry)
			case "n", "N":
				m.rulesDeletePending = nil
				return m, nil
			}
		case tea.KeyEsc:
			m.rulesDeletePending = nil
			return m, nil
		}
		return m, nil
	}
	// Paging works in both insert and normal mode; routeFinderKey does
	// not handle PgUp/PgDn, so intercept them here.
	switch msg.Type {
	case tea.KeyPgUp:
		m.rulesFinder.PageUp(m.rulesBodyHeight())
		return m, nil
	case tea.KeyPgDown:
		m.rulesFinder.PageDown(m.rulesBodyHeight())
		return m, nil
	}
	// Delete is normal-mode only so typing 'd' into the filter works.
	if m.rulesFinder.Normal() && msg.Type == tea.KeyRunes && msg.String() == "d" {
		sel := m.rulesFinder.Selected()
		if sel == nil || !sel.Deletable {
			return m, nil
		}
		entry := *sel
		m.rulesDeletePending = &entry
		return m, nil
	}
	m2, confirmed := routeFinderKey(m, msg, m.rulesFinder)
	if confirmed {
		// Enter on a rules entry just closes the picker (no action).
		m2.mode = ModeChat
		return m2, nil
	}
	return m2, nil
}

// rulesBodyHeight estimates the list body height for page navigation.
func (m Model) rulesBodyHeight() int {
	_, h := m.rulesFinderDimensions()
	return max(h-4, 1)
}

// rulesFinderDimensions returns the inner content (width, height) of the
// rules picker float. The frame formula matches pickerFloat; the inner
// size subtracts the DialogBorder inset (4 horizontal, 2 vertical).
func (m Model) rulesFinderDimensions() (int, int) {
	w, h := m.width, m.height
	fw := min(max(w*4/5, 62), 110)
	if fw > w-2 {
		fw = max(w-2, 20)
	}
	fh := min(max(h*3/5, 10), 26)
	return fw - 4, fh - 2 // DialogBorder: border×2 + horizontal padding×2
}

// --- follow-tail helpers ---

// followTailSnapLines bounds the "near bottom" zone: when the viewport is
// within this many lines of the transcript tail, an incoming block pulls the
// view back to the bottom instead of counting an unseen event.
const followTailSnapLines = 3

func (m *Model) pauseFollowTail() {
	m.followTail = false
}

func (m *Model) updateFollowTailAtBottom() {
	if m.viewport.AtBottom() {
		m.followTail = true
		m.newEvents = 0
	}
}

// nearBottom reports whether the viewport is within followTailSnapLines of
// the transcript tail, computed against the currently rendered content.
func (m *Model) nearBottom() bool {
	maxOffset := max(0, m.viewport.TotalLineCount()-m.viewport.Height)
	return maxOffset-m.viewport.YOffset <= followTailSnapLines
}

func (m *Model) resumeFollowTail() {
	m.followTail = true
	m.newEvents = 0
	m.viewport.GotoBottom()
}

// --- composer submission and slash commands ---

func (m Model) submitUserInput(raw string) (tea.Model, tea.Cmd) {
	value := strings.TrimSpace(raw)
	if value == "" && len(m.attachedImages) == 0 {
		return m, nil
	}
	if strings.HasPrefix(value, "/") {
		return m.handleSlashCommand(value)
	}
	if m.eventsDead {
		m.setStatus("Cannot submit: runtime event stream is down. Press Ctrl+C to exit.", true)
		return m, nil
	}
	// A previous submission is still waiting on image loads. Submitting
	// again now would reset the pending counters while stale load results
	// are still arriving, causing duplicate attachments and premature
	// submission — ignore further submissions until the loads finish.
	if m.pendingSubmitAttachTotal > 0 {
		m.setStatus(fmt.Sprintf("Images still loading (%d/%d), please wait", m.pendingSubmitAttachDone, m.pendingSubmitAttachTotal), true)
		return m, nil
	}

	// Scan the input text for local image file paths and load them
	// asynchronously. Already-attached images are merged in (deduped by path).
	var attachCmds []tea.Cmd
	if value != "" {
		for _, candidate := range extractImagePaths(value) {
			// Skip paths that are already attached (dedup by path).
			dup := false
			for _, p := range m.attachedPaths {
				if p == candidate {
					dup = true
					break
				}
			}
			if !dup {
				attachCmds = append(attachCmds, m.attachImageCmd(candidate))
			}
		}
	}
	// If there are images to load, defer submission until loading finishes;
	// otherwise submit immediately with whatever is already attached.
	if len(attachCmds) > 0 {
		m.pendingSubmitDraft = raw
		m.pendingSubmitAttachDone = 0
		m.pendingSubmitAttachTotal = len(attachCmds)
		// Kick off all loads in parallel.
		return m, tea.Batch(attachCmds...)
	}

	return m.finishSubmitUserInput(raw)
}

// finishSubmitUserInput is the tail of submitUserInput, called once all
// image loading is complete (or immediately if no images needed loading).
func (m Model) finishSubmitUserInput(raw string) (tea.Model, tea.Cmd) {
	// Capture attached images for this submission, then clear.
	images := m.attachedImages
	m.attachedImages = nil
	m.attachedPaths = nil
	// Image-only submissions (empty text) get a placeholder so the
	// optimistic echo and the prompt don't render as an empty bubble.
	display := raw
	if display == "" {
		display = "[image]"
	}
	m.pendingSubmitID = m.blocks.AddPendingUserBlock(display)
	m.pendingSubmitPrompt = raw
	m.resumeFollowTail()
	m.textArea.Reset()
	m.setStatus("Prompt submitted", false)
	m.setActivity("Waiting for the model")
	m.quitConfirm = false
	return m, m.submitPromptWithImagesCmd(raw, images)
}

// imageAttachedMsg reports the result of loading an image from a file path.
type imageAttachedMsg struct {
	path    string
	content domain.ImageContent
	err     error
}

// maxImageLoadBytes bounds a single image attachment at load time, whether
// it comes from a file path or the system clipboard.
const maxImageLoadBytes = 20 * 1024 * 1024 // 20 MiB

// attachImageCmd loads an image file from the given path asynchronously.
func (m Model) attachImageCmd(path string) tea.Cmd {
	return func() tea.Msg {
		content, err := domain.LoadImageFromPath(path, maxImageLoadBytes)
		return imageAttachedMsg{path: path, content: content, err: err}
	}
}

// maxImageAttachments bounds the number of images that can be attached to a
// single prompt to avoid blowing up the context size.
const maxImageAttachments = 5

// handleImageAttached processes the result of an asynchronous image load.
// On success, the image is appended to m.attachedImages; on failure, a
// warning is posted to the status line. When all pending loads complete,
// the deferred prompt submission fires.
func (m Model) handleImageAttached(msg imageAttachedMsg) (tea.Model, tea.Cmd) {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Failed to load image %s: %v", filepath.Base(msg.path), msg.err), true)
	} else {
		m.attachedImages = append(m.attachedImages, msg.content)
		m.attachedPaths = append(m.attachedPaths, msg.path)
		m.setStatus(fmt.Sprintf("Attached %s (%d/%d)", filepath.Base(msg.path), len(m.attachedImages), m.pendingSubmitAttachTotal), false)
	}
	m.pendingSubmitAttachDone++

	// All loads finished — submit the prompt with whatever images loaded
	// successfully.
	if m.pendingSubmitAttachDone >= m.pendingSubmitAttachTotal && m.pendingSubmitDraft != "" {
		draft := m.pendingSubmitDraft
		m.pendingSubmitDraft = ""
		m.pendingSubmitAttachDone = 0
		m.pendingSubmitAttachTotal = 0
		return m.finishSubmitUserInput(draft)
	}
	return m, nil
}

// extractImagePaths scans text for tokens that look like local file paths
// with a supported image extension. Tokens follow shell-like quoting and
// backslash escaping (dragging a file into a terminal inserts an escaped
// absolute path), and a leading ~/ expands to the user's home directory.
// Only absolute paths to existing files are returned; relative paths are
// skipped to avoid false positives on prose.
func extractImagePaths(text string) []string {
	var out []string
	for _, candidate := range splitShellFields(text) {
		// Strip surrounding prose punctuation (quotes are consumed by the
		// splitter, but a trailing ". from an unmatched pair may remain).
		candidate = strings.Trim(candidate, `"'.,;:!?)]}([{`)
		if candidate == "" {
			continue
		}
		if strings.HasPrefix(candidate, "~/") {
			if home, err := os.UserHomeDir(); err == nil {
				candidate = filepath.Join(home, candidate[2:])
			}
		}
		if !domain.IsImageExtension(candidate) {
			continue
		}
		if !filepath.IsAbs(candidate) {
			continue // only absolute paths are auto-detected to avoid false positives
		}
		if _, err := os.Stat(candidate); err != nil {
			continue
		}
		out = append(out, candidate)
		if len(out) >= maxImageAttachments {
			break
		}
	}
	return out
}

// splitShellFields splits text into whitespace-separated fields while
// honoring single/double quotes and backslash escapes — the escaping a
// terminal applies when a file path is dragged into it. Quotes are stripped
// and escaped characters are unescaped. An unterminated quote is treated
// literally from the opening quote on, so prose like "don't" survives.
func splitShellFields(text string) []string {
	var fields []string
	var cur strings.Builder
	has := false
	i := 0
	for i < len(text) {
		c := text[i]
		switch {
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			if has {
				fields = append(fields, cur.String())
				cur.Reset()
				has = false
			}
			i++
		case c == '\\' && i+1 < len(text):
			cur.WriteByte(text[i+1])
			has = true
			i += 2
		case c == '\'' || c == '"':
			end := strings.IndexByte(text[i+1:], c)
			if end < 0 {
				cur.WriteByte(c) // unterminated: literal
				i++
			} else {
				cur.WriteString(text[i+1 : i+1+end])
				i += end + 2
				has = true
			}
		default:
			cur.WriteByte(c)
			has = true
			i++
		}
	}
	if has {
		fields = append(fields, cur.String())
	}
	return fields
}

func (m Model) handlePromptSubmitted(msg promptSubmittedMsg) tea.Model {
	if msg.err == nil {
		if msg.result.Followup {
			m.setStatus(fmt.Sprintf("Queued as followup (%d pending) — runs as its own turn after this one", msg.result.QueueLen), false)
			m.pendingSubmitID, m.pendingSubmitPrompt = "", ""
			return m
		}
		if msg.result.Steered {
			// The optimistic echo must not become a transcript user block:
			// the message lives in the steer panel until the loop injects it
			// (steer.queued fills the panel, steer.injected emits the block).
			if m.pendingSubmitID != "" {
				m.blocks.Remove(m.pendingSubmitID)
			}
			status := fmt.Sprintf("Queued (%d pending) — injects before the next model call; Ctrl+C flushes now", msg.result.QueueLen)
			if msg.imageCount > 0 {
				// Steer messages are text-only; the controller drops image
				// attachments when queueing. Warn instead of losing them silently.
				status += fmt.Sprintf(" — %d image%s dropped (steer is text-only)", msg.imageCount, pluralS(msg.imageCount))
			}
			m.setStatus(status, msg.imageCount > 0)
		}
		m.pendingSubmitID, m.pendingSubmitPrompt = "", ""
		return m
	}
	// The controller rejected the prompt: drop the optimistic echo and hand
	// the draft back to the composer.
	if m.pendingSubmitID != "" {
		m.blocks.Remove(m.pendingSubmitID)
	}
	m.textArea.SetValue(msg.prompt)
	m.pendingSubmitID, m.pendingSubmitPrompt = "", ""
	m.setStatus(fmt.Sprintf("Submit failed: %v (draft restored)", msg.err), true)
	m.setActivity("Ready")
	return m
}

func (m Model) handleSlashCommand(cmd string) (tea.Model, tea.Cmd) {
	fields := strings.Fields(cmd)
	if len(fields) == 0 {
		return m, nil
	}
	switch fields[0] {
	case "/help":
		m.textArea.Reset()
		m.mode = ModeHelp
	case "/new":
		m.textArea.Reset()
		m.setStatus("Creating session...", false)
		return m, m.sessionCmd(sessionAction{
			name:    "New session",
			command: cmd,
			success: "New session created",
			run:     m.controller.NewSession,
		})
	case "/sessions":
		m.textArea.Reset()
		m.mode = ModeSessionPicker
		m.sessionFinder = m.NewSessionFinder()
		return m, m.requestSessions()
	case "/resume":
		if len(fields) != 2 {
			m.setStatus("Usage: /resume <session-id>", true)
			return m, nil
		}
		sessionID, err := domain.ParseSessionID(fields[1])
		if err != nil {
			m.setStatus(fmt.Sprintf("Invalid session ID: %v", err), true)
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Resuming session...", false)
		return m, m.sessionCmd(sessionAction{
			name:    "Resume",
			command: cmd,
			success: "Session resumed",
			run:     func(ctx context.Context) error { return m.controller.ResumeSession(ctx, sessionID) },
		})
	case "/clear":
		m.textArea.Reset()
		m.blocks = NewBlockIndex()
		m.setStatus("Transcript cleared (session history retained)", false)
	case "/model":
		if len(fields) > 2 {
			m.setStatus("Usage: /model [provider/]<model>", true)
			return m, nil
		}
		if len(fields) == 1 {
			m.textArea.Reset()
			// With a known catalog, bare /model opens the picker (the active
			// entry is marked, so it doubles as "show current"); without one
			// (e.g. a bare test harness) fall back to the status line.
			if len(m.models) > 0 {
				m.modelFinder = m.NewModelFinder(m.models, m.modelName)
				m.mode = ModeModelPicker
				return m, nil
			}
			m.setStatus(fmt.Sprintf("Current model: %s", m.modelName), false)
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Switching model...", false)
		return m, m.setModelCmd(fields[1], cmd)
	case "/reasoning":
		if len(fields) > 2 {
			m.setStatus("Usage: /reasoning [off|low|medium|high|default]", true)
			return m, nil
		}
		if len(fields) == 1 {
			// Bare /reasoning opens the picker with the cursor on the active
			// dial (the override level, or "default" when following config).
			m.textArea.Reset()
			m.reasoningFinder = m.NewReasoningFinder(m.reasoningEffort, m.reasoningOverridden)
			m.mode = ModeReasoningPicker
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Updating reasoning...", false)
		return m, m.setReasoningCmd(fields[1], cmd)
	case "/exit":
		return m, tea.Quit
	case "/agent":
		if len(fields) != 1 {
			m.setStatus("Usage: /agent", true)
			return m, nil
		}
		m.textArea.Reset()
		return m.openSubagentOverlay()
	case "/compact":
		if len(fields) != 1 {
			m.setStatus("Usage: /compact", true)
			return m, nil
		}
		m.textArea.Reset()
		return m, m.compactCmd()
	case "/rewind":
		if len(fields) > 2 {
			m.setStatus("Usage: /rewind [checkpoint-sequence]", true)
			return m, nil
		}
		m.textArea.Reset()
		if len(fields) == 1 {
			m.setStatus("Loading checkpoints...", false)
			return m, m.listCheckpointsCmd()
		}
		sequence, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil || sequence <= 0 {
			m.setStatus(fmt.Sprintf("Invalid checkpoint sequence: %s", fields[1]), true)
			return m, nil
		}
		m.setStatus(fmt.Sprintf("Rewinding to checkpoint #%d...", sequence), false)
		return m, m.rewindCmd(sequence, cmd)
	case "/skill":
		if len(fields) != 1 {
			m.setStatus("Usage: /skill", true)
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Loading skills...", false)
		return m, m.listSkillsCmd()
	case "/doctor":
		if len(fields) != 1 {
			m.setStatus("Usage: /doctor", true)
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Loading environment report...", false)
		return m, m.envReportCmd()
	case "/mcp":
		if len(fields) != 1 {
			m.setStatus("Usage: /mcp", true)
			return m, nil
		}
		m.textArea.Reset()
		m.setStatus("Loading MCP servers...", false)
		return m, m.listMCPServersCmd()
	case "/rules":
		if len(fields) != 1 {
			m.setStatus("Usage: /rules", true)
			return m, nil
		}
		m.textArea.Reset()
		m.mode = ModeRules
		m.rulesFinder = nil
		m.rulesDeletePending = nil
		return m, m.requestRules()
	default:
		m.setStatus(fmt.Sprintf("Unknown command: %s", fields[0]), true)
	}
	return m, nil
}

func (m Model) handleSessionSwitched(msg sessionSwitchedMsg) (tea.Model, tea.Cmd) {
	if msg.err != nil {
		if msg.action.command != "" {
			m.textArea.SetValue(msg.action.command)
		}
		m.setStatus(fmt.Sprintf("%s failed: %v", msg.action.name, msg.err), true)
		return m, nil
	}
	m.blocks = NewBlockIndex()
	m.sessionID = m.controller.SessionID()
	m.usage = domain.Usage{}
	m.pendingApproval = nil
	m.pendingQuestion = nil
	m.choiceList = nil
	m.pendingSteers = nil
	m.pendingFollowups = nil
	m.subOverlay = nil
	m.mode = ModeChat
	// The fresh subscription belongs to a fresh session: a dead-stream
	// lockout or a spent resubscribe budget from the previous session must
	// not carry over, or prompt submission would stay blocked even though
	// the new stream is healthy.
	m.resubscribes = 0
	m.eventsDead = false
	m.setStatus(msg.action.success, false)
	// The subscription is bound to a session: after a switch the old stream
	// only carries the previous session's events, so re-attach to the new
	// session. The cursor stays at the last applied sequence (global
	// sequence space), so nothing is replayed and nothing is missed. The
	// cancelled old subscription's waitForEvent is still in flight; its
	// close report arrives with a stale generation and is ignored.
	if m.unsubscribeEvents != nil {
		m.unsubscribeEvents()
	}
	eventsCh, unsubscribe := subscribeEvents(m.controller, m.lastEventSeq)
	m.adoptEvents(eventsCh, unsubscribe)
	return m, tea.Batch(m.waitForEvent(), m.requestSnapshot())
}

// handleReasoningChanged applies the ack of a /reasoning command: on
// success the header picks up the new dial immediately; on failure the
// draft is restored so the user can fix a mistyped level.
func (m Model) handleReasoningChanged(msg reasoningChangedMsg) tea.Model {
	if msg.err != nil {
		m.textArea.SetValue(msg.command)
		m.setStatus(fmt.Sprintf("Set reasoning failed: %v", msg.err), true)
		return m
	}
	m.reasoningOverridden = msg.result.Overridden
	m.reasoningEffort = msg.result.Effective.Label()
	m.setStatus(fmt.Sprintf("Reasoning: %s", reasoningStatusText(m.reasoningEffort, m.reasoningOverridden)), false)
	return m
}

func (m Model) setReasoningCmd(arg, command string) tea.Cmd {
	return func() tea.Msg {
		result, err := m.controller.SetReasoning(context.Background(), arg)
		return reasoningChangedMsg{command: command, result: result, err: err}
	}
}

// handleCompactRequested applies the ack of a /compact request: the pass
// itself runs inside the next model call and is reported through the
// ContextCompacted event; this message only confirms the scheduling.
func (m Model) handleCompactRequested(msg compactRequestedMsg) tea.Model {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Compact failed: %v", msg.err), true)
		return m
	}
	if msg.result.AlreadyPending {
		m.setStatus("Compaction already scheduled for the next model call", false)
		return m
	}
	detail := ""
	if m.contextOccupancy > 0 {
		detail = fmt.Sprintf(" (current context ≈ %s)", humanizeTokens(m.contextOccupancy))
	}
	m.setStatus(fmt.Sprintf("Will compact before the next model call%s", detail), false)
	return m
}

func (m Model) compactCmd() tea.Cmd {
	return func() tea.Msg {
		result, err := m.controller.RequestCompaction(context.Background())
		return compactRequestedMsg{result: result, err: err}
	}
}

// handleCheckpointsListed renders the bare /rewind listing as a compact
// status summary: the most recent checkpoints with their labels, plus
// the invocation hint. The full history stays in the session store; the
// status line is a picker aid, not a pager.
func (m Model) handleCheckpointsListed(msg checkpointsListedMsg) tea.Model {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("List checkpoints failed: %v", msg.err), true)
		return m
	}
	if len(msg.checkpoints) == 0 {
		m.setStatus("No checkpoints yet — checkpoints appear after the first turn", false)
		return m
	}
	const maxShown = 3
	var b strings.Builder
	b.WriteString("Checkpoints: ")
	for i, cp := range msg.checkpoints {
		if i >= maxShown {
			fmt.Fprintf(&b, " · (+%d more)", len(msg.checkpoints)-maxShown)
			break
		}
		if i > 0 {
			b.WriteString(" · ")
		}
		label := cp.Label
		if label == "" {
			label = "(no user message)"
		}
		fmt.Fprintf(&b, "#%d %q", cp.Sequence, label)
	}
	b.WriteString(" — /rewind <seq> to restore")
	m.setStatus(b.String(), false)
	return m
}

func (m Model) listCheckpointsCmd() tea.Cmd {
	return func() tea.Msg {
		checkpoints, err := m.controller.ListCheckpoints(context.Background(), 50)
		return checkpointsListedMsg{checkpoints: checkpoints, err: err}
	}
}

// handleRewindFinished applies the ack of a /rewind <seq>: on success the
// transcript view resets (the truncated-away turns are gone) and the
// restoration breakdown lands in the status line; on failure the draft
// is restored so the user can fix a mistyped sequence.
func (m Model) handleRewindFinished(msg rewindFinishedMsg) (tea.Model, tea.Cmd) {
	if msg.err != nil {
		m.textArea.SetValue(msg.command)
		m.setStatus(fmt.Sprintf("Rewind failed: %v", msg.err), true)
		return m, nil
	}
	m.blocks = NewBlockIndex()
	m.usage = domain.Usage{}
	m.pendingApproval = nil
	m.pendingQuestion = nil
	m.choiceList = nil
	m.pendingSteers = nil
	m.pendingFollowups = nil
	m.subOverlay = nil
	m.mode = ModeChat
	out := msg.outcome
	status := fmt.Sprintf("Rewound to checkpoint #%d: %d restored, %d deleted",
		out.Checkpoint.Sequence, len(out.Restored), len(out.Deleted))
	if len(out.Conflicts) > 0 {
		status += fmt.Sprintf(", %d conflicts", len(out.Conflicts))
	}
	if len(out.Skipped) > 0 {
		status += fmt.Sprintf(", %d unrestorable", len(out.Skipped))
	}
	m.setStatus(status, len(out.Conflicts) > 0 || len(out.Skipped) > 0)
	return m, m.requestSnapshot()
}

func (m Model) rewindCmd(sequence int64, command string) tea.Cmd {
	return func() tea.Msg {
		outcome, err := m.controller.Rewind(context.Background(), sequence)
		return rewindFinishedMsg{command: command, outcome: outcome, err: err}
	}
}

func (m Model) listSkillsCmd() tea.Cmd {
	return func() tea.Msg {
		listing, err := m.controller.ListSkills(context.Background())
		return skillsLoadedMsg{listing: listing, err: err}
	}
}

func (m Model) listMCPServersCmd() tea.Cmd {
	return func() tea.Msg {
		servers, err := m.controller.ListMCPServers(context.Background())
		return mcpServersLoadedMsg{servers: servers, err: err}
	}
}

func (m Model) handleSkillsLoaded(msg skillsLoadedMsg) tea.Model {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Failed to load skills: %v", msg.err), true)
		return m
	}
	return m.openListing(listingContent{
		kind:   listingSkills,
		title:  fmt.Sprintf("Skills (%d)", len(msg.listing.Skills)),
		skills: msg.listing,
	})
}

func (m Model) handleMCPServersLoaded(msg mcpServersLoadedMsg) tea.Model {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Failed to load MCP servers: %v", msg.err), true)
		return m
	}
	return m.openListing(listingContent{
		kind:    listingMCP,
		title:   fmt.Sprintf("MCP Servers (%d)", len(msg.servers)),
		servers: msg.servers,
	})
}

func (m Model) envReportCmd() tea.Cmd {
	return func() tea.Msg {
		report, err := m.controller.ToolchainEnvironment(context.Background())
		return envLoadedMsg{report: report, err: err}
	}
}

func (m Model) handleEnvLoaded(msg envLoadedMsg) tea.Model {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Failed to load environment report: %v", msg.err), true)
		return m
	}
	return m.openListing(listingContent{
		kind:  listingEnv,
		title: "Environment",
		env:   msg.report,
	})
}

// openListing switches to the read-only listing dialog (ModeListing),
// resetting the scroll to the top.
func (m Model) openListing(c listingContent) tea.Model {
	m.listing = c
	m.listingScroll = 0
	m.mode = ModeListing
	m.setStatus("", false)
	return m
}

// handleListingKey routes keys while the read-only listing dialog is
// active: navigation scrolls, esc/q/enter closes.
func (m Model) handleListingKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	maxScroll := max(0, m.listingContentRows()-m.listingVisibleRows())
	switch msg.Type {
	case tea.KeyEsc, tea.KeyEnter, tea.KeyCtrlC:
		m.mode = ModeChat
		return m, nil
	case tea.KeyUp:
		m.listingScroll = max(0, m.listingScroll-1)
	case tea.KeyDown:
		m.listingScroll = min(maxScroll, m.listingScroll+1)
	case tea.KeyPgUp:
		m.listingScroll = max(0, m.listingScroll-m.listingVisibleRows())
	case tea.KeyPgDown:
		m.listingScroll = min(maxScroll, m.listingScroll+m.listingVisibleRows())
	case tea.KeyHome:
		m.listingScroll = 0
	case tea.KeyEnd:
		m.listingScroll = maxScroll
	case tea.KeyRunes:
		switch msg.String() {
		case "q":
			m.mode = ModeChat
			return m, nil
		case "k":
			m.listingScroll = max(0, m.listingScroll-1)
		case "j":
			m.listingScroll = min(maxScroll, m.listingScroll+1)
		case "g":
			m.listingScroll = 0
		case "G":
			m.listingScroll = maxScroll
		}
	}
	return m, nil
}

// scrollListing adjusts listingScroll by delta rows, clamped to [0, max].
func (m *Model) scrollListing(delta int) {
	maxScroll := max(0, m.listingContentRows()-m.listingVisibleRows())
	m.listingScroll = max(0, min(maxScroll, m.listingScroll+delta))
}

func pluralS(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

// reasoningStatusText renders the /reasoning ack for the status line.
func reasoningStatusText(effort string, overridden bool) string {
	if effort == "" {
		effort = "provider default"
	}
	if overridden {
		return effort + " (session override)"
	}
	return effort + " (model config)"
}

// handleModelChanged applies the ack of a /model switch: on success the
// status bar picks up the new provider/model and the ctx denominator from
// the model's metadata immediately; on failure the draft is restored so
// the user can fix a mistyped reference.
func (m Model) handleModelChanged(msg modelChangedMsg) tea.Model {
	if msg.err != nil {
		m.textArea.SetValue(msg.command)
		m.setStatus(fmt.Sprintf("Switch model failed: %v", msg.err), true)
		return m
	}
	m.modelName = msg.result.Cur.String()
	// Re-base the ctx denominator on the server-derived effective window
	// (nil = the model declares no usable window → hide the denominator).
	m.contextWindow = 0
	if msg.result.Window != nil {
		m.contextWindow = int(msg.result.Window.Effective)
	}
	// A turn already in flight keeps the model it started on; make that
	// visible instead of letting the user expect an immediate swap.
	note := ""
	if m.isBusy() {
		note = " (applies from next turn)"
	}
	if msg.result.Prev.Provider == "" || msg.result.Prev == msg.result.Cur {
		m.setStatus(fmt.Sprintf("Model set to %s%s", msg.result.Cur.String(), note), false)
	} else {
		m.setStatus(fmt.Sprintf("Switched model: %s → %s%s", msg.result.Prev.String(), msg.result.Cur.String(), note), false)
	}
	return m
}

func (m Model) setModelCmd(ref, command string) tea.Cmd {
	return func() tea.Msg {
		result, err := m.controller.SetModel(context.Background(), ref)
		return modelChangedMsg{command: command, result: result, err: err}
	}
}

// --- interrupt and exit ---

func (m Model) handleCtrlC() (tea.Model, tea.Cmd) {
	now := time.Now()
	state := m.controller.State()
	switch state {
	case app.ControllerStateRunning, app.ControllerStateAwaitingApproval:
		if !m.lastCancelTime.IsZero() && now.Sub(m.lastCancelTime) < 2*time.Second {
			return m, tea.Quit
		}
		m.lastCancelTime = now
		m.setStatus("Cancelling turn... (Ctrl+C again within 2s to force quit)", false)
		return m, m.cancelTurnCmd()
	case app.ControllerStateIdle:
		if m.textArea.Value() != "" {
			m.textArea.Reset()
			m.quitConfirm = false
			m.setStatus("Input cleared", false)
		} else if m.quitConfirm {
			return m, tea.Quit
		} else {
			m.quitConfirm = true
			m.lastCancelTime = now
			m.setStatus("Press Ctrl+C again or Ctrl+D to exit", false)
		}
	case app.ControllerStateCancelling:
		if now.Sub(m.lastCancelTime) < 2*time.Second {
			return m, tea.Quit
		}
		m.lastCancelTime = now
		m.setStatus("Still cancelling... (Ctrl+C again to force quit)", false)
	default:
		// booting / fatal / closed: no turn to cancel and input may be locked,
		// so the only sensible action is to leave.
		return m, tea.Quit
	}
	return m, nil
}

func (m Model) handleCtrlD() (tea.Model, tea.Cmd) {
	switch m.controller.State() {
	case app.ControllerStateIdle, app.ControllerStateFatal, app.ControllerStateClosed:
		return m, tea.Quit
	}
	m.setStatus("A turn is active; use Ctrl+C to cancel it", false)
	return m, nil
}

// --- approval ---

// approval cursor positions: 0 = allow once, 1 = allow always (persist a
// categorical rule with its derived minimal grant), 2 = always trust
// unsandboxed (escalated run_cmd calls only), last = deny.

// approvalDecisionGuard is the window right after the approval overlay
// appears during which decision keys (Enter/y/a/n/Esc/Ctrl+C) are ignored.
// Rationale: back-to-back approvals are common with parallel tool calls,
// and a held key's terminal auto-repeat (typically starting ~500ms in) or
// an accidental double tap otherwise leaks from the previous overlay into
// approving a call the user never saw. Observed in a real session: a
// second web_fetch was auto-approved 527ms after its overlay appeared.
// Navigation keys stay responsive; 700ms is imperceptible for a genuine
// decision, which takes at least a second of reading.
const approvalDecisionGuard = 700 * time.Millisecond

func (m Model) handleApprovalKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.pendingApproval == nil {
		m.mode = ModeChat
		return m, nil
	}
	if !m.approvalShownAt.IsZero() && time.Since(m.approvalShownAt) < approvalDecisionGuard {
		switch msg.Type {
		case tea.KeyEnter, tea.KeyRunes, tea.KeyEsc, tea.KeyCtrlC:
			return m, nil
		}
	}
	var decision domain.Decision
	remember := false
	trust := ""
	maxCursor := m.approvalOptionCount() - 1
	switch msg.Type {
	case tea.KeyLeft, tea.KeyShiftTab, tea.KeyUp:
		if m.approvalCursor > 0 {
			m.approvalCursor--
		}
		return m, nil
	case tea.KeyRight, tea.KeyTab, tea.KeyDown:
		if m.approvalCursor < maxCursor {
			m.approvalCursor++
		}
		return m, nil
	case tea.KeyEnter:
		switch m.approvalCursor {
		case 0:
			decision = domain.DecisionAllow
		case 1:
			if !m.approvalAlwaysAvailable() {
				return m, nil
			}
			decision = domain.DecisionAllow
			remember = true
		case 2:
			if m.approvalOptionCount() == 4 {
				decision = domain.DecisionAllow
				remember = true
				trust = app.TrustUnsandboxed
			} else {
				decision = domain.DecisionDeny
			}
		default:
			decision = domain.DecisionDeny
		}
	case tea.KeyRunes:
		switch msg.String() {
		case "y", "Y", "1":
			decision = domain.DecisionAllow
		case "a", "A", "2":
			if !m.approvalAlwaysAvailable() {
				return m, nil
			}
			decision = domain.DecisionAllow
			remember = true
		case "t", "T", "3":
			if m.approvalOptionCount() != 4 {
				if msg.String() == "3" {
					decision = domain.DecisionDeny
					break
				}
				return m, nil
			}
			decision = domain.DecisionAllow
			remember = true
			trust = app.TrustUnsandboxed
		case "n", "N", "4":
			decision = domain.DecisionDeny
		case "k":
			if m.approvalCursor > 0 {
				m.approvalCursor--
			}
			return m, nil
		case "j":
			if m.approvalCursor < maxCursor {
				m.approvalCursor++
			}
			return m, nil
		default:
			return m, nil
		}
	case tea.KeyEsc:
		decision = domain.DecisionDeny
	case tea.KeyCtrlC:
		payload := m.pendingApproval
		m.pendingApproval = nil
		m.mode = ModeChat
		m.setStatus("Denying and cancelling turn...", false)
		// Always cancel the turn, even when the approval was already resolved
		// through another path and the deny comes back rejected.
		return m, tea.Batch(m.resolveApprovalCmd(payload, domain.DecisionDeny, false, ""), m.cancelTurnCmd())
	default:
		return m, nil
	}
	payload := m.pendingApproval
	m.pendingApproval = nil
	m.mode = ModeChat
	return m, m.resolveApprovalCmd(payload, decision, remember, trust)
}

// approvalAlwaysAvailable reports whether "always allow" can persist a
// rule for the pending call. It mirrors the overlay's disabled option:
// shell, heredoc, and non-run_cmd calls are per-call decisions only.
func (m Model) approvalAlwaysAvailable() bool {
	if m.pendingApproval == nil {
		return false
	}
	_, _, ok := app.ApprovalRulePreview(m.pendingApproval.ToolName, m.pendingApproval.Arguments)
	return ok
}

// approvalOptionCount reports how many rows the approval overlay offers:
// escalated run_cmd calls get a fourth "always trust (unsandboxed)" row
// between "always allow" and "deny".
func (m Model) approvalOptionCount() int {
	if m.pendingApproval != nil && app.RunCmdTrustPreview(m.pendingApproval.ToolName, m.pendingApproval.Arguments) {
		return 4
	}
	return 3
}

func approvalBinding(payload *runtimeevent.ApprovalRequestedPayload) app.ApprovalBinding {
	return app.ApprovalBinding{
		ApprovalID: payload.ApprovalID,
		CallID:     payload.CallID,
		ArgsHash:   payload.ArgsHash,
	}
}

// displayModelRef renders the status-bar model label: provider/model when
// the provider is known, the bare model otherwise.
func displayModelRef(provider, model string) string {
	if provider == "" {
		return model
	}
	return provider + "/" + model
}

// --- snapshot and event stream ---

func (m Model) handleSnapshot(msg snapshotMsg) (tea.Model, tea.Cmd) {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Load session state failed: %v", msg.err), true)
		return m, nil
	}
	sessionChanged := !m.sessionID.IsZero() && m.sessionID != msg.snapshot.SessionID
	m.controllerState = msg.snapshot.State
	m.sessionID = msg.snapshot.SessionID
	m.modelName = displayModelRef(msg.snapshot.ProviderName, msg.snapshot.ModelName)
	m.reasoningEffort = msg.snapshot.ReasoningEffort
	m.reasoningOverridden = msg.snapshot.ReasoningOverridden
	if msg.snapshot.ContextWindow > 0 {
		m.contextWindow = int(msg.snapshot.ContextWindow)
	}
	m.workspace = msg.snapshot.WorkspaceRoot
	m.pendingSteers = msg.snapshot.PendingSteers
	m.pendingFollowups = msg.snapshot.PendingFollowups
	m.usage = msg.snapshot.Usage
	if sessionChanged || (m.initialSnapshotPending && len(m.blocks.Order) == 0) {
		m.blocks = RebuildTranscript(msg.snapshot.Messages)
	} else {
		m.mergeSnapshot(msg.snapshot.Messages)
	}
	m.initialSnapshotPending = false
	// The snapshot is the authoritative source for pending approvals and
	// ask_user questions: their requested/asked events may never reach this
	// frontend (a session switch re-subscribes at the global cursor, which
	// other sessions advance, and question.asked is ephemeral), so the
	// overlays are reconciled both ways — stale ones dismissed, missed ones
	// restored. Without the restore half a session awaiting approval shows
	// no prompt at all and its run blocks forever.
	m.reconcilePendingRequests(msg.snapshot.PendingRequests)
	m.resumeFollowTail()
	return m, nil
}

// reconcilePendingRequests aligns the approval/question overlays with the
// runtime's pending requests: overlays whose request is gone are dismissed,
// overlays whose event this frontend missed (session switch, event-stream
// resync) are restored. The run loop awaits requests serially, so at most
// one request is pending at a time.
func (m *Model) reconcilePendingRequests(pending []app.PendingRequest) {
	var approval *runtimeevent.ApprovalRequestedPayload
	var question *domain.Question
	for i := range pending {
		pr := &pending[i]
		switch pr.Kind {
		case app.PendingRequestApproval:
			if approval == nil && pr.Approval != nil {
				approval = pr.Approval
			}
		case app.PendingRequestQuestion:
			if question == nil && pr.Question != nil {
				question = pr.Question
			}
		}
	}

	if m.pendingApproval != nil && (approval == nil || approval.ApprovalID != m.pendingApproval.ApprovalID) {
		m.pendingApproval = nil
		if m.mode == ModeApproval {
			m.mode = ModeChat
		}
	}
	if m.pendingQuestion != nil && (question == nil || question.ID != m.pendingQuestion.QuestionID) {
		m.pendingQuestion = nil
		m.choiceList = nil
		if m.mode == ModeQuestion {
			m.mode = ModeChat
		}
	}

	// Restore at most one overlay; an approval outranks a question.
	if m.pendingApproval == nil && m.pendingQuestion == nil {
		switch {
		case approval != nil:
			m.showApprovalOverlay(*approval)
		case question != nil:
			m.showQuestionOverlay(*question)
		}
	}
}

// showApprovalOverlay raises the approval overlay for a requested payload.
// The overlay lives at the bottom (composer area), so the viewport is
// pinned to the tail: a user who scrolled up to read earlier output would
// otherwise never see the decision prompt.
func (m *Model) showApprovalOverlay(payload runtimeevent.ApprovalRequestedPayload) {
	m.pendingApproval = &payload
	m.approvalCursor = 0
	m.approvalShownAt = time.Now()
	m.mode = ModeApproval
	m.resumeFollowTail()
}

// showQuestionOverlay raises the ask_user overlay for a pending question,
// pinning the viewport to the tail exactly like the approval overlay.
func (m *Model) showQuestionOverlay(q domain.Question) {
	items := make([]ChoiceItem, 0, len(q.Options))
	for _, opt := range q.Options {
		items = append(items, ChoiceItem{Label: opt.Label, Desc: opt.Description})
	}
	m.pendingQuestion = &runtimeevent.QuestionAskedPayload{
		QuestionID:    q.ID,
		Text:          q.Text,
		Options:       q.Options,
		AllowMultiple: q.AllowMultiple,
	}
	m.choiceList = NewChoiceList(ChoiceListConfig{
		Title:    "Model asks:\n" + q.Text,
		Items:    items,
		Multi:    q.AllowMultiple,
		OtherRow: true,
	})
	m.questionShownAt = time.Now()
	m.mode = ModeQuestion
	m.resumeFollowTail()
}

func (m *Model) mergeSnapshot(messages []domain.Message) {
	// A snapshot can race with live events. Treat it only as a source of missing
	// history: replacing the live index would erase streamed assistant text,
	// tool states, and local user echoes that have not reached the snapshot yet.
	snapshot := RebuildTranscript(messages)
	for _, blockID := range snapshot.Order {
		block := snapshot.ByID[blockID]
		if !hasEquivalentBlock(m.blocks, block) {
			m.blocks.Add(block)
		}
	}
}

func hasEquivalentBlock(idx *BlockIndex, candidate *TranscriptBlock) bool {
	for _, id := range idx.Order {
		block := idx.ByID[id]
		if block.Kind == candidate.Kind && block.Content == candidate.Content && block.Tool == candidate.Tool {
			return true
		}
	}
	return false
}

// handleEventsClosed recovers from the broker disconnecting this subscriber
// (slow consumer) by re-subscribing and refreshing from a snapshot. Input is
// locked when the runtime is gone or recovery attempts are exhausted.
func (m Model) handleEventsClosed(msg runtimeEventsClosedMsg) (tea.Model, tea.Cmd) {
	// A stale close report belongs to a subscription that has already been
	// replaced (session switch, or an earlier recovery): the live
	// waitForEvent is healthy, and "recovering" here would kill it and
	// cascade until the budget locked input.
	if msg.gen != m.eventsGen {
		return m, nil
	}
	select {
	case <-m.controller.Done():
		m.eventsDead = true
		m.setStatus("Runtime shut down; press Ctrl+C to exit.", true)
		return m, nil
	default:
	}
	if m.resubscribes >= maxEventResubscribes {
		m.eventsDead = true
		m.setStatus("Runtime event stream lost; input locked. Press Ctrl+C to exit.", true)
		return m, nil
	}
	m.resubscribes++
	// Release the dead subscription before attaching a new one; otherwise
	// every recovery leaks a broker subscriber until process exit.
	if m.unsubscribeEvents != nil {
		m.unsubscribeEvents()
	}
	eventsCh, unsubscribe := subscribeEvents(m.controller, m.lastEventSeq)
	m.adoptEvents(eventsCh, unsubscribe)
	m.setStatus("Event stream interrupted; resubscribed, refreshing view from snapshot", true)
	return m, tea.Batch(m.waitForEvent(), m.requestSnapshot())
}

func (m Model) handleRuntimeEvent(evt runtimeevent.RuntimeEvent) (Model, tea.Cmd) {
	// A delivered event proves the (re)subscription is healthy: reset the
	// consecutive-recovery budget so a long-lived session is never locked
	// out by ancient, already-recovered disconnects.
	m.resubscribes = 0
	if evt.Sequence > m.lastEventSeq {
		m.lastEventSeq = evt.Sequence
	}
	// Drop events belonging to other sessions (for example stale events from
	// before /new or /resume); adopt a session only while unbound.
	if !evt.SessionID.IsZero() {
		if m.sessionID.IsZero() {
			m.sessionID = evt.SessionID
		} else if evt.SessionID != m.sessionID {
			return m, m.waitForEvent()
		}
	}
	m.controllerState = m.controller.State()
	switch evt.Kind {
	case runtimeevent.KindTurnStarted:
		m.setActivity("Preparing turn")
		// A turn start flushes the steer panel: leftovers were relayed as
		// this turn's prompt (steer.queued never fires while idle), so any
		// remaining entries are now that prompt's user block. Surface the
		// relay so the stale "Queued" status from the steer ack is replaced.
		if n := len(m.pendingSteers); n > 0 {
			m.setStatus(fmt.Sprintf("Relayed %d queued message(s) into this turn", n), false)
		}
		m.pendingSteers = nil
		// A turn start consumes the followup queue head when it is the
		// relayed prompt (FIFO, one per turn boundary); a turn started by
		// an unrelated submission leaves the queue untouched.
		if len(m.pendingFollowups) > 0 {
			var payload runtimeevent.TurnStartedPayload
			if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.Prompt == m.pendingFollowups[0] {
				m.pendingFollowups = m.pendingFollowups[1:]
			}
		}
		// The panel shows the current turn's plan only: a new prompt starts
		// the display fresh (Claude Code clears tasks between turns). The
		// runtime plan itself is untouched — an unfinished plan is still
		// re-injected into the model's context, and the next update_plan
		// revision brings the panel right back.
		m.plan = domain.Plan{}
	case runtimeevent.KindSteerQueued:
		var payload runtimeevent.SteerQueuedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			if payload.Queue == "followup" {
				m.pendingFollowups = append(m.pendingFollowups, payload.Text)
			} else {
				m.pendingSteers = append(m.pendingSteers, payload.Text)
			}
		}
	case runtimeevent.KindSteerInjected:
		// The cell is FIFO, so injected messages leave the panel head-first;
		// the block itself is appended by ApplyRuntimeEvent below.
		if len(m.pendingSteers) > 0 {
			m.pendingSteers = m.pendingSteers[1:]
		}
	case runtimeevent.KindSessionOpened:
		var payload runtimeevent.SessionOpenedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.modelName = payload.Model
			m.workspace = payload.Workspace
		}
		// A new session view starts its compaction tally fresh.
		m.compactions = 0
		m.contextOccupancy = 0
	case runtimeevent.KindRunPhaseChanged:
		var payload runtimeevent.RunPhasePayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.phase = string(payload.Phase)
		}
	case runtimeevent.KindModelRequestStarted:
		m.phase = "model"
		m.setActivity("Waiting for the model")
	case runtimeevent.KindModelReasoningDelta:
		m.phase = "model"
		m.setActivity("Thinking")
	case runtimeevent.KindModelTextDelta:
		m.phase = "model"
		m.setActivity("Streaming response")
	case runtimeevent.KindModelToolCallDelta:
		var payload runtimeevent.ModelToolCallDeltaPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.ToolName != "" {
			m.phase = "tool"
			m.setActivity(fmt.Sprintf("Preparing tool: %s", payload.ToolName))
		}
	case runtimeevent.KindToolPrepared:
		m.phase = "tool"
		m.setActivity("Preparing tool execution")
	case runtimeevent.KindToolStarted:
		var payload runtimeevent.ToolStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.ToolName != "" {
			m.setActivity(fmt.Sprintf("Running tool: %s", payload.ToolName))
		} else {
			m.setActivity("Running tool")
		}
		m.phase = "tool"
	case runtimeevent.KindQuestionAsked:
		var payload runtimeevent.QuestionAskedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.showQuestionOverlay(domain.Question{
				ID:            payload.QuestionID,
				Text:          payload.Text,
				Options:       payload.Options,
				AllowMultiple: payload.AllowMultiple,
			})
		}
		m.phase = "question"
		m.setActivity("Waiting for your answer")
	case runtimeevent.KindQuestionAnswered:
		var payload runtimeevent.QuestionAnsweredPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			if m.pendingQuestion != nil && m.pendingQuestion.QuestionID == payload.QuestionID {
				m.pendingQuestion = nil
				m.choiceList = nil
				if m.mode == ModeQuestion {
					m.mode = ModeChat
				}
			}
		}
	case runtimeevent.KindApprovalRequested:
		var payload runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.showApprovalOverlay(payload)
		}
		m.phase = "approval"
		m.setActivity("Waiting for your approval")
	case runtimeevent.KindApprovalResolved:
		var payload runtimeevent.ApprovalResolvedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			if m.pendingApproval != nil && m.pendingApproval.ApprovalID == payload.ApprovalID {
				m.pendingApproval = nil
				if m.mode == ModeApproval {
					m.mode = ModeChat
				}
			}
		}
	case runtimeevent.KindModelRequestFailed:
		var payload runtimeevent.ModelRequestFailedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.setStatus(fmt.Sprintf("Model request failed at %s: %s", payload.Stage, payload.Code), true)
		}
		m.setActivity("Model request failed")
	case runtimeevent.KindModelRequestRetrying:
		var payload runtimeevent.ModelRequestRetryingPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.setStatus(fmt.Sprintf("Model request %s; retrying in %.0fs (attempt %d/%d)",
				payload.Code, float64(payload.WaitMs)/1000, payload.Attempt, payload.MaxAttempts), true)
		}
		m.setActivity("Waiting to retry model request")
	case runtimeevent.KindContextCompacted:
		var payload runtimeevent.ContextCompactedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.compactions++
			m.setStatus(fmt.Sprintf("Context compacted ~%s → ~%s tokens",
				humanizeTokens(int64(payload.EstTokensBefore)),
				humanizeTokens(int64(payload.EstTokensAfter))), false)
		}
	case runtimeevent.KindContextUsage:
		var payload runtimeevent.ContextUsagePayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.contextOccupancy = payload.OccupancyTokens
		}
	case runtimeevent.KindPlanUpdated:
		var plan domain.Plan
		if err := json.Unmarshal(evt.Payload, &plan); err == nil {
			m.plan = plan
		}
	case runtimeevent.KindRunCancelRequested:
		m.phase = "cancelling"
		m.setActivity("Cancelling active work")
	case runtimeevent.KindRunCompleted, runtimeevent.KindTurnFinished:
		m.phase = "idle"
		m.setActivity("Ready")
		if evt.Kind == runtimeevent.KindTurnFinished {
			var payload runtimeevent.TurnFinishedPayload
			if err := json.Unmarshal(evt.Payload, &payload); err == nil {
				switch {
				case payload.Error != "":
					m.setStatus("Turn failed: "+truncateDisplayWidth(payload.Error, 80), true)
				case m.statusIsError:
					// A clean turn clears a stale error from an earlier failure.
					m.setStatus("Ready", false)
				}
			}
		}
	case runtimeevent.KindRunCancelled:
		m.phase = "idle"
		m.setStatus("Turn cancelled", false)
		m.setActivity("Ready")
	case runtimeevent.KindSubagentProgress:
		m.handleSubagentProgressOverlay(evt)
	case runtimeevent.KindSubagentStarted:
		var payload runtimeevent.SubagentStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.setActivity("Sub-agent exploring")
		}
	case runtimeevent.KindSubagentFinished:
		var payload runtimeevent.SubagentFinishedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.Outcome != string(domain.OutcomeSucceeded) && payload.Outcome != "" {
			m.setStatus(fmt.Sprintf("Sub-agent run ended: %s", payload.Outcome), payload.Outcome != string(domain.OutcomeCompletedUnverified))
		}
	}
	switch evt.Kind {
	case runtimeevent.KindUsageUpdated:
		var payload runtimeevent.UsageUpdatedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.usage.InputTokens, m.usage.OutputTokens, m.usage.Turns = payload.InputTokens, payload.OutputTokens, payload.Turns
		}
	case runtimeevent.KindBudgetUpdated:
		var payload runtimeevent.BudgetUpdatedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.usage.InputTokens, m.usage.OutputTokens = payload.InputTokens, payload.OutputTokens
			m.usage.Turns, m.usage.ToolCalls = payload.Turns, payload.ToolCalls
			m.usage.CachedInputTokens, m.usage.ContextTokens = payload.CachedInputTokens, payload.ContextTokens
		}
	}
	if ApplyRuntimeEvent(m.blocks, evt) != "" && !m.followTail {
		// Near-bottom magnetism: a user who scrolled only a few lines away
		// from the tail almost certainly still wants to follow new output.
		// Without this, an accidental wheel/↑ nudge hides freshly streamed
		// replies below the fold with no visible difference from "no reply",
		// and the next prompt submit appears to "answer the previous message".
		if m.mode == ModeChat && m.nearBottom() {
			m.resumeFollowTail()
		} else {
			m.newEvents++
		}
	}
	cmds := []tea.Cmd{m.waitForEvent()}
	if m.isBusy() && !m.spinning {
		m.spinning = true
		cmds = append(cmds, m.spinner.Tick)
	}
	return m, tea.Batch(cmds...)
}

// --- async controller commands ---

// submitPromptWithImagesCmd submits a prompt with optional image attachments.
// Image loading is done asynchronously; on failure a warning is posted to the
// status line but the text prompt still goes through.
func (m Model) submitPromptWithImagesCmd(prompt string, images []domain.ImageContent) tea.Cmd {
	return func() tea.Msg {
		result, err := m.controller.SubmitPrompt(context.Background(), prompt, images)
		return promptSubmittedMsg{prompt: prompt, result: result, err: err, imageCount: len(images)}
	}
}

func (m Model) cancelTurnCmd() tea.Cmd {
	return func() tea.Msg {
		err := m.controller.CancelTurn(context.Background())
		return turnCancelRequestedMsg{err: err}
	}
}

func (m Model) resolveApprovalCmd(payload *runtimeevent.ApprovalRequestedPayload, decision domain.Decision, remember bool, trust string) tea.Cmd {
	return func() tea.Msg {
		var hint *app.ApprovalRuleHint
		if remember && decision == domain.DecisionAllow {
			hint = &app.ApprovalRuleHint{ToolName: payload.ToolName, Arguments: payload.Arguments, Trust: trust}
		}
		note, err := m.controller.ResolveApproval(context.Background(), approvalBinding(payload), decision, hint)
		return approvalResolvedMsg{err: err, ruleNote: note}
	}
}

func (m Model) sessionCmd(action sessionAction) tea.Cmd {
	return func() tea.Msg {
		err := action.run(context.Background())
		return sessionSwitchedMsg{action: action, err: err}
	}
}

// --- transcript search (Ctrl+F) ---

// searchDebounceDelay paces the full-block scan: matching runs once the
// user pauses typing instead of on every keystroke.
const searchDebounceDelay = 150 * time.Millisecond

// searchDebounceMsg fires the debounced scan for one query generation.
type searchDebounceMsg struct{ gen int }

// debounceSearch bumps the query generation and schedules the scan. A
// tick that arrives after a newer keystroke (or after search was exited)
// is stale and ignored.
func (m *Model) debounceSearch() tea.Cmd {
	m.searchGen++
	gen := m.searchGen
	return tea.Tick(searchDebounceDelay, func(time.Time) tea.Msg {
		return searchDebounceMsg{gen: gen}
	})
}

func (m Model) handleSearchKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.Type {
	case tea.KeyEsc:
		m.exitSearch()
		return m, nil
	case tea.KeyEnter:
		m.nextSearchMatch()
		return m, nil
	case tea.KeyBackspace:
		if m.searchQuery != "" {
			runes := []rune(m.searchQuery)
			m.searchQuery = string(runes[:len(runes)-1])
			return m, m.debounceSearch()
		}
		return m, nil
	case tea.KeyRunes:
		m.searchQuery += string(msg.Runes)
		return m, m.debounceSearch()
	case tea.KeyCtrlC:
		// Keep Ctrl+C's global meaning (cancel/quit) even in search mode.
		m.exitSearch()
		return m.handleCtrlC()
	}
	return m, nil
}

// enterSearch switches the composer area into transcript-search mode.
func (m *Model) enterSearch() {
	m.mode = ModeSearch
	m.searchQuery = ""
	m.searchMatches = nil
	m.searchIndex = 0
}

func (m *Model) exitSearch() {
	m.mode = ModeChat
	m.searchQuery = ""
	m.searchMatches = nil
	m.searchIndex = 0
	m.searchGen++ // invalidate any debounce tick still in flight
}

// updateSearch recomputes matches for the current query and jumps to the
// first one. Matches cover block text, targets, previews and diffs.
func (m *Model) updateSearch() {
	m.searchMatches = m.searchMatches[:0]
	query := strings.ToLower(m.searchQuery)
	if query == "" {
		return
	}
	for _, id := range m.blocks.Order {
		b := m.blocks.ByID[id]
		haystack := strings.ToLower(b.Title + "\n" + b.Content + "\n" + b.Target + "\n" + b.Preview + "\n" + b.Diff)
		if strings.Contains(haystack, query) {
			m.searchMatches = append(m.searchMatches, id)
		}
	}
	m.searchIndex = 0
	m.jumpToSearchMatch()
}

// nextSearchMatch advances to the next match, wrapping around.
func (m *Model) nextSearchMatch() {
	if len(m.searchMatches) == 0 {
		return
	}
	m.searchIndex = (m.searchIndex + 1) % len(m.searchMatches)
	m.jumpToSearchMatch()
}

// jumpToSearchMatch scrolls the transcript so the current match is visible.
func (m *Model) jumpToSearchMatch() {
	if len(m.searchMatches) == 0 || m.searchIndex >= len(m.searchMatches) {
		return
	}
	offset, ok := m.blockOffsets[m.searchMatches[m.searchIndex]]
	if !ok {
		return
	}
	m.followTail = false
	m.viewport.SetYOffset(max(0, offset-1))
}

// --- clipboard (Ctrl+Y) ---

// clipboardCopiedMsg reports the result of a clipboard write.
type clipboardCopiedMsg struct {
	chars int
	err   error
}

// copyToClipboard writes text to the system clipboard using the platform's
// copy command: pbcopy on macOS, wl-copy/xclip/xsel on Linux.
func copyToClipboard(text string) tea.Cmd {
	return func() tea.Msg {
		var cmd *exec.Cmd
		switch runtime.GOOS {
		case "darwin":
			cmd = exec.Command("pbcopy")
		default:
			for _, candidate := range []string{"wl-copy", "xclip", "xsel"} {
				path, err := exec.LookPath(candidate)
				if err != nil {
					continue
				}
				switch candidate {
				case "xclip":
					cmd = exec.Command(path, "-selection", "clipboard")
				case "xsel":
					cmd = exec.Command(path, "--clipboard", "--input")
				default:
					cmd = exec.Command(path)
				}
				break
			}
			if cmd == nil {
				return clipboardCopiedMsg{err: fmt.Errorf("no clipboard tool found (install wl-copy, xclip or xsel)")}
			}
		}
		cmd.Stdin = strings.NewReader(text)
		if err := cmd.Run(); err != nil {
			return clipboardCopiedMsg{err: err}
		}
		return clipboardCopiedMsg{chars: len(text)}
	}
}

// --- clipboard image paste (Ctrl+V) ---

// clipboardImageMsg reports the result of reading an image from the system
// clipboard. The image is attached to the composer without submitting; the
// user sends it (with optional text) by pressing Enter.
type clipboardImageMsg struct {
	name    string
	content domain.ImageContent
	err     error
}

// pngMagic is the 8-byte PNG signature used to validate clipboard bytes —
// platform tools occasionally return text diagnostics on stdout with exit 0
// (notably xclip), so trust the bytes, not the exit code.
var pngMagic = []byte("\x89PNG\r\n\x1a\n")

// pasteClipboardImageCmd reads a PNG image from the system clipboard
// asynchronously and attaches it to the composer.
func (m Model) pasteClipboardImageCmd() tea.Cmd {
	return func() tea.Msg {
		raw, err := readClipboardImage()
		if err != nil {
			return clipboardImageMsg{err: err}
		}
		if len(raw) > maxImageLoadBytes {
			return clipboardImageMsg{err: fmt.Errorf("clipboard image is %d bytes, exceeding the %d byte limit", len(raw), maxImageLoadBytes)}
		}
		if !bytes.HasPrefix(raw, pngMagic) {
			return clipboardImageMsg{err: fmt.Errorf("clipboard does not contain a PNG image")}
		}
		name := fmt.Sprintf("clipboard-%s.png", time.Now().Format("150405"))
		return clipboardImageMsg{name: name, content: domain.ImageContent{MediaType: "image/png", Data: base64.StdEncoding.EncodeToString(raw)}}
	}
}

// readClipboardImage reads PNG bytes of the image currently on the system
// clipboard using the platform's paste command: pngpaste (with an osascript
// fallback) on macOS, wl-paste/xclip on Linux. Windows is not supported.
func readClipboardImage() ([]byte, error) {
	switch runtime.GOOS {
	case "darwin":
		if path, err := exec.LookPath("pngpaste"); err == nil {
			tmp, err := os.CreateTemp("", "loom-clip-*.png")
			if err != nil {
				return nil, err
			}
			name := tmp.Name()
			tmp.Close()
			defer os.Remove(name)
			if out, err := exec.Command(path, name).CombinedOutput(); err != nil {
				return nil, fmt.Errorf("pngpaste: %s", strings.TrimSpace(string(out)))
			}
			return os.ReadFile(name)
		}
		// osascript fallback: no pngpaste installed. «class PNGf» is the
		// clipboard's PNG flavor; write it to a temp file.
		tmp, err := os.CreateTemp("", "loom-clip-*.png")
		if err != nil {
			return nil, err
		}
		name := tmp.Name()
		tmp.Close()
		defer os.Remove(name)
		script := fmt.Sprintf(`set pngData to the clipboard as «class PNGf»
set fp to open for access (POSIX file %q) with write permission
set eof of fp to 0
write pngData to fp
close access fp`, name)
		if out, err := exec.Command("osascript", "-e", script).CombinedOutput(); err != nil {
			return nil, fmt.Errorf("no image on clipboard (or install pngpaste: brew install pngpaste): %s", strings.TrimSpace(string(out)))
		}
		return os.ReadFile(name)
	default:
		if path, err := exec.LookPath("wl-paste"); err == nil {
			out, err := exec.Command(path, "--type", "image/png").Output()
			if err != nil || len(out) == 0 {
				return nil, fmt.Errorf("no image on clipboard")
			}
			return out, nil
		}
		if path, err := exec.LookPath("xclip"); err == nil {
			out, err := exec.Command(path, "-selection", "clipboard", "-t", "image/png", "-o").Output()
			if err != nil || len(out) == 0 {
				return nil, fmt.Errorf("no image on clipboard")
			}
			return out, nil
		}
		return nil, fmt.Errorf("no clipboard image tool found (install wl-paste or xclip)")
	}
}

// handleClipboardImage attaches a clipboard image to the composer. Unlike
// path-based loads (which defer submission until all loads finish), clipboard
// attaches do not interact with the pending-submit counters: the image is
// ready immediately and rides along on the next Enter.
func (m Model) handleClipboardImage(msg clipboardImageMsg) (tea.Model, tea.Cmd) {
	if msg.err != nil {
		m.setStatus(fmt.Sprintf("Clipboard image: %v", msg.err), true)
		return m, nil
	}
	if len(m.attachedImages) >= maxImageAttachments {
		m.setStatus(fmt.Sprintf("At most %d image attachments", maxImageAttachments), true)
		return m, nil
	}
	m.attachedImages = append(m.attachedImages, msg.content)
	m.attachedPaths = append(m.attachedPaths, msg.name)
	m.setStatus(fmt.Sprintf("Attached %s (%d/%d) — send with Enter", msg.name, len(m.attachedImages), maxImageAttachments), false)
	return m, nil
}

// --- status helpers ---

func (m *Model) setActivity(label string) {
	m.activityLabel = label
	m.lastActivityAt = time.Now()
}

func (m *Model) setStatus(message string, isError bool) {
	m.statusMessage = message
	m.statusIsError = isError
}

func (m Model) isBusy() bool {
	return m.phase != "" && m.phase != "idle"
}
