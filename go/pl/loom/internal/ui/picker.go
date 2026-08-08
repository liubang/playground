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

	"github.com/liubang/playground/go/pl/loom/internal/app"
)

// This file wires the three picker modes onto the generic Finder
// component (docs/VIM_UI_DESIGN.md §6.4): each picker is a Finder with a
// source-specific item mapping and preview pane. The Finder itself lives
// in finder.go; only the data shaping lives here.

// finderStyles derives the picker styling from the active theme.
func (m Model) finderStyles() FinderStyles {
	return FinderStyles{
		Cursor: m.theme.UserLabel,
		Hint:   m.theme.Dim,
		Badge:  m.theme.UserLabel,
		Input:  m.theme.DialogLabel,
		Footer: m.theme.Dim,
	}
}

// --- sessions ---

// NewSessionFinder creates the /sessions picker in its loading state;
// the items arrive via sessionsLoadedMsg → Load.
func (m Model) NewSessionFinder() *Finder[app.SessionSummary] {
	return NewLoadingFinder[app.SessionSummary]("Sessions", sessionPreview, m.finderStyles())
}

// sessionFinderItems maps session summaries onto finder rows: the short
// ID filters and displays, version and age ride along as the hint.
func sessionFinderItems(summaries []app.SessionSummary) []FinderItem[app.SessionSummary] {
	items := make([]FinderItem[app.SessionSummary], 0, len(summaries))
	for _, s := range summaries {
		items = append(items, FinderItem[app.SessionSummary]{
			Value: s,
			Text:  s.ID.String(),
			Hint:  fmt.Sprintf("v%d · %s", s.Version, formatTimeAgo(s.UpdatedAt)),
		})
	}
	return items
}

func sessionPreview(s app.SessionSummary) string {
	return fmt.Sprintf("ID:       %s\nVersion:  %d\nCreated:  %s\nUpdated:  %s",
		s.ID, s.Version,
		s.CreatedAt.Format("2006-01-02 15:04:05"),
		s.UpdatedAt.Format("2006-01-02 15:04:05"))
}

// --- models ---

// ModelOption is one selectable provider/model entry in the /model picker.
// The catalog is static for the process lifetime (the config file loads
// once at startup), so it is handed to the TUI via InitOptions.
type ModelOption struct {
	Provider      string
	Name          string
	ContextWindow int64
	WireAPI       string
}

// Ref returns the canonical "provider/model" reference accepted by /model.
func (o ModelOption) Ref() string { return o.Provider + "/" + o.Name }

// NewModelFinder creates the /model picker with the cursor on the active
// model, so the common "open and confirm" flow costs zero keystrokes.
func (m Model) NewModelFinder(options []ModelOption, currentRef string) *Finder[ModelOption] {
	items := make([]FinderItem[ModelOption], 0, len(options))
	cursorAt := 0
	for i, o := range options {
		badge := ""
		if o.Ref() == currentRef {
			badge = "●"
			cursorAt = i
		}
		items = append(items, FinderItem[ModelOption]{
			Value: o,
			Text:  o.Ref(),
			Hint:  modelOptionMeta(o),
			Badge: badge,
		})
	}
	return NewFinder(FinderConfig[ModelOption]{
		Title:    "Models",
		Items:    items,
		Preview:  modelPreview,
		CursorAt: cursorAt,
		Styles:   m.finderStyles(),
	})
}

func modelPreview(o ModelOption) string {
	lines := []string{
		"Provider: " + o.Provider,
		"Model:    " + o.Name,
	}
	if o.ContextWindow > 0 {
		lines = append(lines, fmt.Sprintf("Context:  %s tokens", formatTokens(o.ContextWindow)))
	}
	if o.WireAPI != "" {
		lines = append(lines, "Wire API: "+o.WireAPI)
	}
	return strings.Join(lines, "\n")
}

// modelOptionMeta renders the trailing metadata column: "200k ctx ·
// responses", with absent parts omitted.
func modelOptionMeta(o ModelOption) string {
	var parts []string
	if o.ContextWindow > 0 {
		parts = append(parts, formatTokens(o.ContextWindow)+" ctx")
	}
	if o.WireAPI != "" {
		parts = append(parts, o.WireAPI)
	}
	return strings.Join(parts, " · ")
}

// --- reasoning ---

// ReasoningLevel is one selectable dial in the /reasoning picker. Arg is
// the SetReasoning argument.
type ReasoningLevel struct {
	Arg   string
	Label string
	Desc  string
}

// ReasoningLevels is the fixed dial catalog in display order: the config
// fallback first, then the four override levels from least to most thinking.
var ReasoningLevels = []ReasoningLevel{
	{Arg: "default", Label: "default", Desc: "follow the model's configured reasoning"},
	{Arg: "off", Label: "off", Desc: "no thinking; fastest and cheapest"},
	{Arg: "low", Label: "low", Desc: "light thinking (≈1/8 of the output budget on Anthropic)"},
	{Arg: "medium", Label: "medium", Desc: "moderate thinking (≈1/3 of the output budget)"},
	{Arg: "high", Label: "high", Desc: "deep thinking (≈2/3 of the output budget)"},
}

// NewReasoningFinder creates the /reasoning picker with the cursor on the
// active dial: the session-override level when one is set, "default"
// otherwise.
func (m Model) NewReasoningFinder(effort string, overridden bool) *Finder[ReasoningLevel] {
	current := "default"
	if overridden && effort != "" {
		current = effort
	}
	items := make([]FinderItem[ReasoningLevel], 0, len(ReasoningLevels))
	cursorAt := 0
	for i, l := range ReasoningLevels {
		badge := ""
		if l.Arg == current {
			badge = "●"
			cursorAt = i
		}
		items = append(items, FinderItem[ReasoningLevel]{
			Value: l,
			Text:  l.Label,
			Hint:  l.Desc,
			Badge: badge,
		})
	}
	return NewFinder(FinderConfig[ReasoningLevel]{
		Title:    "Reasoning",
		Items:    items,
		Preview:  func(l ReasoningLevel) string { return l.Label + "\n" + l.Desc },
		CursorAt: cursorAt,
		Styles:   m.finderStyles(),
	})
}

// formatTokens renders a token count in compact decimal form (200k, 1.0M).
// Delegates to humanizeTokens — the single implementation (REVIEW R8).
func formatTokens(n int64) string {
	return humanizeTokens(n)
}

// formatTimeAgo returns a short relative time description.
func formatTimeAgo(t time.Time) string {
	d := time.Since(t)
	if d < time.Minute {
		return "just now"
	}
	if d < time.Hour {
		return fmt.Sprintf("%dm ago", int(d.Minutes()))
	}
	if d < 24*time.Hour {
		return fmt.Sprintf("%dh ago", int(d.Hours()))
	}
	if d < 30*24*time.Hour {
		return fmt.Sprintf("%dd ago", int(d.Hours()/24))
	}
	return t.Format("2006-01-02")
}
