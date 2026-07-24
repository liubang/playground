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
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
	"runtime"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// maxEventResubscribes bounds how many times the UI re-attaches to the event
// stream after the broker disconnects it before locking input.
const maxEventResubscribes = 3

// maxCompletionRows bounds the visible completion candidates.
const maxCompletionRows = 6

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
	{name: "/compact", usage: "/compact", desc: "Compact context (not implemented yet)"},
	{name: "/inspect", usage: "/inspect", desc: "Inspect session state (not implemented yet)"},
	{name: "/model", usage: "/model", desc: "Switch model (not implemented yet)"},
	{name: "/exit", usage: "/exit", desc: "Exit"},
}

// --- async controller command results ---

// promptSubmittedMsg reports the controller's ack for a submitted prompt.
type promptSubmittedMsg struct {
	prompt string
	err    error
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

// turnCancelRequestedMsg reports the result of a cancel request.
type turnCancelRequestedMsg struct{ err error }

// approvalResolvedMsg reports the result of an approval resolution.
type approvalResolvedMsg struct{ err error }

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
		next, cmd = m.handleEventsClosed()
	case snapshotMsg:
		next, cmd = m.handleSnapshot(msg)
	case sessionsLoadedMsg:
		m.picker.Load(msg.sessions, msg.err)
		next = m
	case promptSubmittedMsg:
		next = m.handlePromptSubmitted(msg)
	case sessionSwitchedMsg:
		next, cmd = m.handleSessionSwitched(msg)
	case turnCancelRequestedMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Cancel failed: %v", msg.err), true)
		}
		next = m
	case approvalResolvedMsg:
		if msg.err != nil {
			m.setStatus(fmt.Sprintf("Approval resolution rejected: %v", msg.err), true)
		}
		next = m
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
	if len(m.blocks.Order) == 0 {
		m.viewport.SetContent(m.renderWelcome())
		return
	}
	if m.renderCache == nil {
		m.renderCache = make(map[string]cachedRender)
	}
	lines := make([]string, 0, len(m.blocks.Order))
	m.blockOffsets = make(map[string]int, len(m.blocks.Order))
	offset := 0
	for _, id := range m.blocks.Order {
		block := m.blocks.ByID[id]
		m.blockOffsets[id] = offset
		key := m.blockRenderKey(block)
		var out string
		if key != "" {
			if entry, ok := m.renderCache[id]; ok && entry.key == key {
				out = entry.out
				lines = append(lines, out)
				offset += lipgloss.Height(out)
				continue
			}
		}
		out = m.renderBlock(block)
		if key != "" {
			m.renderCache[id] = cachedRender{key: key, out: out}
		}
		lines = append(lines, out)
		// The "\n" join is a line separator, not an extra row: the next block
		// starts right after this block's own height.
		offset += lipgloss.Height(out)
	}
	m.viewport.SetContent(strings.Join(lines, "\n"))
	if m.followTail {
		m.viewport.GotoBottom()
	}
}

// cachedRender is a memoized renderBlock output.
type cachedRender struct {
	key string
	out string
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
	return sb.String()
}

