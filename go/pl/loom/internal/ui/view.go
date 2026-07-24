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

	"github.com/charmbracelet/lipgloss"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/render"
)

// Reserved heights (including border and padding) for the panels that replace
// the composer in their respective modes.
const (
	approvalOverlayHeight = 16
	helpOverlayHeight     = 23
)

// View renders the complete TUI.
func (m Model) View() string {
	// Until the first WindowSizeMsg arrives the terminal size is unknown.
	// Render a single harmless line: any larger first frame risks scrolling
	// the terminal, which permanently desynchronizes the inline renderer's
	// line tracking (this manifested as corrupted composer/dialog borders).
	if m.width <= 0 || m.height <= 0 {
		return m.theme.Dim.Render("Loom starting…")
	}
	var b strings.Builder

	// Header
	b.WriteString(m.renderHeader())
	b.WriteString("\n")

	if m.mode == ModeSessionPicker {
		b.WriteString(m.renderSessionPicker())
		b.WriteString("\n")
	} else {
		b.WriteString(m.renderTranscript())
		b.WriteString("\n")
	}

	// Approval and help panels replace the composer area instead of being
	// appended below the status bar, so the layout never overflows the
	// terminal and the status bar stays visible.
	switch m.mode {
	case ModeApproval:
		if m.pendingApproval != nil {
			b.WriteString(m.renderApprovalOverlay())
			b.WriteString("\n")
		}
	case ModeHelp:
		b.WriteString(m.renderHelpOverlay())
		b.WriteString("\n")
	case ModeSearch:
		b.WriteString(m.renderSearchBar())
		b.WriteString("\n")
	case ModeSessionPicker:
		// The picker owns the main area; no composer.
	default:
		if m.completionVisible() {
			b.WriteString(m.renderCompletion())
			b.WriteString("\n")
		}
		b.WriteString(m.theme.Composer.Render(m.textArea.View()))
		b.WriteString("\n")
	}

	// Status bar
	b.WriteString(m.renderStatusBar())

	return b.String()
}

func (m Model) renderHeader() string {
	if m.width < 30 {
		style := m.theme.HeaderStyle
		if m.width > 0 {
			style = style.Width(m.width)
		}
		return style.Render("Loom")
	}

	title := fmt.Sprintf("Loom · %s", m.modelName)
	if !m.sessionID.IsZero() {
		title += fmt.Sprintf(" · %s", shortID(m.sessionID))
	}
	if m.workspace != "" && m.width >= 60 {
		title += fmt.Sprintf(" · %s", m.workspace)
	}
	// Truncate before styling: lipgloss wraps overlong text onto a second
	// line, but the layout reserves exactly one row for the header. A wrapped
	// header makes the frame taller than the terminal and corrupts the
	// renderer's line tracking.
	if m.width > 2 {
		title = truncateDisplayWidth(title, m.width-2)
	}

	style := m.theme.HeaderStyle
	if m.width > 0 {
		style = style.Width(m.width)
	}
	return style.Render(title)
}

// welcomeLogo is the ASCII mark shown on the first screen of a fresh
// session; narrow terminals fall back to a plain wordmark.
var welcomeLogo = []string{
	"██╗      ██████╗  ██████╗ ███╗   ███╗",
	"██║     ██╔═══██╗██╔═══██╗████╗ ████║",
	"██║     ██║   ██║██║   ██║██╔████╔██║",
	"██║     ██║   ██║██║   ██║██║╚██╔╝██║",
	"███████╗╚██████╔╝╚██████╔╝██║ ╚═╝ ██║",
	"╚══════╝ ╚═════╝  ╚═════╝ ╚═╝     ╚═╝",
}

