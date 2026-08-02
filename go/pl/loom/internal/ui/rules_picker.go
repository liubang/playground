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

// RuleEntry is one row in the /rules picker: either an argv-prefix rule
// or a domain rule, with enough data to render, filter, and (for
// remembered entries) delete.
type RuleEntry struct {
	Kind          permission.RuleKind // Argv or Domain
	Label         string              // display text: joined prefix or @host
	Decision      string
	Source        string
	Grant         string // grant summary (e.g. "network=full, write=2 paths")
	Justification string
	Deletable     bool // true when Source == "remembered"
	// Deletion keys (only valid when Deletable):
	Prefix []string // argv prefix (Kind == Argv)
	Host   string   // domain host  (Kind == Domain)
}

// NewRulesFinder creates the /rules picker populated with the combined
// rule set. Builtin / user / project entries are read-only; remembered
// entries are deletable.
func (m Model) NewRulesFinder(rules *permission.RuleSet) *Finder[RuleEntry] {
	return NewFinder(FinderConfig[RuleEntry]{
		Title:   "Rules",
		Items:   rulesFinderItems(rules),
		Preview: rulePreview,
		Styles:  m.finderStyles(),
	})
}

// rulesFinderItems flattens the combined rule set into finder rows.
// A nil rule set (rules disabled) yields no rows.
func rulesFinderItems(rules *permission.RuleSet) []FinderItem[RuleEntry] {
	items := make([]FinderItem[RuleEntry], 0, len(rules.Rules())+len(rules.Domains()))
	for _, r := range rules.Rules() {
		e := RuleEntry{
			Kind:          permission.RuleArgv,
			Label:         strings.Join(r.ArgvPrefix, " "),
			Decision:      r.Decision,
			Source:        r.Source,
			Justification: r.Justification,
			Deletable:     r.Source == permission.RememberedSource,
			Prefix:        r.ArgvPrefix,
		}
		if r.Grant != nil {
			e.Grant = summarizeGrant(r.Grant)
		}
		items = append(items, FinderItem[RuleEntry]{
			Value: e,
			Text:  e.Label,
			Hint:  rulesRowHint(e),
		})
	}
	for _, d := range rules.Domains() {
		e := RuleEntry{
			Kind:          permission.RuleDomain,
			Label:         "@" + d.Host,
			Decision:      d.Decision,
			Source:        d.Source,
			Justification: d.Justification,
			Deletable:     d.Source == permission.RememberedSource,
			Host:          d.Host,
		}
		items = append(items, FinderItem[RuleEntry]{
			Value: e,
			Text:  e.Label,
			Hint:  rulesRowHint(e),
		})
	}
	return items
}

// rulesRowHint is the dimmed secondary column: the rule source, plus the
// delete affordance for remembered entries.
func rulesRowHint(e RuleEntry) string {
	if e.Deletable {
		return "remembered · d=delete"
	}
	return e.Source
}

func rulePreview(e RuleEntry) string {
	var b strings.Builder
	if e.Kind == permission.RuleDomain {
		b.WriteString("Kind:     domain\n")
		b.WriteString("Host:     " + e.Host + "\n")
	} else {
		b.WriteString("Kind:     argv\n")
		b.WriteString("Prefix:   " + e.Label + "\n")
	}
	b.WriteString("Decision: " + e.Decision + "\n")
	b.WriteString("Source:   " + e.Source + "\n")
	if e.Grant != "" {
		b.WriteString("Grant:    " + e.Grant + "\n")
	}
	if e.Justification != "" {
		b.WriteString("Reason:   " + e.Justification + "\n")
	}
	if e.Deletable {
		b.WriteString("\nPress d to delete this rule.")
	}
	return b.String()
}

// summarizeGrant renders a RuleGrant as a compact one-liner.
func summarizeGrant(g *permission.RuleGrant) string {
	var parts []string
	if g.Network != "" {
		parts = append(parts, "network="+g.Network)
	}
	if g.Unsandboxed {
		parts = append(parts, "unsandboxed")
	}
	if len(g.Write) > 0 {
		parts = append(parts, fmt.Sprintf("write=%d paths", len(g.Write)))
	}
	return strings.Join(parts, ", ")
}

// rulesDeletePrompt returns the inline prompt shown when the user
// presses d on a deletable entry.
func rulesDeletePrompt(e RuleEntry) string {
	return fmt.Sprintf("Delete %q? [y/n]", e.Label)
}
