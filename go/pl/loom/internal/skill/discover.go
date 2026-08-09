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
	"sync/atomic"
)

// Loader discovers skills under the workspace and user skill roots. Load is
// re-run on every prompt Build so the catalog is always fresh; it is cheap
// (a few readdir calls plus one small read per skill).
type Loader struct {
	workspaceRoot string
	userRoots     []string
	logger        *slog.Logger
	// disabled holds the skill names suppressed at load time (config
	// skills.disabled). It is swappable at runtime so a config hot-apply
	// takes effect on the next Load without reassembling the loader.
	disabled atomic.Pointer[map[string]bool]
}

// NewLoader builds a Loader. Discovery roots are fully injected (the caller
// derives them from the workspace and the configured storage base_dir), so
// the loader itself performs no path conventions of its own. A nil logger
// discards diagnostics.
func NewLoader(workspaceRoot string, userRoots []string, logger *slog.Logger) *Loader {
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	return &Loader{
		workspaceRoot: workspaceRoot,
		userRoots:     append([]string(nil), userRoots...),
		logger:        logger,
	}
}

// SetDisabled replaces the set of skill names suppressed at load time. It
// is safe for concurrent use with Load; an empty list clears the set.
func (l *Loader) SetDisabled(names []string) {
	if len(names) == 0 {
		l.disabled.Store(nil)
		return
	}
	m := make(map[string]bool, len(names))
	for _, name := range names {
		m[name] = true
	}
	l.disabled.Store(&m)
}

// isDisabled reports whether name is currently suppressed.
func (l *Loader) isDisabled(name string) bool {
	if m := l.disabled.Load(); m != nil {
		return (*m)[name]
	}
	return false
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
	// An empty workspace root yields exactly the user-scope roots — joining
	// would produce cwd-relative ".loom/skills" paths, scanning whatever
	// directory the process happens to run in.
	if strings.TrimSpace(l.workspaceRoot) != "" {
		add(filepath.Join(l.workspaceRoot, ".loom", "skills"), ScopeRepo)
		add(filepath.Join(l.workspaceRoot, ".agents", "skills"), ScopeRepo)
	}
	for _, root := range l.userRoots {
		add(root, ScopeUser)
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
	// Disabled skills are skipped before conflict resolution, so a
	// disabled name never records shadowing issues either.
	if l.isDisabled(skill.Name) {
		return
	}
	b.add(skill)
}

// Delete removes the on-disk skill whose symlink-resolved SKILL.md path is
// wantPath, provided the skill lives under one of the loader's discovery
// roots. It returns false when no discovered skill matches — callers must
// resolve symlinks on wantPath first (mirroring loadOne), so the match is
// by real location while the removal targets the directory entry as it
// appears under the root: deleting a symlinked skill removes only the link,
// never the target directory. A SKILL.md sitting directly in a root removes
// just the file; anything nested removes the whole skill directory.
func (l *Loader) Delete(wantPath string) (bool, error) {
	for _, root := range l.roots() {
		info, err := os.Stat(root.path)
		if err != nil || !info.IsDir() {
			continue
		}
		dir := findSkillDir(root.path, wantPath, 0)
		if dir == "" {
			continue
		}
		if dir == root.path {
			if err := os.Remove(filepath.Join(dir, FileName)); err != nil {
				return false, err
			}
			return true, nil
		}
		// os.RemoveAll on a symlink removes the link itself, so a skill
		// linked into the root (ln -s ~/dotfiles/x …) is unlinked, and a
		// real directory is removed with all its supporting files.
		if err := os.RemoveAll(dir); err != nil {
			return false, err
		}
		return true, nil
	}
	return false, nil
}

// findSkillDir mirrors scanRoot's walk (hidden dirs skipped, symlinks
// followed, same depth cap) and returns the directory containing the
// SKILL.md that resolves to wantPath, or "" when absent. The returned
// path is the directory entry as walked — a symlinked skill yields the
// symlink path, not its target.
func findSkillDir(dir, wantPath string, depth int) string {
	if depth > MaxScanDepth {
		return ""
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	for _, entry := range entries {
		name := entry.Name()
		full := filepath.Join(dir, name)
		info, err := os.Stat(full)
		if err != nil {
			continue
		}
		if info.IsDir() {
			if strings.HasPrefix(name, ".") {
				continue
			}
			if found := findSkillDir(full, wantPath, depth+1); found != "" {
				return found
			}
			continue
		}
		if name != FileName || !info.Mode().IsRegular() {
			continue
		}
		resolved, err := filepath.EvalSymlinks(full)
		if err != nil {
			continue
		}
		if abs, err := filepath.Abs(resolved); err == nil {
			resolved = abs
		}
		if resolved == wantPath {
			return dir
		}
	}
	return ""
}