// visibleTranscriptHeight computes available height for the transcript
// viewport, reserving space for whatever occupies the composer area.
func (m Model) visibleTranscriptHeight() int {
	reserved := 1 + 1 // header + status bar
	switch m.mode {
	case ModeChat:
		reserved += m.textArea.Height() + 2 // composer border
		reserved += m.completionHeight()
	case ModeSearch:
		reserved += 3 // one-line search bar + border
	case ModeApproval:
		reserved += approvalOverlayHeight
		if m.pendingApproval != nil && m.pendingApproval.Diff != "" {
			diffLines := strings.Count(m.pendingApproval.Diff, "\n") + 1
			reserved += min(diffLines, approvalDiffMaxLines) + 2
		}
	case ModeHelp:
		reserved += helpOverlayHeight
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
	case ModeSessionPicker:
		return m.handlePickerKey(msg)
	case ModeSearch:
		return m.handleSearchKey(msg)
	}

	switch msg.Type {
	case tea.KeyCtrlC:
		return m.handleCtrlC()
	case tea.KeyCtrlD:
		return m.handleCtrlD()
	case tea.KeyCtrlR:
		m.blocks.ToggleLatestReasoning()
		return m, nil
	case tea.KeyCtrlE:
		m.blocks.ToggleLatestToolOutput()
		return m, nil
	case tea.KeyCtrlO:
		m.blocks.ToggleAllToolOutputs()
		return m, nil
	case tea.KeyCtrlF:
		m.enterSearch()
		return m, nil
	case tea.KeyCtrlY:
		text := m.blocks.LatestFinalAssistantText()
		if text == "" {
			m.setStatus("Nothing to copy", true)
			return m, nil
		}
		return m, copyToClipboard(text)
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
	case tea.KeyCtrlEnd:
		m.resumeFollowTail()
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
	if m.mode != ModeChat {
		return m, nil
	}
	if msg.Action == tea.MouseActionPress && msg.Button == tea.MouseButtonWheelUp {
		m.pauseFollowTail()
	}
	var cmd tea.Cmd
	m.viewport, cmd = m.viewport.Update(msg)
	m.updateFollowTailAtBottom()
	return m, cmd
}

func (m Model) handlePickerKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.Type {
	case tea.KeyEsc:
		m.mode = ModeChat
		return m, nil
	case tea.KeyUp:
		m.picker.MoveUp()
	case tea.KeyDown:
		m.picker.MoveDown()
	case tea.KeyEnter:
		sessionID := m.picker.Selected()
		if sessionID.IsZero() {
			return m, nil
		}
		m.setStatus("Resuming session...", false)
		return m, m.sessionCmd(sessionAction{
			name:    "Resume",
			success: "Session resumed",
			run:     func(ctx context.Context) error { return m.controller.ResumeSession(ctx, sessionID) },
		})
	}
	return m, nil
}

// --- follow-tail helpers ---

func (m *Model) pauseFollowTail() {
	m.followTail = false
}

func (m *Model) updateFollowTailAtBottom() {
	if m.viewport.AtBottom() {
		m.followTail = true
		m.newEvents = 0
	}
}

func (m *Model) resumeFollowTail() {
	m.followTail = true
	m.newEvents = 0
	m.viewport.GotoBottom()
}

// --- composer submission and slash commands ---

func (m Model) submitUserInput(raw string) (tea.Model, tea.Cmd) {
	value := strings.TrimSpace(raw)
	if value == "" {
		return m, nil
	}
	if strings.HasPrefix(value, "/") {
		return m.handleSlashCommand(value)
	}
	if m.eventsDead {
		m.setStatus("Cannot submit: runtime event stream is down. Press Ctrl+C to exit.", true)
		return m, nil
	}
	// Optimistic local echo; replaced by the durable turn.started confirmation
	// or removed when the controller rejects the prompt.
	m.pendingSubmitID = m.blocks.AddPendingUserBlock(raw)
	m.pendingSubmitPrompt = raw
	m.resumeFollowTail()
	m.textArea.Reset()
	m.setStatus("Prompt submitted", false)
	m.setActivity("Waiting for the model")
	m.quitConfirm = false
	return m, m.submitPromptCmd(raw)
}

