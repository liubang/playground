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
// Created: 2026/07/26

// Package skill discovers, parses, and renders SKILL.md skills for the loom
// agent. See go/pl/loom/docs/SKILL_DESIGN.md for the full design.
package skill

import (
	"fmt"
	"sort"
	"sync/atomic"
)

// Discovery and rendering limits. Name/description semantics follow codex
// (core-skills/src/loader.rs): name is validated at load time, description is
// only required to be non-empty at load time and truncated at render time.
const (
	// FileName is the exact skill file name (case-sensitive).
	FileName = "SKILL.md"
	// MaxNameLen bounds the skill name at load time.
	MaxNameLen = 64
	// MaxScanDepth bounds recursion below each skills root.
	MaxScanDepth = 4
	// MaxSkillsPerRoot bounds skills discovered under one root.
	MaxSkillsPerRoot = 500
	// MaxSkillsTotal bounds skills across all roots.
	MaxSkillsTotal = 1000
	// MaxSkillFileBytes bounds a single SKILL.md (read whole, like codex).
	MaxSkillFileBytes = 256 << 10
)

// Scope identifies where a skill was discovered. Repo outranks User for
// ordering and name-conflict resolution.
type Scope int

const (
	ScopeRepo Scope = iota
	ScopeUser
)

// String returns the human-readable scope name.
func (s Scope) String() string {
	if s == ScopeRepo {
		return "repo"
	}
	return "user"
}

// Skill is one discovered SKILL.md with its parsed frontmatter metadata.
type Skill struct {
	Name        string
	Description string
	// Path is the absolute, symlink-resolved path to SKILL.md.
	Path string
	// Dir is the absolute, symlink-resolved skill directory.
	Dir   string
	Scope Scope
}

// LoadIssue records a skill that failed to load without blocking the rest.
type LoadIssue struct {
	Path    string
	Message string
}

// Catalog is an immutable snapshot of discovered skills. It is shared
// read-only between the prompt provider and the read_skill tool.
type Catalog struct {
	skills []*Skill
	byName map[string]*Skill
	issues []LoadIssue
}

func newCatalog(skills []*Skill, issues []LoadIssue) *Catalog {
	sorted := append([]*Skill(nil), skills...)
	sort.Slice(sorted, func(i, j int) bool {
		if sorted[i].Scope != sorted[j].Scope {
			return sorted[i].Scope < sorted[j].Scope
		}
		return sorted[i].Name < sorted[j].Name
	})
	byName := make(map[string]*Skill, len(sorted))
	for _, s := range sorted {
		byName[s.Name] = s
	}
	return &Catalog{skills: sorted, byName: byName, issues: issues}
}

// Skills returns all skills ordered by (Scope, Name).
func (c *Catalog) Skills() []*Skill { return c.skills }

// Issues returns the load issues collected during discovery.
func (c *Catalog) Issues() []LoadIssue { return c.issues }

// Find returns the skill with the given exact name, or nil.
func (c *Catalog) Find(name string) *Skill { return c.byName[name] }

// Names returns the sorted skill names (for error messages).
func (c *Catalog) Names() []string {
	names := make([]string, 0, len(c.skills))
	for _, s := range c.skills {
		names = append(names, s.Name)
	}
	return names
}

// AtomicCatalog holds the latest Catalog snapshot. The prompt provider stores
// a fresh snapshot on every Build; read_skill resolves against the same
// snapshot, so the list the model saw and the tool's resolution stay
// consistent within a turn. The zero value holds an empty Catalog.
type AtomicCatalog struct {
	value atomic.Value // *Catalog
}

// Store replaces the current snapshot. A nil catalog is treated as empty.
func (a *AtomicCatalog) Store(c *Catalog) {
	if c == nil {
		c = &Catalog{byName: map[string]*Skill{}}
	}
	a.value.Store(c)
}

// Get returns the current snapshot (never nil).
func (a *AtomicCatalog) Get() *Catalog {
	if c, ok := a.value.Load().(*Catalog); ok && c != nil {
		return c
	}
	return &Catalog{byName: map[string]*Skill{}}
}

func issuef(path, format string, args ...any) LoadIssue {
	return LoadIssue{Path: path, Message: fmt.Sprintf(format, args...)}
}
