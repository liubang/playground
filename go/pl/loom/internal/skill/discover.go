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

package skill

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
)

// Loader discovers skills under the workspace and user skill roots. Load is
// re-run on every prompt Build so the catalog is always fresh; it is cheap
// (a few readdir calls plus one small read per skill).
type Loader struct {
	workspaceRoot string
	homeDir       string
	extraRoots    []string
	logger        *slog.Logger
}

// NewLoader builds a Loader. homeDir is injected (instead of reading
// os.UserHomeDir internally) so tests and e2e can redirect HOME. A nil
// logger discards diagnostics.
func NewLoader(workspaceRoot, homeDir string, extraRoots []string, logger *slog.Logger) *Loader {
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	return &Loader{
		workspaceRoot: workspaceRoot,
		homeDir:       homeDir,
		extraRoots:    append([]string(nil), extraRoots...),
		logger:        logger,
	}
}

type skillRoot struct {
	path  string
	scope Scope
}

// roots enumerates the discovery roots in precedence order: repo roots first
// (so repo skills win name conflicts), then user roots. Missing directories
// are skipped silently; roots are deduped by canonical path.
func (l *Loader) roots() []skillRoot {
	var roots []skillRoot
	add := func(path string, scope Scope) {
		if strings.TrimSpace(path) != "" {
			roots = append(roots, skillRoot{path: path, scope: scope})
		}
	}
	add(filepath.Join(l.workspaceRoot, ".loom", "skills"), ScopeRepo)
	add(filepath.Join(l.workspaceRoot, ".agents", "skills"), ScopeRepo)
	if l.homeDir != "" {
		add(filepath.Join(l.homeDir, ".loom", "skills"), ScopeUser)
		add(filepath.Join(l.homeDir, ".agents", "skills"), ScopeUser)
	}
	for _, extra := range l.extraRoots {
		add(extra, ScopeUser)
	}

	seen := map[string]struct{}{}
	out := roots[:0]
	for _, r := range roots {
		canonical, err := filepath.EvalSymlinks(r.path)
		if err != nil {
			canonical = r.path
		}
		if abs, err := filepath.Abs(canonical); err == nil {
			canonical = abs
		}
		if _, dup := seen[canonical]; dup {
			continue
		}
		seen[canonical] = struct{}{}
		out = append(out, r)
	}
	return out
}

// Load discovers all skills and returns an immutable Catalog. Individual
// skill failures are recorded as LoadIssues and never block the rest.
func (l *Loader) Load(ctx context.Context) *Catalog {
	b := &catalogBuilder{byName: map[string]*Skill{}}
	for _, root := range l.roots() {
		if ctx.Err() != nil {
			break
		}
		l.scanRoot(ctx, root, b)
	}
	if len(b.issues) > 0 {
		l.logger.Warn("some skills failed to load", "count", len(b.issues), "issues", b.issues)
	}
	return newCatalog(b.skills, b.issues)
}

type catalogBuilder struct {
	skills []*Skill
	byName map[string]*Skill
	issues []LoadIssue
}

func (b *catalogBuilder) add(skill *Skill) {
	if len(b.skills) >= MaxSkillsTotal {
		b.issues = append(b.issues, issuef(skill.Path, "global skill limit %d reached", MaxSkillsTotal))
		return
	}
	if existing, dup := b.byName[skill.Name]; dup {
		b.issues = append(b.issues, issuef(skill.Path,
			"skill name %q already provided by %s", skill.Name, existing.Path))
		return
	}
	b.byName[skill.Name] = skill
	b.skills = append(b.skills, skill)
}

func (b *catalogBuilder) fail(path, message string) {
	b.issues = append(b.issues, LoadIssue{Path: path, Message: message})
}

// scanRoot recursively scans one root for SKILL.md files. Hidden directories
// are skipped; directory symlinks are FOLLOWED (like codex; the depth cap
// bounds cycles).
func (l *Loader) scanRoot(ctx context.Context, root skillRoot, b *catalogBuilder) {
	info, err := os.Stat(root.path)
	if err != nil || !info.IsDir() {
		return
	}
	loaded := 0
	var walk func(dir string, depth int)
	walk = func(dir string, depth int) {
		if depth > MaxScanDepth || loaded >= MaxSkillsPerRoot || ctx.Err() != nil {
			return
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			return
		}
		for _, entry := range entries {
			if ctx.Err() != nil || loaded >= MaxSkillsPerRoot {
				return
			}
			name := entry.Name()
			full := filepath.Join(dir, name)
			// os.Stat follows symlinks, so symlinked directories are
			// traversed and a symlinked SKILL.md resolves to its target.
			info, err := os.Stat(full)
			if err != nil {
				continue
			}
			if info.IsDir() {
				if strings.HasPrefix(name, ".") {
					continue
				}
				walk(full, depth+1)
				continue
			}
			if name != FileName || !info.Mode().IsRegular() {
				continue
			}
			l.loadOne(full, root.scope, b)
			loaded++
		}
	}
	walk(root.path, 0)
}

func (l *Loader) loadOne(path string, scope Scope, b *catalogBuilder) {
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		b.fail(path, "resolve symlinks: "+err.Error())
		return
	}
	abs, err := filepath.Abs(resolved)
	if err != nil {
		b.fail(path, "abs path: "+err.Error())
		return
	}
	info, err := os.Stat(abs)
	if err != nil {
		b.fail(path, "stat: "+err.Error())
		return
	}
	if info.Size() > MaxSkillFileBytes {
		b.fail(path, "file exceeds 256KB limit")
		return
	}
	data, err := os.ReadFile(abs)
	if err != nil {
		b.fail(path, "read: "+err.Error())
		return
	}
	skill, err := parseSkill(filepath.Dir(abs), abs, data, scope)
	if err != nil {
		b.fail(path, err.Error())
		return
	}
	b.add(skill)
}