// renderWelcome renders the first screen shown before any message exists:
// logo, environment summary, and the handful of bindings worth discovering
// on day one.
func (m Model) renderWelcome() string {
	var b strings.Builder
	logoStyle := lipgloss.NewStyle().Foreground(m.theme.Primary).Bold(true)
	if m.width >= 44 {
		for _, line := range welcomeLogo {
			b.WriteString(logoStyle.Render(line))
			b.WriteString("\n")
		}
	} else {
		b.WriteString(logoStyle.Render("Loom"))
		b.WriteString("\n")
	}
	b.WriteString("\n")

	kv := func(key, value string) {
		if value == "" {
			return
		}
		b.WriteString(m.theme.Dim.Render(fmt.Sprintf("%-10s", key)))
		b.WriteString(truncateDisplayWidth(value, max(20, m.width-14)))
		b.WriteString("\n")
	}
	kv("Model", m.modelName)
	kv("Workspace", m.workspace)

	b.WriteString("\n")
	tips := []string{
		"Type a prompt and press Enter to begin",
		"/help lists commands and key bindings",
		"Ctrl+E tool output · Ctrl+O expand all · Ctrl+F search · Ctrl+Y copy reply",
	}
	for _, tip := range tips {
		b.WriteString(m.theme.Dim.Render(tip))
		b.WriteString("\n")
	}
	return strings.TrimRight(b.String(), "\n")
}

// renderSearchBar renders the one-line transcript-search input that replaces
// the composer in ModeSearch, with a live match counter.
func (m Model) renderSearchBar() string {
	count := ""
	switch {
	case m.searchQuery == "":
		count = "type to search the transcript"
	case len(m.searchMatches) == 0:
		count = "no matches"
	default:
		count = fmt.Sprintf("match %d/%d", m.searchIndex+1, len(m.searchMatches))
	}
	hint := m.theme.Dim.Render(count + " · Enter next · Esc done")
	line := m.theme.DialogLabel.Render("Search") + " " + m.searchQuery + "▏ " + hint
	return m.theme.Composer.Render(line)
}

func (m Model) renderSessionPicker() string {
	if m.picker == nil {
		return "Loading sessions..."
	}
	height := m.visibleTranscriptHeight()
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(m.picker.Render(m.width-6, height-2))
}

// renderTranscript renders the transcript viewport. The content itself is
// maintained Update-side by syncTranscript so that scroll state is real.
func (m Model) renderTranscript() string {
	return m.viewport.View()
}

func (m Model) renderBlock(block *TranscriptBlock) string {
	content := render.SanitizeText(block.Content)

	switch block.Kind {
	case BlockKindUser:
		return m.theme.UserBlock.Render(m.theme.UserLabel.Render("You:") + " " + content)
	case BlockKindAssistant:
		if !block.Done {
			content = m.renderReasoning(block) + content
			if block.PreparingTool != "" {
				content = strings.TrimSpace(content)
				if content != "" {
					content += "\n"
				}
				content += m.spinnerView() + " " + m.theme.Dim.Render(fmt.Sprintf("Preparing tool: %s...", block.PreparingTool))
			} else {
				// A soft caret marks the text as still arriving.
				content += m.theme.Dim.Render(" ▌")
			}
			return m.theme.StreamBlock.Render(content)
		}
		// The reasoning notice is already styled; feeding it through glamour
		// would mangle its ANSI sequences into visible text, so it is
		// prepended after the markdown rendering of the raw content.
		return m.theme.AssistantBlock.Render(m.renderReasoning(block) + m.renderMarkdown(content))
	case BlockKindInterrupted:
		return m.theme.InterruptedBlock.Render(content)
	case BlockKindTool:
		out := m.renderToolSummary(block)
		if block.Expanded {
			switch {
			case block.Diff != "":
				out += "\n" + indentLines(m.renderDiff(block.Diff), "  ")
			case block.Preview != "":
				out += "\n" + m.theme.Dim.Render(indentLines(block.Preview, "  "))
			}
		}
		return m.theme.ToolBlock.Render(out)
	case BlockKindNotice:
		return m.theme.NoticeBlock.Render(content)
	default:
		return content
	}
}