func (m Model) handlePromptSubmitted(msg promptSubmittedMsg) tea.Model {
	if msg.err == nil {
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
		m.picker = NewSessionPicker()
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
	case "/exit":
		return m, tea.Quit
	case "/compact", "/inspect", "/model":
		// Keep the draft so the user can edit or retry a mistyped command.
		m.setStatus(fmt.Sprintf("%s is not implemented yet", fields[0]), false)
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
	m.mode = ModeChat
	m.setStatus(msg.action.success, false)
	return m, m.requestSnapshot()
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

func (m Model) handleApprovalKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	if m.pendingApproval == nil {
		m.mode = ModeChat
		return m, nil
	}
	var decision domain.Decision
	switch msg.Type {
	case tea.KeyLeft, tea.KeyShiftTab:
		m.approvalCursor = 0
		return m, nil
	case tea.KeyRight, tea.KeyTab:
		m.approvalCursor = 1
		return m, nil
	case tea.KeyEnter:
		if m.approvalCursor == 0 {
			decision = domain.DecisionAllow
		} else {
			decision = domain.DecisionDeny
		}
	case tea.KeyRunes:
		switch msg.String() {
		case "y", "Y":
			decision = domain.DecisionAllow
		case "n", "N":
			decision = domain.DecisionDeny
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
		return m, tea.Batch(m.resolveApprovalCmd(payload, domain.DecisionDeny), m.cancelTurnCmd())
	default:
		return m, nil
	}
	payload := m.pendingApproval
	m.pendingApproval = nil
	m.mode = ModeChat
	return m, m.resolveApprovalCmd(payload, decision)
}

func approvalBinding(payload *runtimeevent.ApprovalRequestedPayload) app.ApprovalBinding {
	return app.ApprovalBinding{
		ApprovalID: payload.ApprovalID,
		CallID:     payload.CallID,
		ArgsHash:   payload.ArgsHash,
	}
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
	m.modelName = msg.snapshot.ModelName
	m.workspace = msg.snapshot.WorkspaceRoot
	m.usage = msg.snapshot.Usage
	if sessionChanged || (m.initialSnapshotPending && len(m.blocks.Order) == 0) {
		m.blocks = RebuildTranscript(msg.snapshot.Messages)
	} else {
		m.mergeSnapshot(msg.snapshot.Messages)
	}
	m.initialSnapshotPending = false
	// A stale overlay must not survive a snapshot: approvals that are no
	// longer pending at the runtime are dismissed.
	if m.pendingApproval != nil {
		stillPending := false
		for _, id := range msg.snapshot.PendingApprovals {
			if id == m.pendingApproval.ApprovalID {
				stillPending = true
				break
			}
		}
		if !stillPending {
			m.pendingApproval = nil
			if m.mode == ModeApproval {
				m.mode = ModeChat
			}
		}
	}
	m.resumeFollowTail()
	return m, nil
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
func (m Model) handleEventsClosed() (tea.Model, tea.Cmd) {
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
	eventsCh, _ := m.controller.Subscribe()
	m.eventsCh = eventsCh
	m.setStatus("Event stream interrupted; resubscribed, refreshing view from snapshot", true)
	return m, tea.Batch(m.waitForEvent(), m.requestSnapshot())
}

func (m Model) handleRuntimeEvent(evt runtimeevent.RuntimeEvent) (Model, tea.Cmd) {
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
	case runtimeevent.KindSessionOpened:
		var payload runtimeevent.SessionOpenedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.modelName = payload.Model
			m.workspace = payload.Workspace
		}
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
	case runtimeevent.KindApprovalRequested:
		var payload runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			m.pendingApproval = &payload
			m.approvalCursor = 0
			m.mode = ModeApproval
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
	case runtimeevent.KindRunCancelRequested:
		m.phase = "cancelling"
		m.setActivity("Cancelling active work")
	case runtimeevent.KindRunCompleted, runtimeevent.KindTurnFinished:
		m.phase = "idle"
		m.setActivity("Ready")
	case runtimeevent.KindRunCancelled:
		m.phase = "idle"
		m.setStatus("Turn cancelled", false)
		m.setActivity("Ready")
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
		}
	}
	if ApplyRuntimeEvent(m.blocks, evt) != "" && !m.followTail {
		m.newEvents++
	}
	cmds := []tea.Cmd{m.waitForEvent()}
	if m.isBusy() && !m.spinning {
		m.spinning = true
		cmds = append(cmds, m.spinner.Tick)
	}
	return m, tea.Batch(cmds...)
}

// --- async controller commands ---

func (m Model) submitPromptCmd(prompt string) tea.Cmd {
	return func() tea.Msg {
		err := m.controller.SubmitPrompt(context.Background(), prompt)
		return promptSubmittedMsg{prompt: prompt, err: err}
	}
}

func (m Model) cancelTurnCmd() tea.Cmd {
	return func() tea.Msg {
		err := m.controller.CancelTurn(context.Background())
		return turnCancelRequestedMsg{err: err}
	}
}

func (m Model) resolveApprovalCmd(payload *runtimeevent.ApprovalRequestedPayload, decision domain.Decision) tea.Cmd {
	return func() tea.Msg {
		err := m.controller.ResolveApproval(context.Background(), approvalBinding(payload), decision)
		return approvalResolvedMsg{err: err}
	}
}

func (m Model) sessionCmd(action sessionAction) tea.Cmd {
	return func() tea.Msg {
		err := action.run(context.Background())
		return sessionSwitchedMsg{action: action, err: err}
	}
}

// --- transcript search (Ctrl+F) ---

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
			m.updateSearch()
		}
		return m, nil
	case tea.KeyRunes:
		m.searchQuery += string(msg.Runes)
		m.updateSearch()
		return m, nil
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
