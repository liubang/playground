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

package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/skillread"
)

// SkillsHandle exposes the skills loader and shared catalog assembled by
// WireSkills, so frontends can list and refresh the discovered skills
// (the /skill command) against the same catalog the model sees.
type SkillsHandle struct {
	Loader  *skill.Loader
	Catalog *skill.AtomicCatalog
}

// WireSkills builds the skills loader and shared catalog, registers the
// read_skill tool, and returns the prompt option carrying the catalog
// provider plus a handle to the underlying loader/catalog. It is the
// single wiring point shared by the headless (loom run) and TUI entry
// paths so both behave identically.
//
// Skills are disabled (no tool, no prompt section; nil option and nil
// handle returned) when cfg.Enabled is false or the built-in system
// prompt is disabled — a read_skill tool without a visible catalog would
// only mislead the model.
//
// loomSkillsDir is the user-scope root under the loom home; the
// cross-tool ~/.agents/skills convention (anchored to $HOME, not the
// loom home) and cfg.ExtraRoots complete the user-scope roots.
func WireSkills(
	registry *agent.ToolRegistry,
	workspaceRoot string,
	contextWindow int64,
	cfg config.ResolvedSkills,
	loomSkillsDir string,
	systemPromptDisabled bool,
	logger *slog.Logger,
) (prompt.Option, *SkillsHandle, error) {
	if registry == nil {
		return nil, nil, fmt.Errorf("tool registry is required")
	}
	if !cfg.Enabled || systemPromptDisabled {
		return nil, nil, nil
	}
	loader := skill.NewLoader(workspaceRoot, skillUserRoots(cfg, loomSkillsDir), logger)
	loader.SetDisabled(cfg.Disabled)
	catalog := &skill.AtomicCatalog{}
	readSkill, err := skillread.NewReadSkillTool(catalog)
	if err != nil {
		return nil, nil, fmt.Errorf("read_skill: %w", err)
	}
	if err := registry.Register(readSkill); err != nil {
		return nil, nil, fmt.Errorf("register read_skill: %w", err)
	}
	opt := prompt.WithSkillsProvider(skill.NewPromptProvider(loader, catalog, contextWindow))
	return opt, &SkillsHandle{Loader: loader, Catalog: catalog}, nil
}

// skillUserRoots derives the user-scope discovery roots: the loom skills
// dir under the loom home, the cross-tool ~/.agents/skills
// convention (anchored to $HOME, not the loom home), and the configured
// extra roots. Shared by WireSkills and the aggregated skills listing so
// both see the same user scope.
func skillUserRoots(cfg config.ResolvedSkills, loomSkillsDir string) []string {
	userRoots := make([]string, 0, 2+len(cfg.ExtraRoots))
	if loomSkillsDir != "" {
		userRoots = append(userRoots, loomSkillsDir)
	}
	if home, err := os.UserHomeDir(); err == nil {
		userRoots = append(userRoots, filepath.Join(home, ".agents", "skills"))
	}
	return append(userRoots, cfg.ExtraRoots...)
}

// SkillGroup is one section of the aggregated skills listing: either the
// user-scope group shared by every workspace (Shared=true), or one
// workspace's repo-scope skills.
type SkillGroup struct {
	WorkspaceID   string      `json:"workspace_id,omitempty"`
	WorkspaceName string      `json:"workspace_name"`
	Root          string      `json:"root,omitempty"`
	Shared        bool        `json:"shared,omitempty"`
	Skills        []SkillInfo `json:"skills"`
	Issues        []string    `json:"issues,omitempty"`
}

// SkillsOverview is the GET /v1/skills response model: the shared
// user-scope group plus every registered workspace's repo-scope skills.
type SkillsOverview struct {
	Enabled bool         `json:"enabled"`
	Reason  string       `json:"reason,omitempty"`
	Groups  []SkillGroup `json:"groups"`
}

