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
	"math"
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

// View renders the complete TUI: the base frame first, then — under the
// alt-screen renderer only — floating windows composed over it (docs/
// VIM_UI_DESIGN.md §7.1). Inline mode keeps the in-flow overlay layout
// because its line tracking cannot tolerate overlays.
func (m Model) View() string {
	// Until the first WindowSizeMsg arrives the terminal size is unknown.
	// Render a single harmless line: any larger first frame risks scrolling
	// the terminal, which permanently desynchronizes the inline renderer's
	// line tracking (this manifested as corrupted composer/dialog borders).
	if m.width <= 0 || m.height <= 0 {
		return m.theme.Dim.Render("Loom starting…")
	}
	base := m.renderBase()
	floats := m.activeFloats()
	if len(floats) == 0 {
		return base
	}
	return ComposeFloats(base, m.width, m.height, floats...)
}

// baseMode maps overlay modes onto what the base frame renders beneath
// them. With floats active (alt-screen), help, question and the pickers
// float over an ordinary chat frame; inline they own their area in-flow.
func (m Model) baseMode() Mode {
	if !m.altScreen {
		return m.mode
	}
	switch m.mode {
	case ModeHelp, ModeQuestion, ModeSessionPicker, ModeModelPicker, ModeReasoningPicker, ModeListing, ModeRules:
		return ModeChat
	}
	return m.mode
}

// activeFloats returns the floating windows for the current mode, in
// bottom-to-top composition order. Nil unless the alt-screen renderer is
// active.
func (m Model) activeFloats() []Float {
	if !m.altScreen {
		return nil
	}
	switch m.mode {
	case ModeHelp:
		return []Float{centeredFloat(m.renderHelpOverlay(), m.width, m.height)}
	case ModeListing:
		return []Float{centeredFloat(m.renderListingOverlay(), m.width, m.height)}
	case ModeQuestion:
		if m.choiceList != nil {
			return []Float{centeredFloat(m.renderQuestionOverlay(), m.width, m.height)}
		}
	case ModeSessionPicker:
		return pickerFloat(m, m.sessionFinder)
	case ModeModelPicker:
		return pickerFloat(m, m.modelFinder)
	case ModeReasoningPicker:
		return pickerFloat(m, m.reasoningFinder)
	case ModeRules:
		return m.rulesPickerFloat()
	}
	return nil
}

// pickerFloat frames a finder as a large centered float (snacks.picker's
// default big layout): wide enough for the list + preview panes, tall
// enough for a comfortable scroll window, clamped to the screen.
func pickerFloat[T any](m Model, f *Finder[T]) []Float {
	if f == nil {
		return nil
	}
	width := min(max(m.width*4/5, 62), 110)
	if width > m.width-2 {
		width = max(m.width-2, 20)
	}
	height := min(max(m.height*3/5, 10), 26)
	content := m.theme.DialogBorder.Width(width).Render(f.Render(width-4, height-2))
	return []Float{centeredFloat(content, m.width, m.height)}
}

// rulesPickerFloat renders the /rules picker, overlaying a delete
// confirmation prompt when active.
func (m Model) rulesPickerFloat() []Float {
	if m.rulesFinder == nil {
		return nil
	}
	innerW, innerH := m.rulesFinderDimensions()
	content := m.rulesFinder.Render(innerW, innerH)
	// Overlay the delete confirmation on the input line when pending.
	if m.rulesDeletePending != nil {
		prompt := rulesDeletePrompt(*m.rulesDeletePending)
		lines := strings.Split(content, "\n")
		if len(lines) > 0 {
			lines[0] = m.theme.DialogLabel.Render("❯ ") + prompt
			content = strings.Join(lines, "\n")
		}
	}
	// DialogBorder adds 4 cells horizontally (border + padding×2), so the
	// frame width is innerW+4 — same inset pickerFloat uses.
	bordered := m.theme.DialogBorder.Width(innerW + 4).Render(content)
	return []Float{centeredFloat(bordered, m.width, m.height)}
}

