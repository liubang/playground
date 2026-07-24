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
	"github.com/charmbracelet/lipgloss"
)

// Theme holds all visual styling for the TUI.
type Theme struct {
	// Header
	HeaderStyle lipgloss.Style

	// Transcript blocks
	UserBlock        lipgloss.Style
	AssistantBlock   lipgloss.Style
	ToolBlock        lipgloss.Style
	NoticeBlock      lipgloss.Style
	StreamBlock      lipgloss.Style
	InterruptedBlock lipgloss.Style

	// Status bar
	StatusBarIdle  lipgloss.Style
	StatusBarBusy  lipgloss.Style
	StatusBarError lipgloss.Style

	// Composer
	Composer lipgloss.Style

	// Approval overlay
	ApprovalBorder   lipgloss.Style
	ApprovalTitle    lipgloss.Style
	ApprovalKey      lipgloss.Style
	ApprovalOption   lipgloss.Style
	ApprovalSelected lipgloss.Style

	// Spinner for in-progress activity (thinking, streaming, tool runs)
	SpinnerStyle lipgloss.Style

	// Field labels inside dialogs (dimmed, right-padded)
	DialogLabel lipgloss.Style

	// Neutral dialogs (help, session picker, command completion)
	DialogBorder lipgloss.Style
	DialogTitle  lipgloss.Style

	// NoColor marks the theme as color-free (NO_COLOR / TERM=dumb); the
	// markdown renderer uses it to select glamour's ANSI-free notty style.
	NoColor bool

	// MarkdownProfile selects the glamour style for assistant markdown:
	// "dark" (Everforest), "light" (glamour light), or "notty" (no ANSI).
	MarkdownProfile string

	// User message label ("You:")
	UserLabel lipgloss.Style

	// Colors
	Primary   lipgloss.Color
	Secondary lipgloss.Color
	Success   lipgloss.Color
	Warning   lipgloss.Color
	Error     lipgloss.Color
	Muted     lipgloss.Color
	Highlight lipgloss.Color

	// Tool status
	ToolRunning  lipgloss.Style
	ToolSuccess  lipgloss.Style
	ToolError    lipgloss.Style
	ToolCanceled lipgloss.Style

	// Dim
	Dim lipgloss.Style
}

// palette carries the per-variant accent colors; every style is derived
// from these so dark and light share one construction path.
type palette struct {
	primary   lipgloss.Color
	secondary lipgloss.Color
	success   lipgloss.Color
	warning   lipgloss.Color
	error     lipgloss.Color
	muted     lipgloss.Color
	highlight lipgloss.Color
	onAccent  lipgloss.Color // text drawn on accent-filled backgrounds
}

// everforestDarkHard is the default palette (Everforest Dark Hard):
//
//	bg0 #1e2326  fg #d3c6aa  grey1 #859289
//	red #e67e80  orange #e69875  yellow #dbbc7f
//	green #a7c080  blue #7fbbb3
var everforestDarkHard = palette{
	primary:   lipgloss.Color("#7fbbb3"),
	secondary: lipgloss.Color("#a7c080"),
	success:   lipgloss.Color("#a7c080"),
	warning:   lipgloss.Color("#dbbc7f"),
	error:     lipgloss.Color("#e67e80"),
	muted:     lipgloss.Color("#859289"),
	highlight: lipgloss.Color("#e69875"),
	onAccent:  lipgloss.Color("#1e2326"),
}

// everforestLightHard is the light counterpart (Everforest Light Hard):
//
//	bg0 #fffbef  fg #5c6a72  grey1 #a6b0a0
//	red #f85552  orange #f57d26  yellow #dfa000
//	green #8da101  blue #3a94c5
var everforestLightHard = palette{
	primary:   lipgloss.Color("#3a94c5"),
	secondary: lipgloss.Color("#8da101"),
	success:   lipgloss.Color("#8da101"),
	warning:   lipgloss.Color("#dfa000"),
	error:     lipgloss.Color("#f85552"),
	muted:     lipgloss.Color("#a6b0a0"),
	highlight: lipgloss.Color("#f57d26"),
	onAccent:  lipgloss.Color("#fffbef"),
}

// DefaultTheme returns the default dark theme: Everforest Dark Hard, so the
// TUI blends into terminals using the same palette.
func DefaultTheme() *Theme {
	return themeFromPalette(everforestDarkHard, "dark")
}

// LightTheme returns the Everforest Light Hard theme for light-background
// terminals.
func LightTheme() *Theme {
	return themeFromPalette(everforestLightHard, "light")
}

// DetectTheme picks dark or light based on the terminal's actual background
// color (queried via OSC 11; detection failure falls back to dark).
func DetectTheme() *Theme {
	if lipgloss.HasDarkBackground() {
		return DefaultTheme()
	}
	return LightTheme()
}