// SkillsOverview aggregates the skill catalog across every registered
// workspace for the settings UI. Discovery re-runs against disk so the
// listing reflects files added or fixed since the last prompt build; for
// already-assembled workspaces the fresh snapshot is stored back into the
// shared catalog (keeping read_skill consistent with what the user just
// saw, like Controller.ListSkills). User-scope roots are identical for
// every workspace, so they are scanned once and reported as the shared
// group. Workspaces that were never assembled are scanned with a
// throwaway repo-scope loader instead of forcing a full assembly.
func (s *SessionService) SkillsOverview(ctx context.Context) SkillsOverview {
	resolved := s.proc.Resolved()
	if resolved == nil {
		return SkillsOverview{Reason: "no configuration loaded", Groups: []SkillGroup{}}
	}
	if !resolved.Skills.Enabled {
		return SkillsOverview{Reason: "skills.enabled=false", Groups: []SkillGroup{}}
	}
	if resolved.Prompt.DisableBuiltin {
		return SkillsOverview{Reason: "prompt.disable_builtin=true", Groups: []SkillGroup{}}
	}
	ov := SkillsOverview{Enabled: true, Groups: []SkillGroup{}}

	// The management listing deliberately scans WITHOUT the disabled
	// filter: suppressed skills stay visible (marked disabled) so the UI
	// can re-enable them. Only the catalogs stored back into the
	// assembled workspaces are filtered.
	disabled := map[string]bool{}
	for _, name := range resolved.Skills.Disabled {
		disabled[name] = true
	}

	// Shared user scope: identical for every workspace, listed once. An
	// empty workspace root yields exactly the user-scope roots.
	shared := skill.NewLoader("", skillUserRoots(resolved.Skills, resolved.Storage.SkillsDir()), s.logger).Load(ctx)
	ov.Groups = append(ov.Groups, SkillGroup{
		WorkspaceName: "用户级（所有工作区共享）",
		Shared:        true,
		Skills:        skillInfos(shared.Skills(), disabled),
		Issues:        issueStrings(shared.Issues()),
	})

	workspaces, err := s.registry.List(ctx)
	if err != nil {
		s.logger.Warn("skills overview: list workspaces failed", "error", err)
		return ov
	}
	defID := s.registry.DefaultID()
	sort.Slice(workspaces, func(i, j int) bool {
		if (workspaces[i].ID == defID) != (workspaces[j].ID == defID) {
			return workspaces[i].ID == defID
		}
		return workspaces[i].Name < workspaces[j].Name
	})
	for _, ws := range workspaces {
		g := SkillGroup{
			WorkspaceID:   ws.ID.String(),
			WorkspaceName: ws.Name,
			Root:          ws.RootPath,
			Skills:        []SkillInfo{},
		}
		if b, ok := s.registry.Get(ws.ID); ok && b.Skills != nil {
			catalog := b.Skills.Loader.Load(ctx)
			b.Skills.Catalog.Store(catalog)
			// The stored catalog is what the model sees (disabled skills
			// filtered out); the listing needs them visible, so rescan
			// repo-only without the filter when any name is suppressed.
			display := catalog
			if len(disabled) > 0 {
				display = skill.NewLoader(ws.RootPath, nil, s.logger).Load(ctx)
			}
			// The workspace loader also scans the user roots; those skills
			// (and their issues) already appear in the shared group, so only
			// the repo scope is reported here.
			for _, sk := range display.Skills() {
				if sk.Scope == skill.ScopeRepo {
					g.Skills = append(g.Skills, skillInfoOf(sk, disabled))
				}
			}
			prefix := ws.RootPath + string(os.PathSeparator)
			for _, issue := range display.Issues() {
				if strings.HasPrefix(issue.Path, prefix) {
					g.Issues = append(g.Issues, issueString(issue))
				}
			}
		} else {
			catalog := skill.NewLoader(ws.RootPath, nil, s.logger).Load(ctx)
			g.Skills = skillInfos(catalog.Skills(), disabled)
			g.Issues = issueStrings(catalog.Issues())
		}
		ov.Groups = append(ov.Groups, g)
	}
	return ov
}

