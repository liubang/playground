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
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/charmbracelet/lipgloss"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/render"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Reserved height (including border and padding) for the help panel that
// replaces the composer. The approval band reserves its actual line count
// instead (see visibleTranscriptHeight).
const helpOverlayHeight = 23

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
	} else if m.mode == ModeModelPicker {
		b.WriteString(m.renderModelPicker())
		b.WriteString("\n")
	} else if m.mode == ModeReasoningPicker {
		b.WriteString(m.renderReasoningPicker())
		b.WriteString("\n")
	} else if m.mode == ModeSubagent {
		b.WriteString(m.renderSubagentOverlay())
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
	case ModeQuestion:
		if m.choiceList != nil {
			b.WriteString(m.renderQuestionOverlay())
			b.WriteString("\n")
		}
	case ModeHelp:
		b.WriteString(m.renderHelpOverlay())
		b.WriteString("\n")
	case ModeSearch:
		b.WriteString(m.renderSearchBar())
		b.WriteString("\n")
	case ModeSessionPicker, ModeModelPicker, ModeReasoningPicker, ModeSubagent:
		// The picker/overlay owns the main area; no composer.
	default:
		// Pinned panels sit directly above the composer (Claude Code
		// style): steer queue first, then the plan checklist.
		if panel := m.renderSteerPanel(); panel != "" {
			b.WriteString(panel)
			b.WriteString("\n")
		}
		if panel := m.renderPlanPanel(); panel != "" {
			b.WriteString(panel)
			b.WriteString("\n")
		}
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

// renderHeader renders the one-line accent band at the top of the frame.
// The left side carries the brand and model name (bold); the right side
// carries the working context — git branch and an abbreviated workspace
// path — in the same band but unbolded, so the brand keeps visual
// priority. The session id deliberately no longer appears here: it is
// debugging detail with no day-to-day value (the session picker still
// lists ids when one is actually needed).
func (m Model) renderHeader() string {
	if m.width < 30 {
		style := m.theme.HeaderStyle
		if m.width > 0 {
			style = style.Width(m.width)
		}
		return style.Render("Loom")
	}

	left := "Loom"
	if m.modelName != "" {
		left += " · " + m.modelName
	}
	if m.reasoningEffort != "" {
		dial := "think:" + m.reasoningEffort
		if m.reasoningOverridden {
			dial += "*" // session override, not the model's configured default
		}
		left += " · " + dial
	}

	// Strip the placeholder width from the shared style; every path below
	// either sizes the segments individually or sets an explicit width.
	base := m.theme.HeaderStyle.Width(0)
	leftSeg := base.Render(left)

	// Budget for the context text: the columns left after the rendered
	// left segment, the right segment's own padding, and a 2-cell gap.
	right := m.headerContext(m.width - lipgloss.Width(leftSeg) - 4)
	if right == "" {
		// No context fits (or there is none): a single truncated title.
		// Truncate before styling: lipgloss wraps overlong text onto a
		// second line, but the layout reserves exactly one row for the
		// header. A wrapped header makes the frame taller than the
		// terminal and corrupts the renderer's line tracking.
		title := truncateDisplayWidth(left, m.width-2)
		return base.Width(m.width).Render(title)
	}

	rightSeg := base.Bold(false).Render(right)
	gap := m.width - lipgloss.Width(leftSeg) - lipgloss.Width(rightSeg)
	// gap >= 2 by construction (the budget above reserves it); clamp only
	// to keep strings.Repeat safe on degenerate geometries.
	if gap < 1 {
		gap = 1
	}
	fill := base.Bold(false).Padding(0).Render(strings.Repeat(" ", gap))
	return leftSeg + fill + rightSeg
}

// headerContext picks the richest working-context string that fits the
// budget (in display cells): branch plus full path first, degrading
// through progressively shorter paths, then path-only variants, then the
// bare branch, and finally a truncated basename. An empty result means
// nothing meaningful fits, so the header falls back to the title alone.
func (m Model) headerContext(budget int) string {
	if budget <= 0 {
		return ""
	}
	branch := m.gitBranch
	if icon := m.iconSet().Branch; icon != "" && branch != "" {
		branch = icon + " " + branch
	}
	full := abbreviateHome(m.workspace)
	base := filepath.Base(m.workspace)
	short := shortenPath(full)

	var candidates []string
	if branch != "" && m.workspace != "" {
		candidates = append(
			candidates,
			branch+" · "+full,
			branch+" · "+short,
			branch+" · "+base,
		)
	}
	if m.workspace != "" {
		candidates = append(candidates, full, short, base)
	}
	if branch != "" {
		candidates = append(candidates, branch)
	}
	for _, c := range candidates {
		if lipgloss.Width(c) <= budget {
			return c
		}
	}
	// Below a handful of cells a truncated basename is unreadable noise;
	// the band is better off showing the title alone.
	if m.workspace != "" && budget >= 4 {
		return truncateDisplayWidth(base, budget)
	}
	return ""
}

// abbreviateHome rewrites the user's home directory prefix as ~, so the
// workspace path spends its width on the components that identify it.
func abbreviateHome(path string) string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return path
	}
	if path == home {
		return "~"
	}
	prefix := home + string(filepath.Separator)
	if strings.HasPrefix(path, prefix) {
		return "~" + path[len(home):]
	}
	return path
}

