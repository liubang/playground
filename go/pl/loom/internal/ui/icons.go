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
// Created: 2026/07/24

package ui

import "strings"

// Icons holds the glyph set used across the TUI. Two sets are provided:
// Nerd Font glyphs (the default, requiring a Nerd Font patched terminal
// font) and plain Unicode text symbols that render in any terminal.
type Icons struct {
	Success   string // tool call succeeded, affirmative actions
	Error     string // tool call failed, deny actions
	Cancelled string // tool call cancelled
	Pending   string // waiting / unknown state
	Approval  string // approval-required marker
	Warning   string // warning title marker
}

// NerdIcons returns the Nerd Font glyph set (Font Awesome codepoints, the
// most widely patched range).
func NerdIcons() Icons {
	return Icons{
		Success:   "\uf00c", //  check
		Error:     "\uf00d", //  times
		Cancelled: "\uf05e", //  ban
		Pending:   "\uf10c", //  circle-o
		Approval:  "\uf059", //  question-circle
		Warning:   "\uf071", //  exclamation-triangle
	}
}

// PlainIcons returns the font-safe text symbol set. It deliberately avoids
// U+26A0 (⚠): many terminals render it with emoji presentation, which looks
// unprofessional and breaks width calculations.
func PlainIcons() Icons {
	return Icons{
		Success:   "✓", // ✓
		Error:     "✗", // ✗
		Cancelled: "⊘", // ⊘
		Pending:   "○", // ○
		Approval:  "?", // ?
		Warning:   "!", // !
	}
}

// ResolveIcons maps the LOOM_ICONS preference to a glyph set: "plain"
// selects the font-safe set; anything else (including empty) selects the
// Nerd Font set.
func ResolveIcons(preference string) Icons {
	if strings.EqualFold(strings.TrimSpace(preference), "plain") {
		return PlainIcons()
	}
	return NerdIcons()
}