// renderMarkdown renders finalized assistant text through glamour: full
// CommonMark (tables, lists, links) plus chroma syntax highlighting, styled
// with the Everforest palette (or glamour's notty style for NO_COLOR). The
// renderer caches per content+width, so re-rendering the transcript each
// frame stays cheap. Streaming text bypasses this path (see renderBlock).
func (m Model) renderMarkdown(content string) string {
	r := m.markdown
	if r == nil {
		// Zero-value models constructed directly in tests get an ephemeral
		// renderer; production models always hold one from NewModel.
		r = newMarkdownRenderer()
	}
	wordWrap := 0
	if m.width > 8 {
		wordWrap = m.width - 8
	}
	return r.render(content, wordWrap, m.theme.MarkdownProfile)
}

func (m Model) renderReasoning(block *TranscriptBlock) string {
	if block.StreamReasoning == "" {
		return ""
	}
	if block.ReasoningExpanded {
		return m.theme.NoticeBlock.Render("Thinking:\n"+render.SanitizeText(block.StreamReasoning)) + "\n"
	}
	if !block.Done {
		return m.spinnerView() + " " + m.theme.Dim.Render("Thinking... (press Ctrl+R to expand)") + "\n"
	}
	return m.theme.Dim.Render("Thought process hidden (press Ctrl+R to expand)") + "\n"
}

// spinnerView renders the current spinner frame, falling back to a static
// marker when the spinner was never initialized (zero-value models in tests).
func (m Model) spinnerView() string {
	if len(m.spinner.Spinner.Frames) == 0 {
		return "●"
	}
	return m.spinner.View()
}

// phaseStyle colors the status-bar phase badge by what the runtime is doing.
func (m Model) phaseStyle(phase string) lipgloss.Style {
	switch phase {
	case "model":
		return lipgloss.NewStyle().Foreground(m.theme.Primary).Bold(true)
	case "tool":
		return lipgloss.NewStyle().Foreground(m.theme.Secondary).Bold(true)
	case "approval", "cancelling":
		return lipgloss.NewStyle().Foreground(m.theme.Warning).Bold(true)
	case "idle":
		return m.theme.Dim
	default:
		return lipgloss.NewStyle()
	}
}

// renderToolSummary renders the one-line summary of a tool block: the icon
// carries the status color, the tool name stays bold, the call target keeps
// normal weight, and secondary details (error code, duration) are dimmed.
// In-progress calls get an animated spinner and a live elapsed timer.
func (m Model) renderToolSummary(block *TranscriptBlock) string {
	inProgress := block.Status == "running" || block.Status == "prepared"

	var icon string
	if inProgress {
		icon = m.spinnerView()
	} else {
		icon = m.toolStatusIcon(block.Status)
	}

	var iconStyle lipgloss.Style
	switch block.Status {
	case "running", "prepared", "approval":
		iconStyle = m.theme.ToolRunning
	case "success":
		iconStyle = m.theme.ToolSuccess
	case "error":
		iconStyle = m.theme.ToolError
	default:
		iconStyle = m.theme.ToolCanceled
	}

	name := lipgloss.NewStyle().Bold(true).Render(block.Title)
	parts := []string{fmt.Sprintf("%s %s", iconStyle.Render(icon), name)}
	if block.Target != "" {
		parts = append(parts, truncateDisplayWidth(block.Target, 80))
	}
	switch {
	case block.Status == "running" && !block.StartedAt.IsZero():
		elapsed := time.Since(block.StartedAt).Round(time.Second)
		parts = append(parts, m.theme.Dim.Render(elapsed.String()))
	case block.Detail != "":
		parts = append(parts, m.theme.Dim.Render(block.Detail))
	}
	return strings.Join(parts, " · ")
}

// indentLines prefixes every line with the given indent string.
func indentLines(text, indent string) string {
	lines := strings.Split(text, "\n")
	for i, line := range lines {
		lines[i] = indent + line
	}
	return strings.Join(lines, "\n")
}

