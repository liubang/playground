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
// Created: 2026/08/02

package ui

import (
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// RuleEntry is one row in the /rules picker: one capability package,
// with enough data to render, filter, and (for remembered entries)
// delete.
type RuleEntry struct {
	Kind          string // argv | exact | host | path | tool
	Label         string // display text: joined prefix, @host, #tool, ~/dir
	Decision      string
	Scope         string // builtin | project | user | session
	Source        string
	Grant         string // grant summary (e.g. "network=full, write=2 paths")
	Consequence   string // consequence ceiling (allow packages)
	Justification string
	Deletable     bool // true when Source == "remembered"
	// Bind is the deletion key (only valid when Deletable).
	Bind permission.Binding
}

// NewRulesFinder creates the /rules picker populated with the combined
// capability set. Builtin / user / project entries are read-only;
// remembered entries are deletable.
func (m Model) NewRulesFinder(packages []permission.Package) *Finder[RuleEntry] {
	return NewFinder(FinderConfig[RuleEntry]{
		Title:   "Rules",
		Items:   rulesFinderItems(packages),
		Preview: rulePreview,
		Styles:  m.finderStyles(),
	})
}

// rulesFinderItems flattens the capability set into finder rows.
func rulesFinderItems(packages []permission.Package) []FinderItem[RuleEntry] {
	items := make([]FinderItem[RuleEntry], 0, len(packages))
	for _, p := range packages {
		e := ruleEntryOf(p)
		items = append(items, FinderItem[RuleEntry]{
			Value: e,
			Text:  e.Label,
			Hint:  rulesRowHint(e),
		})
	}
	return items
}

// ruleEntryOf renders one package as a picker row.
func ruleEntryOf(p permission.Package) RuleEntry {
	e := RuleEntry{
		Decision:      string(p.Decision),
		Scope:         p.Scope.String(),
		Source:        p.Source,
		Justification: p.Justification,
		Deletable:     p.Source == permission.RememberedSource,
		Bind:          p.Bind,
	}
	switch p.Bind.Kind {
	case permission.BindArgv:
		e.Kind = "argv"
		e.Label = strings.Join(p.Bind.Argv, " ")
	case permission.BindArgvExact:
		e.Kind = "exact"
		e.Label = "exact: " + strings.Join(p.Bind.Argv, " ")
	case permission.BindHost:
		e.Kind = "host"
		e.Label = "@" + p.Bind.Host
	case permission.BindPath:
		e.Kind = "path"
		e.Label = p.Bind.Path
	case permission.BindTool:
		e.Kind = "tool"
		e.Label = "#" + p.Bind.Tool
	}
	if !p.Grant.IsZero() {
		e.Grant = summarizeGrant(p.Grant)
	}
	if p.Decision == "allow" && p.MaxConsequence != permission.ConsequenceConfined {
		e.Consequence = p.MaxConsequence.String()
	}
	return e
}

// rulesRowHint is the dimmed secondary column: the package source, plus
// the delete affordance for remembered entries.
func rulesRowHint(e RuleEntry) string {
	if e.Deletable {
		return "remembered · d=delete"
	}
	if e.Source != "" {
		return e.Source
	}
	return e.Scope
}

func rulePreview(e RuleEntry) string {
	var b strings.Builder
	b.WriteString("Kind:     " + e.Kind + "\n")
	b.WriteString("Binding:  " + e.Label + "\n")
	b.WriteString("Decision: " + e.Decision + "\n")
	b.WriteString("Scope:    " + e.Scope + "\n")
	if e.Source != "" {
		b.WriteString("Source:   " + e.Source + "\n")
	}
	if e.Grant != "" {
		b.WriteString("Grant:    " + e.Grant + "\n")
	}
	if e.Consequence != "" {
		b.WriteString("Ceiling:  " + e.Consequence + "\n")
	}
	if e.Justification != "" {
		b.WriteString("Reason:   " + e.Justification + "\n")
	}
	if e.Deletable {
		b.WriteString("\nPress d to delete this package.")
	}
	return b.String()
}

// summarizeGrant renders a PackageGrant as a compact one-liner.
func summarizeGrant(g permission.PackageGrant) string {
	var parts []string
	if g.Unsandboxed {
		parts = append(parts, "unsandboxed")
	}
	if g.NetworkFull {
		parts = append(parts, "network=full")
	}
	for _, h := range g.NetworkHosts {
		parts = append(parts, "network="+h)
	}
	if len(g.WritablePaths) > 0 {
		parts = append(parts, fmt.Sprintf("write=%d paths", len(g.WritablePaths)))
	}
	if g.GUIOpen {
		parts = append(parts, "gui_open")
	}
	return strings.Join(parts, ", ")
}

// rulesDeletePrompt returns the inline prompt shown when the user
// presses d on a deletable entry.
func rulesDeletePrompt(e RuleEntry) string {
	return fmt.Sprintf("Delete %q? [y/n]", e.Label)
}