// shortenPath abbreviates every interior path component to its first rune
// (fish-shell style), keeping the last component whole so the directory
// that matters stays readable: ~/workspace/github/loom → ~/w/g/loom.
// Hidden interior components keep their dot: .config → .c.
func shortenPath(path string) string {
	sep := string(filepath.Separator)
	parts := strings.Split(path, sep)
	for i, p := range parts {
		if p == "" || p == "~" || i == len(parts)-1 {
			continue
		}
		if rest, found := strings.CutPrefix(p, "."); found {
			if r, _ := utf8.DecodeRuneInString(rest); r != utf8.RuneError {
				parts[i] = "." + string(r)
			}
			continue
		}
		if r, _ := utf8.DecodeRuneInString(p); r != utf8.RuneError {
			parts[i] = string(r)
		}
	}
	return strings.Join(parts, sep)
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
	// One row of breathing room: the wordmark must not touch the header
	// band, especially since both are accent-colored.
	b.WriteString("\n")
	logoColors := gradientColors(m.theme.Highlight, m.theme.Warning, len(welcomeLogo))
	logoStyle := lipgloss.NewStyle().Bold(true)
	if m.width >= 44 {
		for i, line := range welcomeLogo {
			style := logoStyle
			if logoColors != nil {
				style = style.Foreground(logoColors[i])
			} else {
				style = style.Foreground(m.theme.Highlight)
			}
			b.WriteString(style.Render(line))
			b.WriteString("\n")
		}
	} else {
		b.WriteString(logoStyle.Foreground(m.theme.Highlight).Render("Loom"))
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

func (m Model) renderModelPicker() string {
	if m.modelPicker == nil {
		return ""
	}
	height := m.visibleTranscriptHeight()
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(m.modelPicker.Render(m.width-6, height-2))
}

func (m Model) renderReasoningPicker() string {
	if m.reasoningPicker == nil {
		return ""
	}
	height := m.visibleTranscriptHeight()
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(m.reasoningPicker.Render(m.width-6, height-2))
}

// renderQuestionOverlay renders the ask_user dialog. The choice list owns
// the content; the overlay just frames it like the approval dialog.
func (m Model) renderQuestionOverlay() string {
	if m.choiceList == nil {
		return ""
	}
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(m.choiceList.Render(m.width-6, 0))
}

// questionOverlayHeight reserves the overlay's rendered height plus its
// border so the transcript above never gets overdrawn.
func (m Model) questionOverlayHeight() int {
	if m.choiceList == nil {
		return 0
	}
	return strings.Count(m.choiceList.Render(m.width-6, 0), "\n") + 1 + 2
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
		if progress := m.renderSubagentProgress(block); progress != "" {
			out += "\n" + progress
		}
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
		// The expanded thinking gets a panel background with a primary left
		// bar: backstage material must be visually distinct from delivered
		// output so it is never mistaken for the answer.
		width := max(m.width-4, 20)
		return m.theme.ReasoningBlock.Width(width).Render("Thinking:\n"+render.SanitizeText(block.StreamReasoning)) + "\n"
	}
	if !block.Done {
		return m.spinnerView() + " " + m.theme.Dim.Render("Thinking... (click or Ctrl+R to expand)") + "\n"
	}
	return m.theme.Dim.Render("Thought process hidden (click or Ctrl+R to expand)") + "\n"
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

	usage := formatUsage(m.usage)
	usageStyle := m.theme.Dim
	if budgetUsageRatio(m.usage, m.limits) >= 0.8 {
		// Approaching the wall-time/cost budget: the soft-landing wrap-up is near.
		usageStyle = lipgloss.NewStyle().Foreground(m.theme.Warning)
	}
	add(usage, usageStyle.Render(usage))

	if ctx, warn := formatContext(m.contextEst, m.lastCallInput, m.contextWindow); ctx != "" {
		ctxStyle := m.theme.Dim
		if warn {
			ctxStyle = lipgloss.NewStyle().Foreground(m.theme.Warning)
		}
		add(ctx, ctxStyle.Render(ctx))
	}

	if m.compactions > 0 {
		cpt := fmt.Sprintf("compact:%d", m.compactions)
		add(cpt, m.theme.Dim.Render(cpt))
	}

	if len(m.plan.Items) > 0 {
		seg := fmt.Sprintf("plan:%d/%d (ctrl+t:hide)", planDoneCount(m.plan), len(m.plan.Items))
		add(seg, m.theme.Dim.Render(seg))
	}

	// Whenever the view sits above the transcript tail, say so explicitly —
	// a window that happens to end right at the last user echo is
	// indistinguishable from "no reply yet", which reads as a lost reply.
	if !m.viewport.AtBottom() {
		hint := "scrolled · ctrl+end for latest"
		if m.newEvents > 0 {
			hint = fmt.Sprintf("↓%d new · ctrl+end for latest", m.newEvents)
		}
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

// planPanelMaxItems caps the checklist rows shown in the pinned panel; a
// longer plan collapses into a "+N more" line.
const planPanelMaxItems = 6

// planPanelHeight returns the rows reserved for the pinned plan panel above
// the composer (0 when there is no plan or it is collapsed via ctrl+t).
// Layout: blank, title, items, blank — the blank rows keep the panel from
// gluing to the transcript above or the composer below.
func (m Model) planPanelHeight() int {
	if len(m.plan.Items) == 0 || m.planHidden {
		return 0
	}
	items := min(len(m.plan.Items), planPanelMaxItems)
	if len(m.plan.Items) > items {
		items++ // the "… +N more" line
	}
	return items + 3 // blank + title + items + blank
}

// renderPlanPanel renders the pinned task-plan checklist, Claude Code style:
// a one-line title carrying the live activity (spinner + current action +
// elapsed; falling back to the static N/M summary when idle), then the
// steps indented under it like a tree — done steps dimmed, the in-progress
// step highlighted — with a blank row on each side.
// renderSteerPanel renders the pinned pending-steer list directly above
// the composer (codex's PendingInputPreview equivalent): messages the user
// submitted while a turn was busy, waiting for the loop to inject them
// before its next model call. Hidden when empty.
func (m Model) renderSteerPanel() string {
	if len(m.pendingSteers) == 0 {
		return ""
	}
	width := m.width - 2
	if width < 10 {
		width = 10
	}
	var b strings.Builder
	b.WriteString("\n")
	b.WriteString(m.theme.Dim.Render(fmt.Sprintf(
		"  Steering (%d queued — injects before next model call, Ctrl+C flushes now):", len(m.pendingSteers),
	)))
	for _, text := range m.pendingSteers {
		b.WriteString("\n  " + m.theme.Dim.Render("↳ "+truncateDisplayWidth(strings.ReplaceAll(text, "\n", " "), width-6)))
	}
	return b.String()
}

func (m Model) renderPlanPanel() string {
	if len(m.plan.Items) == 0 || m.planHidden {
		return ""
	}
	width := m.width - 2
	if width < 10 {
		width = 10
	}
	var b strings.Builder
	b.WriteString("\n") // separate from the transcript above
	b.WriteString(m.planPanelTitle(width))
	icons := m.iconSet()
	shown := min(len(m.plan.Items), planPanelMaxItems)
	for i := 0; i < shown; i++ {
		item := m.plan.Items[i]
		mark := icons.PlanTodo
		style := lipgloss.NewStyle()
		switch item.Status {
		case domain.PlanItemDone:
			mark = icons.PlanDone
			style = m.theme.Dim
		case domain.PlanItemInProgress:
			mark = icons.PlanCurrent
			style = lipgloss.NewStyle().Foreground(m.theme.Highlight)
		}
		// Items indent two columns under the title; a tree stub on the first
		// row draws the parent→children grouping while keeping every step's
		// mark glyph on the same column.
		indent := "  "
		if i == 0 {
			indent = "└ "
		}
		b.WriteString("\n" + indent + style.Render(truncateDisplayWidth(mark+" "+item.Goal, width-3)))
	}
	if len(m.plan.Items) > shown {
		b.WriteString("\n  " + m.theme.Dim.Render(fmt.Sprintf("… +%d more", len(m.plan.Items)-shown)))
	}
	b.WriteString("\n") // separate from the composer below
	return b.String()
}

// planPanelTitle renders the panel's title row: the model-authored short
// title of the whole plan, falling back to the progress summary when the
// plan has no title yet.
func (m Model) planPanelTitle(width int) string {
	title := strings.TrimSpace(m.plan.Title)
	if title == "" {
		title = fmt.Sprintf("plan · %d/%d done", planDoneCount(m.plan), len(m.plan.Items))
	}
	return m.theme.Dim.Render(truncateDisplayWidth(title, width))
}

// formatUsage renders the status-bar usage segment. Token counts are
// session-cumulative counters — the tokens budget denominators
// (docs/CONTEXT_DESIGN.md §4.4.3: context pressure is shown by the ctx
// segment against the effective window; tokens/cost are the budgets).
func formatUsage(usage domain.Usage) string {
	return fmt.Sprintf("turns:%d in:%s out:%s tools:%d",
		usage.Turns, humanizeTokens(usage.InputTokens), humanizeTokens(usage.OutputTokens), usage.ToolCalls)
}

// budgetUsageRatio reports the highest consumption ratio across the real
// budget dimensions (session tokens, cost); it is 0 when neither is
// configured. The status bar warns at ≥80% — the graduated-notice band.
func budgetUsageRatio(usage domain.Usage, limits domain.Limits) float64 {
	ratio := 0.0
	if limits.MaxTokens > 0 {
		ratio = max(ratio, float64(usage.InputTokens+usage.OutputTokens)/float64(limits.MaxTokens))
	}
	if limits.MaxEstimatedCostUSD > 0 && usage.CostUSD > 0 {
		ratio = max(ratio, usage.CostUSD/limits.MaxEstimatedCostUSD)
	}
	return ratio
}

// formatContext renders the ctx status segment: the estimated size of the
// next model request, falling back to the provider-metered input of the last
// call when no estimate exists yet. warn reports occupancy ≥ 80% of the
// configured context window. Returns "" when nothing is known.
func formatContext(estTokens int, lastCallInput int64, contextWindow int) (string, bool) {
	current := int64(estTokens)
	if current <= 0 {
		current = lastCallInput
	}
	if current <= 0 {
		return "", false
	}
	label := "ctx:~" + humanizeTokens(current)
	if contextWindow <= 0 {
		return label, false
	}
	label += "/" + humanizeTokens(int64(contextWindow))
	return label, float64(current)/float64(contextWindow) >= 0.8
}

// humanizeTokens renders token counts compactly: 999 → "999",
// 6095 → "6.1k", 212456 → "212k", 2_500_000 → "2.5M".
func humanizeTokens(n int64) string {
	switch {
	case n < 1000:
		return fmt.Sprintf("%d", n)
	case n < 10_000:
		return fmt.Sprintf("%.1fk", float64(n)/1000)
	case n < 1_000_000:
		return fmt.Sprintf("%dk", n/1000)
	default:
		return fmt.Sprintf("%.1fM", float64(n)/1_000_000)
	}
}

// renderApprovalOverlay renders the approval prompt as a full-width band
// rather than a boxed dialog: a risk-colored bar down the left edge, a
// title rule, the call summary with structured metadata, and a vertical
// numbered option list. It shares the full-width language of the header
// and status bar, and gives long commands and URLs room to breathe.
func (m Model) renderApprovalOverlay() string {
	return strings.Join(m.approvalOverlayLines(), "\n")
}

// approvalOverlayLines builds the band line by line. The layout reserves
// exactly len(lines) rows for the panel, so the status bar hugs the bottom
// of the terminal no matter how compact the prompt is — a fixed
// reservation used to leave a gap under short prompts.
func (m Model) approvalOverlayLines() []string {
	p := m.pendingApproval
	if p == nil {
		return nil
	}

	bar := m.approvalBarStyle(p.Risk).Render("▍")
	prefix := bar + " "
	contentWidth := max(m.width-4, 20) // bar, its trailing space, one margin

	lines := []string{prefix + m.approvalTitleLine(p, contentWidth), bar}

	desc := parseApprovalDesc(render.SanitizeText(p.Description))
	summary := lipgloss.NewStyle().Bold(true).Render(p.ToolName)
	if desc.action != "" {
		summary += m.theme.Dim.Render(" · ") + truncateDisplayWidth(desc.action, contentWidth-lipgloss.Width(p.ToolName)-3)
	}
	lines = append(lines, prefix+summary)
	if len(desc.meta) > 0 {
		lines = append(lines, prefix+m.theme.Dim.Render(truncateDisplayWidth(strings.Join(desc.meta, " · "), contentWidth)))
	}
	if desc.note != "" {
		note := desc.note
		if desc.escalated {
			note = "runs WITHOUT the sandbox — " + note
		}
		lines = append(lines, prefix+lipgloss.NewStyle().Foreground(m.theme.Warning).Render(truncateDisplayWidth(note, contentWidth)))
	}
	lines = append(lines, m.approvalPathLines(p, prefix, contentWidth)...)

	// The argument diff is the primary evidence for the allow/deny decision
	// on file-editing calls; it gets the remaining vertical budget.
	if p.Diff != "" {
		lines = append(lines, bar)
		for _, line := range strings.Split(m.renderDiff(headLines(p.Diff, approvalDiffMaxLines)), "\n") {
			lines = append(lines, prefix+line)
		}
	}

	lines = append(lines, bar)
	alwaysRule, grant, alwaysOK := app.ApprovalRulePreview(p.ToolName, p.Arguments)
	alwaysLabel := "Always allow (not available for this call)"
	if alwaysOK {
		alwaysLabel = fmt.Sprintf("Always allow `%s`", alwaysRule)
		if summary := grant.Summary(); summary != "" {
			alwaysLabel += fmt.Sprintf(" (%s)", summary)
		}
	}
	labels := []string{"Allow once", alwaysLabel}
	if app.RunCmdTrustPreview(p.ToolName, p.Arguments) {
		labels = append(labels, fmt.Sprintf("Always TRUST `%s` — runs WITHOUT sandbox", alwaysRule))
	}
	labels = append(labels, "Deny")
	for i, label := range labels {
		lines = append(lines, prefix+m.approvalOptionLine(i, label, i == 1 && !alwaysOK))
	}

	lines = append(lines, bar)
	key := m.theme.ApprovalKey
	quick := "y/a/n"
	if len(labels) == 4 {
		quick = "y/a/t/n"
	}
	hint := strings.Join([]string{
		key.Render("↑/↓") + " select",
		key.Render(fmt.Sprintf("1-%d/Enter", len(labels))) + " confirm",
		key.Render(quick) + " quick",
		key.Render("Esc") + " deny",
		key.Render("Ctrl+C") + " deny+cancel",
	}, m.theme.Dim.Render(" · "))
	lines = append(lines, prefix+hint)
	return lines
}

// approvalTitleLine renders the band header: the warning title on the left,
// the risk badge on the right, joined by a dim rule across the full width.
func (m Model) approvalTitleLine(p *runtimeevent.ApprovalRequestedPayload, width int) string {
	title := m.theme.ApprovalTitle.Render(m.iconSet().Warning + " Approval Required")
	badge := m.riskBadge(p.Risk)
	gap := width - lipgloss.Width(title) - lipgloss.Width(badge) - 2
	if gap < 4 {
		return title + "  " + badge
	}
	return title + " " + m.theme.Dim.Render(strings.Repeat("─", gap)) + " " + badge
}

// approvalBarStyle colors the band's left bar by risk: red for destructive
// calls, yellow for writes, green for read-only — the danger is visible
// before the user reads a single word.
func (m Model) approvalBarStyle(risk domain.RiskLevel) lipgloss.Style {
	switch {
	case risk >= domain.R3:
		return lipgloss.NewStyle().Foreground(m.theme.Error)
	case risk == domain.R2:
		return lipgloss.NewStyle().Foreground(m.theme.Warning)
	default:
		return lipgloss.NewStyle().Foreground(m.theme.Success)
	}
}

// approvalOptionLine renders one row of the vertical option list: a ❯
// cursor and reverse video on the selected row, dim on disabled rows.
func (m Model) approvalOptionLine(index int, label string, disabled bool) string {
	text := fmt.Sprintf("%d. %s", index+1, label)
	switch {
	case m.approvalCursor == index:
		return "❯ " + m.theme.ApprovalSelected.Render(text)
	case disabled:
		return m.theme.Dim.Render("  " + text)
	default:
		return "  " + m.theme.ApprovalOption.Render(text)
	}
}

// approvalPathLines renders the Reads/Writes rows, collapsing identical
// path sets and relativizing everything against the workspace root so an
// approval for a workspace-scoped call reads "workspace (.)" instead of a
// long absolute path.
func (m Model) approvalPathLines(p *runtimeevent.ApprovalRequestedPayload, prefix string, width int) []string {
	label := m.theme.DialogLabel
	renderRow := func(name string, paths []string) string {
		shown := make([]string, 0, len(paths))
		for _, path := range paths {
			shown = append(shown, m.approvalPathDisplay(path))
		}
		// 13 columns: the longest label ("Reads/Writes") is 12, keep one
		// space of separation before the value.
		row := label.Render(fmt.Sprintf("%-13s", name)) + truncateDisplayWidth(render.SanitizeText(strings.Join(shown, ", ")), width-13)
		return prefix + row
	}
	reads, writes := strings.Join(p.ReadPaths, "\x00"), strings.Join(p.WritePaths, "\x00")
	if reads != "" && reads == writes {
		return []string{renderRow("Reads/Writes", p.ReadPaths)}
	}
	var lines []string
	if len(p.ReadPaths) > 0 {
		lines = append(lines, renderRow("Reads", p.ReadPaths))
	}
	if len(p.WritePaths) > 0 {
		lines = append(lines, renderRow("Writes", p.WritePaths))
	}
	return lines
}

// approvalPathDisplay renders a path relative to the workspace when it is
// inside it; the root itself collapses to "workspace (.)".
func (m Model) approvalPathDisplay(path string) string {
	if m.workspace == "" {
		return path
	}
	clean := filepath.Clean(path)
	root := filepath.Clean(m.workspace)
	if clean == root {
		return "workspace (.)"
	}
	if rel, err := filepath.Rel(root, clean); err == nil && rel != ".." && !strings.HasPrefix(rel, "../") {
		return rel
	}
	return path
}

// approvalDescParts is a tool approval description split into the primary
// action, secondary metadata, and the model's justification note.
type approvalDescParts struct {
	action    string
	meta      []string
	note      string
	escalated bool
}

// parseApprovalDesc splits a structured approval description ("Run; 'cmd';
// env[none]; cwd='.'; timeout=…; network=…; note[…]; args_hash=…") into
// displayable pieces. The args_hash segment is audit plumbing and dropped;
// unknown formats fall back to a single action line.
func parseApprovalDesc(desc string) approvalDescParts {
	var out approvalDescParts
	if desc == "" {
		return out
	}
	var actions []string
	sawNetwork := false
	for _, seg := range strings.Split(desc, "; ") {
		seg = strings.TrimSpace(seg)
		switch {
		case seg == "":
		case strings.HasPrefix(seg, "env["):
			keys := strings.TrimSuffix(strings.TrimPrefix(seg, "env["), "]")
			if keys != "" && keys != "none" {
				out.meta = append(out.meta, "env="+keys)
			}
		case strings.HasPrefix(seg, "cwd="):
			out.meta = append(out.meta, "cwd="+strings.Trim(strings.TrimPrefix(seg, "cwd="), "'"))
		case strings.HasPrefix(seg, "timeout="):
			out.meta = append(out.meta, seg)
		case strings.HasPrefix(seg, "network="):
			sawNetwork = true
			out.meta = append(out.meta, seg)
		case seg == "shell=R3":
			out.meta = append(out.meta, "shell (elevated)")
		case strings.HasPrefix(seg, "args_hash="):
		case strings.HasPrefix(seg, "ESCALATED(no-sandbox)["):
			out.escalated = true
			out.note = strings.TrimSuffix(strings.TrimPrefix(seg, "ESCALATED(no-sandbox)["), "]")
		case strings.HasPrefix(seg, "note["):
			out.note = strings.TrimSuffix(strings.TrimPrefix(seg, "note["), "]")
		default:
			actions = append(actions, seg)
		}
	}
	out.action = strings.Join(actions, " ")
	if out.action == "" {
		out.action = desc
	}
	// run_cmd descriptions always carry the network segment; use it as the
	// signal to annotate the sandbox mode.
	if sawNetwork {
		if out.escalated {
			out.meta = append(out.meta, "sandbox=off")
		} else {
			out.meta = append(out.meta, "sandboxed")
		}
	}
	return out
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
		usage := fmt.Sprintf("%-*s", commandUsageWidth(), c.usage)
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

// commandUsageWidth is the shared usage-column width for the completion
// popup and the help overlay: the widest registered usage plus padding, so
// descriptions stay aligned regardless of the active completion filter.
func commandUsageWidth() int {
	width := 0
	for _, c := range slashCommands {
		if len(c.usage) > width {
			width = len(c.usage)
		}
	}
	return width + 2
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
	keyRow("Ctrl+G", "View sub-agent (read-only)", "Click", "Delegate block: view sub-agent")
	keyRow("Ctrl+C", "Cancel turn / clear (x2 quit)", "Ctrl+D", "Exit (when idle)")
	keyRow("Esc", "Cancel turn; close dialogs", "", "")
	b.WriteString("\n")

	b.WriteString(m.theme.DialogTitle.Render("Commands"))
	b.WriteString("\n")
	for _, c := range slashCommands {
		b.WriteString("  ")
		b.WriteString(key.Render(fmt.Sprintf("%-*s", commandUsageWidth(), c.usage)))
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