// renderDiff colors a compact argument diff (see render.DiffForToolCall):
// additions green, removals red, context lines and separators dim.
func (m Model) renderDiff(diff string) string {
	lines := strings.Split(diff, "\n")
	for i, line := range lines {
		switch {
		case strings.HasPrefix(line, "+ "):
			lines[i] = lipgloss.NewStyle().Foreground(m.theme.Success).Render(line)
		case strings.HasPrefix(line, "- "):
			lines[i] = lipgloss.NewStyle().Foreground(m.theme.Error).Render(line)
		default:
			lines[i] = m.theme.Dim.Render(line)
		}
	}
	return strings.Join(lines, "\n")
}

// toolStatusIcon maps a terminal tool status to its static icon.
func (m Model) toolStatusIcon(status string) string {
	icons := m.iconSet()
	switch status {
	case "success":
		return icons.Success
	case "error":
		return icons.Error
	case "approval":
		return icons.Approval
	case "cancelled":
		return icons.Cancelled
	default:
		return icons.Pending
	}
}

// renderStatusBar joins segments by descending priority and drops the least
// important ones until the bar fits the terminal width, so narrow screens
// lose detail instead of being truncated mid-escape-sequence.
func (m Model) renderStatusBar() string {
	phase := m.phase
	if phase == "" {
		phase = "idle"
	}
	phaseStyle := m.phaseStyle(phase)
	switch m.controllerState {
	case app.ControllerStateFatal, app.ControllerStateClosed, app.ControllerStateBooting:
		phase = StatusLabel(m.controllerState)
		phaseStyle = m.theme.StatusBarError
	}

	var plainParts, styledParts []string
	add := func(plain, styled string) {
		plainParts = append(plainParts, plain)
		styledParts = append(styledParts, styled)
	}

	add(fmt.Sprintf("[%s]", phase), phaseStyle.Render(fmt.Sprintf("[%s]", phase)))

	if m.activityLabel != "" && phase != "idle" {
		activity := m.activityLabel
		if !m.lastActivityAt.IsZero() {
			activity = fmt.Sprintf("%s · %s", activity, time.Since(m.lastActivityAt).Round(time.Second))
		}
		add(activity, m.spinnerView()+" "+m.theme.StatusBarBusy.Render(activity))
	}

	usage := fmt.Sprintf("turns:%d tokens:%d/%d tools:%d", m.usage.Turns, m.usage.InputTokens, m.usage.OutputTokens, m.usage.ToolCalls)
	add(usage, m.theme.Dim.Render(usage))

	if !m.followTail && m.newEvents > 0 {
		hint := fmt.Sprintf("↓%d new", m.newEvents)
		add(hint, lipgloss.NewStyle().Foreground(m.theme.Highlight).Bold(true).Render(hint))
	}

	if m.statusMessage != "" {
		style := m.theme.Dim
		if m.statusIsError {
			style = m.theme.StatusBarError
		}
		add(m.statusMessage, style.Render(m.statusMessage))
	}

	bar := strings.Join(styledParts, " · ")
	if m.width > 0 {
		for len(styledParts) > 1 && lipgloss.Width(bar) > m.width {
			plainParts = plainParts[:len(plainParts)-1]
			styledParts = styledParts[:len(styledParts)-1]
			bar = strings.Join(styledParts, " · ")
		}
		if lipgloss.Width(bar) > m.width {
			// Final fallback on plain text: truncating styled output could cut
			// an ANSI sequence in half and leak terminal state.
			bar = truncateDisplayWidth(strings.Join(plainParts, " · "), m.width)
		}
	}
	return bar
}