// skillInfoOf projects one discovered skill for frontend display.
func skillInfoOf(s *skill.Skill, disabled map[string]bool) SkillInfo {
	return SkillInfo{
		Name:        s.Name,
		Description: s.Description,
		Scope:       s.Scope.String(),
		Path:        s.Path,
		Disabled:    disabled[s.Name],
	}
}

func skillInfos(skills []*skill.Skill, disabled map[string]bool) []SkillInfo {
	out := make([]SkillInfo, 0, len(skills))
	for _, s := range skills {
		out = append(out, skillInfoOf(s, disabled))
	}
	return out
}

// ErrSkillNotFound reports a skill mutation (e.g. delete) against a path
// that no discovery root resolves to a discovered skill.
var ErrSkillNotFound = errors.New("skill not found")

// DeleteSkill removes the on-disk skill addressed by rawPath (the SKILL.md
// path as reported by SkillsOverview). The path must resolve under one of
// the discovery roots — the shared user roots or any registered
// workspace's repo roots — otherwise ErrSkillNotFound is returned, which
// also confines the delete to directories loom itself would load from.
// Assembled catalogs are refreshed so read_skill stops resolving the
// skill immediately rather than at the next prompt build.
func (s *SessionService) DeleteSkill(ctx context.Context, rawPath string) error {
	resolved := s.proc.Resolved()
	if resolved == nil {
		return fmt.Errorf("no configuration loaded")
	}
	trimmed := strings.TrimSpace(rawPath)
	abs, err := filepath.Abs(trimmed)
	if trimmed == "" || err != nil || filepath.Base(abs) != skill.FileName {
		return fmt.Errorf("invalid input: path must name a %s file", skill.FileName)
	}
	// Resolve like the loader does (loadOne) so the match is by real
	// location; a missing file cannot match anything.
	want, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return fmt.Errorf("%w: %s", ErrSkillNotFound, rawPath)
	}
	if absWant, err := filepath.Abs(want); err == nil {
		want = absWant
	}

	// Candidate loaders: one user-scope loader plus one per registered
	// workspace (the assembled loader already covers repo + user roots).
	loaders := []*skill.Loader{
		skill.NewLoader("", skillUserRoots(resolved.Skills, resolved.Storage.SkillsDir()), s.logger),
	}
	if workspaces, err := s.registry.List(ctx); err == nil {
		for _, ws := range workspaces {
			if b, ok := s.registry.Get(ws.ID); ok && b.Skills != nil {
				loaders = append(loaders, b.Skills.Loader)
			} else {
				loaders = append(loaders, skill.NewLoader(ws.RootPath, nil, s.logger))
			}
		}
	}
	for _, loader := range loaders {
		ok, err := loader.Delete(want)
		if err != nil {
			return err
		}
		if ok {
			s.refreshSkillCatalogs(ctx)
			return nil
		}
	}
	return fmt.Errorf("%w: %s", ErrSkillNotFound, rawPath)
}

// refreshSkillCatalogs reloads and re-stores every assembled workspace's
// skill catalog, keeping read_skill consistent with on-disk changes and
// config hot-applies ahead of the next prompt build.
func (s *SessionService) refreshSkillCatalogs(ctx context.Context) {
	for _, b := range s.registry.Bootstraps() {
		if b.Skills != nil {
			b.Skills.Catalog.Store(b.Skills.Loader.Load(ctx))
		}
	}
}

func issueString(issue skill.LoadIssue) string {
	return fmt.Sprintf("%s: %s", issue.Path, issue.Message)
}

func issueStrings(issues []skill.LoadIssue) []string {
	if len(issues) == 0 {
		return nil
	}
	out := make([]string, 0, len(issues))
	for _, issue := range issues {
		out = append(out, issueString(issue))
	}
	return out
}
