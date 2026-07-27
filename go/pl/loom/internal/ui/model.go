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
	"os"
	"os/exec"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textarea"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"golang.org/x/term"
)

// Mode represents the top-level UI mode.
type Mode string

const (
	ModeChat            Mode = "chat"
	ModeApproval        Mode = "approval"
	ModeSessionPicker   Mode = "session_picker"
	ModeModelPicker     Mode = "model_picker"
	ModeReasoningPicker Mode = "reasoning_picker"
	ModeHelp            Mode = "help"
	ModeSearch          Mode = "search"
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
	gitBranch string
	phase     string
	usage     domain.Usage
	limits    domain.Limits
	// reasoningEffort is the effective reasoning dial shown in the header
	// ("" = provider decides); reasoningOverridden marks a session-level
	// /reasoning override (rendered with a trailing *).
	reasoningEffort     string
	reasoningOverridden bool
// compactions counts context compaction passes observed in this session
// view (shown in the status bar once non-zero).
compactions int
// plan is the latest task plan published via plan.updated (empty when the
// model never called update_plan). planHidden is the ctrl+t toggle that
// collapses the pinned plan panel above the composer.
plan       domain.Plan
planHidden bool
	// contextEst is the estimated token size of the next model request
	// (byte/4 approximation); lastCallInput is the provider-metered input
	// tokens of the most recent call. contextWindow is the optional model
	// context-window size used as the denominator in the status bar.
	contextEst    int
	lastCallInput int64
	contextWindow int

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
	// approvalShownAt marks when the overlay appeared; decision keys are
	// ignored briefly so a held/double-tapped key from the previous overlay
	// cannot spill into a fresh approval the user has not read yet.
	approvalShownAt time.Time

	// Session picker
	picker *SessionPicker

	// Model picker (/model): the static catalog resolved at startup, and
	// its popup state while ModeModelPicker is active.
	models      []ModelOption
	modelPicker *ModelPicker

	// Reasoning picker (/reasoning): popup state while ModeReasoningPicker
	// is active.
	reasoningPicker *ReasoningPicker

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

	// pendingSteers mirrors the controller's steer queue for the pinned
	// steer panel: messages queued while a turn is busy, waiting for the
	// loop to inject them before its next model call. Fed by steer.queued,
	// drained FIFO by steer.injected, rebuilt from Snapshot.PendingSteers.
	pendingSteers []string

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

// SetLimits records the active run budget for the status bar display.
func (m *Model) SetLimits(limits domain.Limits) {
	m.limits = limits
}

// SetModels records the selectable model catalog for the /model picker.
func (m *Model) SetModels(models []ModelOption) {
	m.models = models
}

// SetContextWindow records the model context-window size used as the
// denominator of the ctx status segment; zero hides the denominator.
func (m *Model) SetContextWindow(tokens int) {
	m.contextWindow = tokens
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
	// Limits is the active run budget shown by the status bar; a zero value
	// renders usage without the budget denominator.
	Limits domain.Limits
	// ContextWindow is the startup model's context-window size in tokens
	// (from its config metadata); zero renders the ctx segment without it.
	ContextWindow int
	// Models is the selectable provider/model catalog for the /model picker
	// (static for the process lifetime).
	Models []ModelOption
}

// StartTUI starts the Bubble Tea program. Blocks until the TUI exits.
func StartTUI(controller *app.Controller, modelName, workspace string, opts InitOptions) error {
	m := NewModel(controller, modelName, workspace)
	// The header band shows the workspace's git branch. Detection is a
	// one-shot, bounded probe: a slow or missing git must never delay
	// startup, and branch switches mid-session are rare enough to ignore.
	m.gitBranch = detectGitBranch(workspace)
	eventsCh, unsubscribe := controller.Subscribe()
	m.eventsCh = eventsCh
	defer unsubscribe()
	m.SetIcons(ResolveIcons(opts.Icons))
	m.SetLimits(opts.Limits)
	m.SetContextWindow(opts.ContextWindow)
	m.SetModels(opts.Models)
	if opts.NoColor {
		m.SetTheme(NoColorTheme())
	}

	programOptions := []tea.ProgramOption{tea.WithMouseCellMotion()}
	if opts.AltScreen {
		programOptions = append(programOptions, tea.WithAltScreen())
	}
	// Route input through the sequence-aware reader so fragmented escape
	// sequences (mouse reports delivered in small pieces) arrive at the
	// parser whole. Bubble Tea only puts the terminal into raw mode when it
	// owns the input file, so we must do it ourselves here.
	if isTerminalFd(os.Stdin) {
		if oldState, err := term.MakeRaw(int(os.Stdin.Fd())); err == nil {
			defer func() { _ = term.Restore(int(os.Stdin.Fd()), oldState) }()
			programOptions = append(programOptions, tea.WithInput(newInputReader(os.Stdin)))
		}
	}
	p := tea.NewProgram(m, programOptions...)

	_, err := p.Run()
	return err
}

// detectGitBranch resolves the workspace's current git branch for the
// header band. An empty result simply hides the branch segment.
func detectGitBranch(workspace string) string {
	if workspace == "" {
		return ""
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, "git", "-C", workspace, "rev-parse", "--abbrev-ref", "HEAD").Output()
	if err != nil {
		return ""
	}
	branch := strings.TrimSpace(string(out))
	// A detached HEAD reports "HEAD": that is a commit, not a branch name.
	if branch == "HEAD" {
		return ""
	}
	return branch
}

// isTerminalFd reports whether f is a character device (terminal).
func isTerminalFd(f *os.File) bool {
	info, err := f.Stat()
	if err != nil {
		return false
	}
	return info.Mode()&os.ModeCharDevice != 0
}