func (m Model) renderApprovalOverlay() string {
	p := m.pendingApproval
	if p == nil {
		return ""
	}

	var b strings.Builder
	title := m.theme.ApprovalTitle.Render(m.iconSet().Warning+" Approval Required") + "  " + m.riskBadge(p.Risk)
	b.WriteString(title)
	b.WriteString("\n\n")

	field := func(label, value string) {
		if value == "" {
			return
		}
		b.WriteString(m.theme.DialogLabel.Render(fmt.Sprintf("%-8s", label)))
		b.WriteString(value)
		b.WriteString("\n")
	}
	field("Tool", p.ToolName)
	if desc := render.SanitizeText(p.Description); desc != "" {
		field("Action", truncateDisplayWidth(desc, 200))
	}
	if len(p.ReadPaths) > 0 {
		field("Reads", truncateDisplayWidth(render.SanitizeText(strings.Join(p.ReadPaths, ", ")), 120))
	}
	if len(p.WritePaths) > 0 {
		field("Writes", truncateDisplayWidth(render.SanitizeText(strings.Join(p.WritePaths, ", ")), 120))
	}
	if p.ArgsHash != "" {
		hash := p.ArgsHash
		if len(hash) > 12 {
			hash = hash[:12]
		}
		field("Args hash", m.theme.Dim.Render(hash))
	}

	// The argument diff is the primary evidence for the allow/deny decision
	// on file-editing calls; it gets the remaining vertical budget.
	if p.Diff != "" {
		b.WriteString("\n")
		b.WriteString(m.renderDiff(headLines(p.Diff, approvalDiffMaxLines)))
		b.WriteString("\n")
	}

	b.WriteString("\n")
	icons := m.iconSet()
	allow := m.approvalOption(icons.Success+" Allow once", m.approvalCursor == 0)
	deny := m.approvalOption(icons.Error+" Deny", m.approvalCursor == 1)
	fmt.Fprintf(&b, "%s  %s\n\n", allow, deny)

	key := m.theme.ApprovalKey
	hint := strings.Join([]string{
		key.Render("←/→") + " select",
		key.Render("Enter") + " confirm",
		key.Render("y/n") + " quick",
		key.Render("Ctrl+C") + " deny+cancel",
	}, m.theme.Dim.Render(" · "))
	b.WriteString(hint)

	border := m.theme.ApprovalBorder
	if p.Risk >= domain.R3 {
		// Destructive calls get a red frame so the danger is visible before
		// the user reads a single word.
		border = border.BorderForeground(m.theme.Error)
	}
	width := m.width - 2
	if width <= 0 || width > 72 {
		width = 72
	}
	width = max(width, 20)
	return border.Width(width).Render(b.String())
}

// riskBadge renders the risk level as a colored badge: green for read-only,
// yellow for write, red for destructive and above.
func (m Model) riskBadge(risk domain.RiskLevel) string {
	label := fmt.Sprintf("R%d (%s)", int(risk), render.RiskDescription(risk))
	var style lipgloss.Style
	switch {
	case risk <= domain.R1:
		style = lipgloss.NewStyle().Foreground(m.theme.Success)
	case risk == domain.R2:
		style = lipgloss.NewStyle().Foreground(m.theme.Warning)
	default:
		style = lipgloss.NewStyle().Foreground(m.theme.Error).Bold(true)
	}
	return style.Render(label)
}

// approvalOption renders one dialog option, highlighted when selected.
func (m Model) approvalOption(label string, selected bool) string {
	if selected {
		return m.theme.ApprovalSelected.Render(label)
	}
	return m.theme.ApprovalOption.Render(" " + label + " ")
}

// approvalDiffMaxLines bounds the diff section inside the approval overlay.
const approvalDiffMaxLines = 12

// headLines returns the first n lines of text, appending an ellipsis line
// when truncated.
func headLines(text string, n int) string {
	lines := strings.Split(text, "\n")
	if len(lines) <= n {
		return text
	}
	return strings.Join(lines[:n], "\n") + "\n…"
}

