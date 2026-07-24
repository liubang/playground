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

// Package ui implements the Bubble Tea-based terminal UI for Loom.
// It consumes RuntimeEvents from the app.Controller and renders them
// as a transcript with composer, status bar, and approval overlay.
package ui

import (
	"context"
	"time"

	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textarea"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Mode represents the top-level UI mode.
type Mode string

const (
	ModeChat          Mode = "chat"
	ModeApproval      Mode = "approval"
	ModeSessionPicker Mode = "session_picker"
	ModeHelp          Mode = "help"
	ModeSearch        Mode = "search"
)

// Model is the root Bubble Tea model for the Loom TUI.
type Model struct {
	// Config
	theme    *Theme
	icons    Icons
	markdown *markdownRenderer
	width    int
	height   int

	// Controller (runtime interface)
	controller *app.Controller
	eventsCh   <-chan runtimeevent.RuntimeEvent

	// UI state
	mode      Mode
	sessionID domain.SessionID
	modelName string
	workspace string
	phase     string
	usage     domain.Usage

	// Transcript
	blocks                 *BlockIndex
	initialSnapshotPending bool
	followTail             bool
	newEvents              int // count of new events while not following tail

	// renderCache memoizes renderBlock output per block ID so syncTranscript
	// only re-renders blocks whose inputs changed. Entries are keyed by a
	// fingerprint of every render input (content, status, width, ...).
	renderCache map[string]cachedRender

	// blockOffsets maps block IDs to their line offset in the composed
	// transcript content, used by search to jump to a match.
	blockOffsets map[string]int

	// Transcript search state (ModeSearch).
	searchQuery   string
	searchMatches []string // matching block IDs in document order
	searchIndex   int

	// Composer
	textArea textarea.Model

	// Slash command completion popup. completionDismissedFor ties a dismissal
	// to the draft it was made on; any draft change re-arms the popup.
	completionCursor       int
	completionDismissedFor string

	// Status
	controllerState app.ControllerState
	statusMessage   string
	statusIsError   bool
	activityLabel   string
	lastActivityAt  time.Time

	// Approval overlay
	pendingApproval *runtimeevent.ApprovalRequestedPayload
	approvalCursor  int // 0 = allow once, 1 = deny

	// Session picker
	picker *SessionPicker

	// spinner animates in-progress activity while a turn is busy
	spinner  spinner.Model
	spinning bool

	// Ctrl+C double-tap tracking
	lastCancelTime time.Time

	// viewport for transcript
	viewport viewport.Model

	// Quit confirmation
	quitConfirm bool

	// pendingSubmit tracks the optimistic user echo until the controller acks
	// the prompt; a failure restores the draft and drops the echo.
	pendingSubmitID     string
	pendingSubmitPrompt string

	// resubscribes bounds event-stream recovery attempts; eventsDead locks
	// prompt submission once the stream cannot be recovered.
	resubscribes int
	eventsDead   bool
}

// NewModel creates a new UI model with the given controller.
func NewModel(controller *app.Controller, modelName, workspace string) Model {
	ta := textarea.New()
	ta.Placeholder = "Type your message... (Enter to send, Alt+Enter for newline)"
	ta.CharLimit = MaxPasteBytes
	// A chat composer is not a code editor: no line numbers.
	ta.ShowLineNumbers = false
	ta.Focus()
	ta.SetHeight(1)
	// Match the initial 80-column geometry used before the first WindowSizeMsg.
	ta.SetWidth(76)
	// Plain Enter submits (handled by the root model); newlines only come from
	// Alt+Enter or Ctrl+J, per the TUI design.
	ta.KeyMap.InsertNewline.SetKeys("alt+enter", "ctrl+j")

	vp := viewport.New(80, 20)

	theme := DetectTheme()
	sp := spinner.New(
		spinner.WithSpinner(spinner.MiniDot),
		spinner.WithStyle(theme.SpinnerStyle),
	)

	return Model{
		theme:                  theme,
		icons:                  NerdIcons(),
		markdown:               newMarkdownRenderer(),
		spinner:                sp,
		controller:             controller,
		mode:                   ModeChat,
		modelName:              modelName,
		workspace:              workspace,
		blocks:                 NewBlockIndex(),
		initialSnapshotPending: true,
		followTail:             true,
		textArea:               ta,
		phase:                  "idle",
		controllerState:        controller.State(),
		viewport:               vp,
		statusMessage:          "Ready",
		activityLabel:          "Ready",
		lastActivityAt:         time.Now(),
		picker:                 NewSessionPicker(),
	}
}

// SetTheme sets the active theme.
func (m *Model) SetTheme(theme *Theme) {
	m.theme = theme
	m.spinner.Style = theme.SpinnerStyle
	// Styles are baked into cached renders; start fresh.
	m.renderCache = nil
}

// SetIcons sets the glyph set (Nerd Font or plain text).
func (m *Model) SetIcons(icons Icons) {
	m.icons = icons
	m.renderCache = nil
}

// iconSet returns the active glyph set, defaulting to Nerd Font glyphs for
// zero-value models constructed directly in tests.
func (m Model) iconSet() Icons {
	if m.icons == (Icons{}) {
		return NerdIcons()
	}
	return m.icons
}

// Init implements tea.Model.
func (m Model) Init() tea.Cmd {
	// StartTUI subscribes before creating the Bubble Tea program. Keeping this
	// fallback makes a directly constructed Model safe in tests as well.
	if m.eventsCh == nil {
		eventsCh, _ := m.controller.Subscribe()
		m.eventsCh = eventsCh
	}

	// The spinner is started lazily once a turn becomes busy; idling sessions
	// do not need periodic redraws.
	return tea.Batch(
		m.waitForEvent(),
		m.requestSnapshot(),
		textarea.Blink,
	)
}

// waitForEvent returns a command that waits for the next runtime event.
func (m Model) waitForEvent() tea.Cmd {
	return func() tea.Msg {
		evt, ok := <-m.eventsCh
		if !ok {
			return runtimeEventsClosedMsg{}
		}
		return runtimeEventMsg(evt)
	}
}

func (m Model) requestSnapshot() tea.Cmd {
	return func() tea.Msg {
		snapshot, err := m.controller.RequestSnapshot(context.Background())
		return snapshotMsg{snapshot: snapshot, err: err}
	}
}

func (m Model) requestSessions() tea.Cmd {
	return func() tea.Msg {
		sessions, err := m.controller.ListSessions(context.Background(), 100)
		return sessionsLoadedMsg{sessions: sessions, err: err}
	}
}

// runtimeEventMsg wraps a runtime event for Bubble Tea message passing.
type runtimeEventMsg runtimeevent.RuntimeEvent

type runtimeEventsClosedMsg struct{}

type snapshotMsg struct {
	snapshot app.Snapshot
	err      error
}

type sessionsLoadedMsg struct {
	sessions []app.SessionSummary
	err      error
}

// InitOptions configures the TUI at startup.
type InitOptions struct {
	NoColor   bool
	AltScreen bool
	// Icons is the LOOM_ICONS preference: "nerd" (default) or "plain".
	Icons string
}

// StartTUI starts the Bubble Tea program. Blocks until the TUI exits.
func StartTUI(controller *app.Controller, modelName, workspace string, opts InitOptions) error {
	m := NewModel(controller, modelName, workspace)
	eventsCh, unsubscribe := controller.Subscribe()
	m.eventsCh = eventsCh
	defer unsubscribe()
	m.SetIcons(ResolveIcons(opts.Icons))
	if opts.NoColor {
		m.SetTheme(NoColorTheme())
	}

	programOptions := []tea.ProgramOption{tea.WithMouseCellMotion()}
	if opts.AltScreen {
		programOptions = append(programOptions, tea.WithAltScreen())
	}
	p := tea.NewProgram(m, programOptions...)

	_, err := p.Run()
	return err
}