func (m Model) renderBase() string {
	var b strings.Builder

	// Header
	b.WriteString(m.renderHeader())
	b.WriteString("\n")

	baseMode := m.baseMode()
	if baseMode == ModeSessionPicker {
		b.WriteString(m.renderSessionPicker())
		b.WriteString("\n")
	} else if baseMode == ModeModelPicker {
		b.WriteString(m.renderModelPicker())
		b.WriteString("\n")
	} else if baseMode == ModeReasoningPicker {
		b.WriteString(m.renderReasoningPicker())
		b.WriteString("\n")
	} else if baseMode == ModeSubagent {
		b.WriteString(m.renderSubagentOverlay())
		b.WriteString("\n")
	} else {
		b.WriteString(m.renderTranscript())
		// One blank spacer row separates the main area from whatever
		// occupies the composer area (composer, panels, bands): the
		// last message must never glue to the composer border.
		b.WriteString("\n\n")
	}

	// Approval and help panels replace the composer area instead of being
	// appended below the status bar, so the layout never overflows the
	// terminal and the status bar stays visible.
	switch baseMode {
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
	case ModeListing:
		b.WriteString(m.renderListingOverlay())
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
		if indicator := m.renderAttachmentIndicator(); indicator != "" {
			b.WriteString(indicator)
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
	return renderFinderDialog(m, m.sessionFinder, "Loading sessions...")
}

func (m Model) renderModelPicker() string {
	return renderFinderDialog(m, m.modelFinder, "")
}

func (m Model) renderReasoningPicker() string {
	return renderFinderDialog(m, m.reasoningFinder, "")
}

// renderFinderDialog frames a finder in the neutral dialog border,
// filling the area the composer and transcript leave for picker modes.
func renderFinderDialog[T any](m Model, f *Finder[T], fallback string) string {
	if f == nil {
		return fallback
	}
	height := m.visibleTranscriptHeight()
	return m.theme.DialogBorder.Width(max(1, m.width-2)).Render(f.Render(m.width-6, height-2))
}

// dialogWidth returns the outer width for overlay dialogs: a centered
// float gets a comfortable fraction of the screen (alt-screen), while
// the in-flow band (inline) spans the full width.
func (m Model) dialogWidth() int {
	if m.altScreen {
		return min(max(m.width*3/4, 24), 76)
	}
	return max(1, m.width-2)
}

// renderQuestionOverlay renders the ask_user dialog. The choice list owns
// the content; the overlay just frames it like the approval dialog.
func (m Model) renderQuestionOverlay() string {
	if m.choiceList == nil {
		return ""
	}
	width := m.dialogWidth()
	return m.theme.DialogBorder.Width(width).Render(m.choiceList.Render(width-4, 0))
}

// questionOverlayHeight reserves the overlay's rendered height plus its
// border so the transcript above never gets overdrawn. Height() computes
// the line count arithmetically — rendering just to count newlines cost
// a full overlay render per frame.
func (m Model) questionOverlayHeight() int {
	if m.choiceList == nil {
		return 0
	}
	return m.choiceList.Height() + 2
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
		// Collapsed blocks keep the target on one bounded line; an expanded
		// block shows it in full (long run_cmd command lines) — expansion is
		// the explicit ask-for-more gesture, and the line simply wraps.
		target := block.Target
		if !block.Expanded {
			target = truncateDisplayWidth(target, 80)
		}
		parts = append(parts, target)
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
	case app.ControllerStateClosed, app.ControllerStateBooting:
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

	if ctx, warn := formatContext(m.contextOccupancy, m.contextWindow); ctx != "" {
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
// Layout: title + items + trailing blank; the leading blank above the
// panel is the global spacer row, reserved separately.
func (m Model) planPanelHeight() int {
	if len(m.plan.Items) == 0 || m.planHidden {
		return 0
	}
	items := min(len(m.plan.Items), planPanelMaxItems)
	if len(m.plan.Items) > items {
		items++ // the "… +N more" line
	}
	return items + 2 // title + items + trailing blank
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
//
// Geometry: title + items + one trailing blank row; the leading blank is
// the global spacer row written by renderBase (or the previous element's
// trailing blank).
func (m Model) renderSteerPanel() string {
	if len(m.pendingSteers) == 0 && len(m.pendingFollowups) == 0 {
		return ""
	}
	width := m.width - 2
	if width < 10 {
		width = 10
	}
	var b strings.Builder
	section := func(title string, items []string) {
		b.WriteString(m.theme.Dim.Render(title))
		for _, text := range items {
			b.WriteString("\n  " + m.theme.Dim.Render("↳ "+truncateDisplayWidth(strings.ReplaceAll(text, "\n", " "), width-6)))
		}
		b.WriteString("\n") // separate from the next element below
	}
	if len(m.pendingSteers) > 0 {
		section(fmt.Sprintf(
			"  Steering (%d queued — injects before next model call, Ctrl+C flushes now):", len(m.pendingSteers),
		), m.pendingSteers)
	}
	if len(m.pendingFollowups) > 0 {
		section(fmt.Sprintf(
			"  Followups (%d queued — each runs as its own turn after this one):", len(m.pendingFollowups),
		), m.pendingFollowups)
	}
	return b.String()
}

// steerPanelHeight returns the rows the steer panel occupies: per section,
// title + items + trailing blank (0 when hidden).
func (m Model) steerPanelHeight() int {
	height := 0
	if len(m.pendingSteers) > 0 {
		height += len(m.pendingSteers) + 2
	}
	if len(m.pendingFollowups) > 0 {
		height += len(m.pendingFollowups) + 2
	}
	return height
}

func (m Model) renderPlanPanel() string {
	if len(m.plan.Items) == 0 || m.planHidden {
		return ""
	}
	width := m.width - 2
	if width < 10 {
		width = 10
	}
	// Geometry: title + items + one trailing blank row; the leading blank
	// is the global spacer row (or the previous element's trailing
	// blank), so the panel no longer writes its own.
	var b strings.Builder
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
// The cache segment is the provider-metered session hit ratio
// (CachedInputTokens/ContextTokens), shown once a metered call exists.
func formatUsage(usage domain.Usage) string {
	label := fmt.Sprintf("turns:%d in:%s out:%s tools:%d",
		usage.Turns, humanizeTokens(usage.InputTokens), humanizeTokens(usage.OutputTokens), usage.ToolCalls)
	if usage.ContextTokens > 0 {
		label += fmt.Sprintf(" cache:%d%%", cacheHitPercent(usage))
	}
	return label
}

// cacheHitPercent reports the session cache-hit ratio in percent,
// rounded and clamped for sessions resumed from before ContextTokens
// was tracked.
func cacheHitPercent(usage domain.Usage) int {
	if usage.ContextTokens <= 0 {
		return 0
	}
	pct := int(math.Round(float64(usage.CachedInputTokens) / float64(usage.ContextTokens) * 100))
	return min(pct, 100)
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

// formatContext renders the ctx status segment: the calibrated occupancy
// of the next model request (context.usage events). warn reports occupancy
// ≥ 80% of the effective context window. Returns "" when nothing is known.
func formatContext(occupancy int64, contextWindow int) (string, bool) {
	if occupancy <= 0 {
		return "", false
	}
	label := "ctx:~" + humanizeTokens(occupancy)
	if contextWindow <= 0 {
		return label, false
	}
	label += "/" + humanizeTokens(int64(contextWindow))
	return label, float64(occupancy)/float64(contextWindow) >= 0.8
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

	// The consequence line renders what the operation DOES (the derived
	// effect), not just its text — the user's decision basis.
	if p.Consequence != "" {
		lines = append(lines, prefix+m.theme.Dim.Render(truncateDisplayWidth(p.Consequence, contentWidth)))
	}

	// The argument diff is rendered on the tool block (from tool.prepared),
	// not repeated here: the overlay stays compact enough to keep the
	// options on screen.
	lines = append(lines, bar)
	alwaysOK := p.RulePreview != ""
	alwaysLabel := "Always allow (not available for this call)"
	if alwaysOK {
		alwaysLabel = fmt.Sprintf("Always allow `%s`", p.RulePreview)
	}
	labels := []string{"Allow once", alwaysLabel}
	if p.TrustPreview != "" {
		labels = append(labels, fmt.Sprintf("Always TRUST `%s` — runs WITHOUT sandbox", p.TrustPreview))
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
	keyRow("Ctrl+V", "Paste image from clipboard", "Ctrl+G", "View sub-agent (read-only)")
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

	width := min(m.dialogWidth(), 76)
	width = max(width, 20)
	return m.theme.DialogBorder.Width(width).Render(b.String())
}

// listingMaxVisibleRows bounds the content window of the listing dialog so
// long lists scroll instead of walling off the transcript.
const listingMaxVisibleRows = 18

// renderListingOverlay renders the read-only listing dialog (/skill, /mcp):
// a titled, scrollable content window framed like the help dialog.
func (m Model) renderListingOverlay() string {
	dim := m.theme.Dim
	outer := m.listingDialogWidth()
	contentWidth := max(outer-2, 10) // dialog border columns

	var b strings.Builder
	b.WriteString(m.theme.DialogTitle.Render(m.listing.title))
	b.WriteString("\n\n")

	rows := m.listingRows(contentWidth)
	visible := m.listingVisibleRows()
	start := min(m.listingScroll, max(0, len(rows)-visible))
	end := min(start+visible, len(rows))
	for _, row := range rows[start:end] {
		b.WriteString(clampLineANSI(row, contentWidth))
		b.WriteString("\n")
	}
	// Pad short content so the dialog keeps a stable geometry.
	for i := end - start; i < visible; i++ {
		b.WriteString("\n")
	}
	b.WriteString("\n")

	footer := "up/down scroll · esc close"
	if len(rows) > visible {
		footer = fmt.Sprintf("up/down scroll (%d-%d/%d) · esc close", start+1, end, len(rows))
	}
	b.WriteString(dim.Render(footer))

	return m.theme.DialogBorder.Width(outer).Render(b.String())
}

// listingDialogWidth sizes the listing dialog: alt-screen gets a wide
// centered float (like the pickers); inline spans the frame width.
func (m Model) listingDialogWidth() int {
	if m.altScreen {
		width := min(max(m.width*4/5, 62), 110)
		return min(width, max(m.width-2, 20))
	}
	return max(1, m.width-2)
}

// listingRows lays out the listing payload as one styled row per line,
// each clipped to width cells by the caller (clampLineANSI), so the count
// is width-independent and matches listingContentRows.
func (m Model) listingRows(width int) []string {
	switch m.listing.kind {
	case listingSkills:
		return m.listingSkillRows(width)
	case listingMCP:
		return m.listingMCPRows(width)
	case listingEnv:
		return m.listingEnvRows(width)
	}
	return nil
}

// listingSkillRows renders one card per skill: name + scope on the first
// row, the cleaned description and the SKILL.md path indented below it.
func (m Model) listingSkillRows(width int) []string {
	key := m.theme.ApprovalKey
	dim := m.theme.Dim
	listing := m.listing.skills
	if len(listing.Skills) == 0 {
		return []string{
			dim.Render("  No skills discovered."),
			"",
			dim.Render("  Skills are discovered under:"),
			dim.Render("    <workspace>/.loom/skills  or  <workspace>/.agents/skills"),
			dim.Render("    ~/.loom/skills  or  ~/.agents/skills"),
			dim.Render("    (or skills.extra_roots in config)"),
		}
	}
	var rows []string
	for i, s := range listing.Skills {
		if i > 0 {
			rows = append(rows, "")
		}
		rows = append(
			rows,
			"  "+key.Render(truncateDisplayWidth(s.Name, width-10))+" "+dim.Render("· "+s.Scope),
			"    "+dim.Render(truncateDisplayWidth(cleanListingText(s.Description), width-4)),
			"    "+dim.Render(truncateDisplayWidth(s.Path, width-4)),
		)
	}
	if len(listing.Issues) > 0 {
		rows = append(rows, "", m.theme.DialogTitle.Render("  Load issues"))
		for _, issue := range listing.Issues {
			rows = append(rows, "  "+m.theme.ToolError.Render("x "+truncateDisplayWidth(issue, width-6)))
		}
	}
	return rows
}

// listingEnvRows renders the /doctor report: key-tool resolutions first,
// then every candidate PATH directory with its source/status attribution,
// then the effective PATH. The layout mirrors the settings "development
// environment" card so both frontends answer the same question.
func (m Model) listingEnvRows(width int) []string {
	key := m.theme.ApprovalKey
	dim := m.theme.Dim
	report := m.listing.env
	if report == nil {
		return []string{dim.Render("  Environment report unavailable.")}
	}
	var rows []string
	rows = append(rows, m.theme.DialogTitle.Render("  Key tools"))
	for _, t := range report.Tools {
		if t.Found {
			rows = append(rows, "  "+m.theme.ToolSuccess.Render("+ ")+key.Render(t.Name)+"  "+dim.Render(truncateDisplayWidth(t.Path, width-12)))
		} else {
			rows = append(rows, "  "+m.theme.ToolError.Render("x ")+key.Render(t.Name)+"  "+dim.Render("not found"))
		}
	}
	rows = append(rows, "", m.theme.DialogTitle.Render("  PATH directories (by priority)"))
	if len(report.Dirs) == 0 {
		rows = append(rows, dim.Render("  (no candidates)"))
	}
	for _, d := range report.Dirs {
		mark := m.theme.ToolSuccess.Render("+ ")
		switch d.Status {
		case "missing":
			mark = m.theme.ToolError.Render("x ")
		case "existing":
			mark = dim.Render("= ")
		}
		meta := string(d.Source) + "/" + string(d.Status)
		rows = append(rows, "  "+mark+dim.Render(truncateDisplayWidth(d.Path, width-22)+"  ("+meta+")"))
	}
	if report.EffectivePATH != "" {
		rows = append(rows, "", m.theme.DialogTitle.Render("  Effective PATH"))
		for _, entry := range strings.Split(report.EffectivePATH, ":") {
			rows = append(rows, "    "+dim.Render(truncateDisplayWidth(entry, width-4)))
		}
	}
	return rows
}

// listingMCPRows renders one card per server: name + tool count (or the
// startup error) on the first row, details indented below it.
func (m Model) listingMCPRows(width int) []string {
	key := m.theme.ApprovalKey
	dim := m.theme.Dim
	servers := m.listing.servers
	if len(servers) == 0 {
		return []string{
			dim.Render("  No MCP servers configured."),
			"",
			dim.Render("  Add mcp_servers entries to ~/.loom/config.yaml."),
		}
	}
	var rows []string
	for i, srv := range servers {
		if i > 0 {
			rows = append(rows, "")
		}
		if srv.Connected {
			header := fmt.Sprintf("%s · %d tool%s", srv.Name, len(srv.Tools), pluralS(len(srv.Tools)))
			rows = append(
				rows,
				"  "+m.theme.ToolSuccess.Render("+ ")+key.Render(header),
				"    "+dim.Render(truncateDisplayWidth(strings.Join(srv.Tools, ", "), width-4)),
			)
			continue
		}
		errMsg := srv.Error
		if errMsg == "" {
			errMsg = "unknown error"
		}
		rows = append(
			rows,
			"  "+m.theme.ToolError.Render("x ")+key.Render(srv.Name),
			"    "+m.theme.ToolError.Render(truncateDisplayWidth(errMsg, width-4)),
		)
	}
	return rows
}

// listingContentRows returns the number of content rows the listing dialog
// renders for the current payload (width-independent: every field is one
// clipped row), shared by the scroll handlers and the renderer.
func (m Model) listingContentRows() int {
	switch m.listing.kind {
	case listingSkills:
		n := len(m.listing.skills.Skills)
		if n == 0 {
			return 6
		}
		rows := n*3 + (n - 1) // card rows + blank separators
		if issues := len(m.listing.skills.Issues); issues > 0 {
			rows += 2 + issues // blank + header + issue rows
		}
		return rows
	case listingMCP:
		n := len(m.listing.servers)
		if n == 0 {
			return 3
		}
		return n*2 + (n - 1) // 2 rows per server + blank separators
	}
	return 0
}

// listingVisibleRows returns how many content rows the dialog window shows.
func (m Model) listingVisibleRows() int {
	return min(max(m.listingContentRows(), 1), listingMaxVisibleRows)
}

// listingOverlayHeight is the dialog's rendered height including its
// border, reserved in-flow by the inline layout (alt-screen composes the
// dialog as a float instead).
func (m Model) listingOverlayHeight() int {
	return m.listingVisibleRows() + 6 // title + 2 blanks + footer + border
}

// clampLineANSI clips a styled row to width display cells, appending an
// ellipsis when content was cut. ANSI-aware via truncateANSI, so escape
// sequences are never split and lipgloss never re-wraps the row.
func clampLineANSI(s string, width int) string {
	if lipgloss.Width(s) <= width {
		return s
	}
	if width < 2 {
		return truncateANSI(s, width)
	}
	return truncateANSI(s, width-1) + "…"
}

// cleanListingText flattens a free-form description onto one line: markdown
// emphasis markers are dropped and all whitespace (including newlines) is
// collapsed to single spaces.
func cleanListingText(s string) string {
	s = strings.ReplaceAll(s, "**", "")
	return strings.Join(strings.Fields(s), " ")
}

// attachmentIndicatorHeight returns the rows reserved for the image
// attachment indicator line above the composer (0 when no images are
// attached or loading).
func (m Model) attachmentIndicatorHeight() int {
	if len(m.attachedImages) == 0 && m.pendingSubmitAttachTotal == 0 {
		return 0
	}
	return 1
}

// renderAttachmentIndicator renders a compact line above the composer
// showing attached image files. While images are loading, it shows
// progress; once loaded, it shows the filenames. The indicator is
// hidden when no images are attached.
func (m Model) renderAttachmentIndicator() string {
	if len(m.attachedImages) == 0 && m.pendingSubmitAttachTotal == 0 {
		return ""
	}
	width := m.width - 2
	if width < 10 {
		width = 10
	}
	icons := m.iconSet()
	if m.pendingSubmitAttachTotal > 0 && m.pendingSubmitAttachDone < m.pendingSubmitAttachTotal {
		// Loading in progress
		label := fmt.Sprintf("%s Loading images %d/%d", icons.Attachment, m.pendingSubmitAttachDone, m.pendingSubmitAttachTotal)
		return m.theme.Dim.Render(truncateDisplayWidth(label, width))
	}
	// All loaded — show filenames
	var names []string
	for _, p := range m.attachedPaths {
		names = append(names, filepath.Base(p))
	}
	label := fmt.Sprintf("%s %d image%s: %s", icons.Attachment, len(names), pluralS(len(names)), strings.Join(names, ", "))
	return m.theme.Dim.Render(truncateDisplayWidth(label, width))
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