// renderCompletion renders the slash-command completion popup above the
// composer, windowed around the cursor like the session picker.
func (m Model) renderCompletion() string {
	candidates := m.completionCandidates()
	if len(candidates) == 0 {
		return ""
	}
	cursor := min(m.completionCursor, len(candidates)-1)
	start := 0
	if cursor >= maxCompletionRows {
		start = cursor - maxCompletionRows + 1
	}
	end := min(start+maxCompletionRows, len(candidates))

	var b strings.Builder
	for i := start; i < end; i++ {
		c := candidates[i]
		usage := fmt.Sprintf("%-14s", c.usage)
		if i == cursor {
			b.WriteString(m.theme.UserLabel.Render("▶ " + usage))
		} else {
			b.WriteString("  " + usage)
		}
		b.WriteString(m.theme.Dim.Render(c.desc))
		if i < end-1 {
			b.WriteString("\n")
		}
	}
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(b.String())
}

// renderHelpOverlay renders the help dialog with a neutral border (the amber
// approval frame is reserved for risky actions), highlighted key names and a
// two-column key section to keep the dialog compact.
func (m Model) renderHelpOverlay() string {
	key := m.theme.ApprovalKey
	dim := m.theme.Dim

	var b strings.Builder
	b.WriteString(m.theme.DialogTitle.Render("Loom TUI Help"))
	b.WriteString("\n\n")

	b.WriteString(m.theme.DialogTitle.Render("Keyboard"))
	b.WriteString("\n")
	keyRow := func(k1, d1, k2, d2 string) {
		b.WriteString("  ")
		b.WriteString(key.Render(fmt.Sprintf("%-10s", k1)))
		b.WriteString(dim.Render(fmt.Sprintf("%-32s", d1)))
		if k2 != "" {
			b.WriteString(key.Render(fmt.Sprintf("%-10s", k2)))
			b.WriteString(dim.Render(d2))
		}
		b.WriteString("\n")
	}
	keyRow("Enter", "Send prompt", "Alt+Enter", "Newline in draft")
	keyRow("Up/Down", "Move in draft; scroll at edge", "PgUp/PgDn", "Scroll transcript")
	keyRow("Ctrl+End", "Jump to bottom (follow)", "Wheel", "Scroll transcript")
keyRow("Ctrl+R", "Toggle thought process", "Tab", "Complete /command")
keyRow("Ctrl+E", "Toggle tool output", "Ctrl+O", "Expand/collapse all tools")
keyRow("Ctrl+F", "Search transcript", "Ctrl+Y", "Copy last reply")
keyRow("Ctrl+C", "Cancel turn / clear (x2 quit)", "Ctrl+D", "Exit (when idle)")
	keyRow("Esc", "Cancel turn; close dialogs", "", "")
	b.WriteString("\n")

	b.WriteString(m.theme.DialogTitle.Render("Commands"))
	b.WriteString("\n")
	for _, c := range slashCommands {
		b.WriteString("  ")
		b.WriteString(key.Render(fmt.Sprintf("%-14s", c.usage)))
		b.WriteString(dim.Render(c.desc))
		b.WriteString("\n")
	}
	b.WriteString("\n")
	b.WriteString(dim.Render("Type / for command completion · press any key to close"))

	width := m.width - 2
	if width <= 0 || width > 76 {
		width = 76
	}
	width = max(width, 20)
	return m.theme.DialogBorder.Width(width).Render(b.String())
}

// Helper functions

func shortID(id fmt.Stringer) string {
	s := id.String()
	if len(s) > 12 {
		return s[:12]
	}
	return s
}

// truncateDisplayWidth shortens s to at most width display cells, appending an
// ellipsis. It walks runes once and assumes s contains no ANSI sequences.
func truncateDisplayWidth(s string, width int) string {
	if width <= 0 || lipgloss.Width(s) <= width {
		return s
	}
	if width <= 3 {
		return strings.Repeat(".", width)
	}
	limit := width - 3
	var b strings.Builder
	b.Grow(width)
	used := 0
	for _, r := range s {
		w := lipgloss.Width(string(r))
		if used+w > limit {
			break
		}
		b.WriteRune(r)
		used += w
	}
	return b.String() + "..."
}