func themeFromPalette(p palette, markdownProfile string) *Theme {
	t := &Theme{
		Primary:         p.primary,
		Secondary:       p.secondary,
		Success:         p.success,
		Warning:         p.warning,
		Error:           p.error,
		Muted:           p.muted,
		Highlight:       p.highlight,
		MarkdownProfile: markdownProfile,
	}

	t.HeaderStyle = lipgloss.NewStyle().
		Bold(true).
		Foreground(p.onAccent). // text on the accent-filled statusline
		Background(t.Primary).
		Padding(0, 1).
		Width(80) // will be adjusted dynamically

	t.UserBlock = lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder(), false, false, false, true).
		BorderForeground(t.Primary).
		PaddingLeft(1)

	t.AssistantBlock = lipgloss.NewStyle().
		PaddingLeft(1)

	t.ToolBlock = lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder(), false, false, false, true).
		BorderForeground(t.Muted).
		PaddingLeft(1)

	t.NoticeBlock = lipgloss.NewStyle().
		Foreground(t.Muted).
		Italic(true).
		PaddingLeft(1)

	t.StreamBlock = lipgloss.NewStyle().
		PaddingLeft(1)

	t.InterruptedBlock = lipgloss.NewStyle().
		Foreground(t.Warning).
		PaddingLeft(1)

	t.StatusBarIdle = lipgloss.NewStyle().
		Foreground(t.Muted)

	t.StatusBarBusy = lipgloss.NewStyle().
		Foreground(t.Secondary)

	t.StatusBarError = lipgloss.NewStyle().
		Foreground(t.Error)

	t.Composer = lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(t.Muted).
		Padding(0, 1)

	t.ApprovalBorder = lipgloss.NewStyle().
		Border(lipgloss.ThickBorder()).
		BorderForeground(t.Warning).
		Padding(1, 2)

	t.ApprovalTitle = lipgloss.NewStyle().
		Bold(true).
		Foreground(t.Warning)

	t.ApprovalKey = lipgloss.NewStyle().
		Bold(true).
		Foreground(t.Primary)

	t.ApprovalOption = lipgloss.NewStyle().
		Foreground(t.Muted)

	t.ApprovalSelected = lipgloss.NewStyle().
		Bold(true).
		Foreground(p.onAccent).
		Background(t.Primary).
		Padding(0, 1)

	t.SpinnerStyle = lipgloss.NewStyle().
		Foreground(t.Secondary)

	t.DialogLabel = lipgloss.NewStyle().
		Foreground(t.Muted)

	t.DialogBorder = lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(t.Muted).
		Padding(0, 1)

	t.DialogTitle = lipgloss.NewStyle().
		Bold(true).
		Foreground(t.Primary)

	t.UserLabel = lipgloss.NewStyle().
		Bold(true).
		Foreground(t.Primary)

	t.ToolRunning = lipgloss.NewStyle().
		Foreground(t.Secondary).
		Bold(true)

	t.ToolSuccess = lipgloss.NewStyle().
		Foreground(t.Success)

	t.ToolError = lipgloss.NewStyle().
		Foreground(t.Error)

	t.ToolCanceled = lipgloss.NewStyle().
		Foreground(t.Muted)

	t.Dim = lipgloss.NewStyle().
		Foreground(t.Muted)

	return t
}

// NoColorTheme returns a minimal theme without colors (for NO_COLOR / TERM=dumb).
func NoColorTheme() *Theme {
	t := &Theme{NoColor: true, MarkdownProfile: "notty"}
	noStyle := lipgloss.NewStyle()
	t.HeaderStyle = noStyle.Bold(true)
	t.UserBlock = noStyle
	t.AssistantBlock = noStyle
	t.ToolBlock = noStyle
	t.NoticeBlock = noStyle.Italic(true)
	t.StreamBlock = noStyle
	t.InterruptedBlock = noStyle.Italic(true)
	t.StatusBarIdle = noStyle
	t.StatusBarBusy = noStyle.Bold(true)
	t.StatusBarError = noStyle.Bold(true)
	t.Composer = noStyle
	t.ApprovalBorder = noStyle
	t.ApprovalTitle = noStyle.Bold(true)
	t.ApprovalKey = noStyle.Bold(true)
	t.ApprovalOption = noStyle
	t.ApprovalSelected = noStyle.Bold(true).Reverse(true)
	t.SpinnerStyle = noStyle
	t.DialogLabel = noStyle
	t.DialogBorder = noStyle
	t.DialogTitle = noStyle.Bold(true)
	t.UserLabel = noStyle.Bold(true)
	t.ToolRunning = noStyle.Bold(true)
	t.ToolSuccess = noStyle
	t.ToolError = noStyle.Bold(true)
	t.ToolCanceled = noStyle
	t.Dim = noStyle
	return t
}
