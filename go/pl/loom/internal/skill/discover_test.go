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
	"os"
	"path/filepath"
	"testing"
)

func writeSkill(t *testing.T, dir, name, description string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("MkdirAll(%q) error = %v", dir, err)
	}
	body := "---\nname: " + name + "\ndescription: " + description + "\n---\n"
	path := filepath.Join(dir, FileName)
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatalf("WriteFile(%q) error = %v", path, err)
	}
	return path
}

func canonical(t *testing.T, path string) string {
	t.Helper()
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		t.Fatalf("EvalSymlinks(%q) error = %v", path, err)
	}
	return resolved
}

func newEnv(t *testing.T) (workspace, home string) {
	t.Helper()
	return t.TempDir(), t.TempDir()
}

// defaultUserRoots mirrors the production wiring (app.WireSkills): the
// loom skills dir plus the cross-tool ~/.agents/skills convention.
func defaultUserRoots(home string) []string {
	return []string{
		filepath.Join(home, ".loom", "skills"),
		filepath.Join(home, ".agents", "skills"),
	}
}

func loadAll(t *testing.T, loader *Loader) *Catalog {
	t.Helper()
	return loader.Load(context.Background())
}

func TestLoaderDiscoversAllRoots(t *testing.T) {
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "a"), "ws-loom", "d")
	writeSkill(t, filepath.Join(ws, ".agents", "skills", "b"), "ws-agents", "d")
	writeSkill(t, filepath.Join(home, ".loom", "skills", "c"), "home-loom", "d")
	writeSkill(t, filepath.Join(home, ".agents", "skills", "d"), "home-agents", "d")
	extra := t.TempDir()
	writeSkill(t, filepath.Join(extra, "e"), "extra-one", "d")

	cat := loadAll(t, NewLoader(ws, append(defaultUserRoots(home), extra), nil))
	if got := cat.Names(); len(got) != 5 {
		t.Fatalf("Names() = %v, want 5 skills", got)
	}
	for _, name := range []string{"ws-loom", "ws-agents", "home-loom", "home-agents", "extra-one"} {
		if cat.Find(name) == nil {
			t.Fatalf("Find(%q) = nil", name)
		}
	}
	if s := cat.Find("ws-loom"); s.Scope != ScopeRepo {
		t.Fatalf("ws-loom scope = %v, want repo", s.Scope)
	}
	if s := cat.Find("home-loom"); s.Scope != ScopeUser {
		t.Fatalf("home-loom scope = %v, want user", s.Scope)
	}
	if len(cat.Issues()) != 0 {
		t.Fatalf("Issues() = %v, want none", cat.Issues())
	}
}

func TestLoaderScopesOrderAndConflictResolution(t *testing.T) {
	ws, home := newEnv(t)
	// Name conflict across scopes: repo must win over user.
	writeSkill(t, filepath.Join(home, ".loom", "skills", "weather"), "weather", "user copy")
	writeSkill(t, filepath.Join(ws, ".agents", "skills", "weather"), "weather", "repo copy")
	// Name conflict within one scope: first root (.loom) wins over .agents.
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "dup"), "dup", "loom first")
	writeSkill(t, filepath.Join(ws, ".agents", "skills", "dup"), "dup", "agents second")
	// Ordering: repo skills sort before user skills, then by name.
	writeSkill(t, filepath.Join(home, ".agents", "skills", "beta"), "beta", "d")
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "zeta"), "zeta", "d")

	cat := loadAll(t, NewLoader(ws, defaultUserRoots(home), nil))
	if got := cat.Find("weather"); got == nil || got.Description != "repo copy" {
		t.Fatalf("weather = %+v, want repo copy", got)
	}
	if got := cat.Find("dup"); got == nil || got.Description != "loom first" {
		t.Fatalf("dup = %+v, want loom first", got)
	}
	if len(cat.Issues()) != 2 {
		t.Fatalf("Issues() = %v, want 2 conflict issues", cat.Issues())
	}
	var order []string
	for _, s := range cat.Skills() {
		order = append(order, s.Name)
	}
	// repo scope first (dup, weather, zeta), then user scope (beta); the
	// user-scope "weather" lost the name conflict and is excluded.
	want := []string{"dup", "weather", "zeta", "beta"}
	if len(order) != len(want) {
		t.Fatalf("order = %v, want %v", order, want)
	}
	for i := range want {
		if order[i] != want[i] {
			t.Fatalf("order = %v, want %v", order, want)
		}
	}
}

func TestLoaderSkipsHiddenAndLowercase(t *testing.T) {
	ws := t.TempDir()
	writeSkill(t, filepath.Join(ws, ".loom", "skills", ".hidden", "x"), "hidden-skill", "d")
	lower := filepath.Join(ws, ".loom", "skills", "lower")
	if err := os.MkdirAll(lower, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(lower, "skill.md"), []byte("---\nname: lower\ndescription: d\n---\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	cat := loadAll(t, NewLoader(ws, nil, nil))
	if len(cat.Skills()) != 0 {
		t.Fatalf("Skills() = %v, want empty (hidden dir + lowercase filename skipped)", cat.Names())
	}
}

func TestLoaderFollowsSymlinks(t *testing.T) {
	ws := t.TempDir()
	realDir := filepath.Join(t.TempDir(), "real-weather")
	realPath := writeSkill(t, realDir, "weather", "d")
	root := filepath.Join(ws, ".loom", "skills")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	// Directory symlink into the root (dotfiles-style distribution).
	if err := os.Symlink(filepath.Dir(realDir), filepath.Join(root, "linked")); err != nil {
		t.Fatal(err)
	}
	// SKILL.md that is itself a symlink.
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "host"), "host", "d")
	target := filepath.Join(ws, ".loom", "skills", "host", FileName)
	linkDir := filepath.Join(ws, ".loom", "skills", "alias")
	if err := os.MkdirAll(linkDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, filepath.Join(linkDir, FileName)); err != nil {
		t.Fatal(err)
	}

	cat := loadAll(t, NewLoader(ws, nil, nil))
	weather := cat.Find("weather")
	if weather == nil {
		t.Fatal("symlinked skill directory not discovered")
	}
	if weather.Path != canonical(t, realPath) {
		t.Fatalf("weather.Path = %q, want canonical %q", weather.Path, canonical(t, realPath))
	}
	// The aliased SKILL.md resolves to the host skill directory.
	if got := cat.Find("host"); got == nil {
		t.Fatal("symlinked SKILL.md not discovered")
	}
}

func TestLoaderDepthLimit(t *testing.T) {
	ws := t.TempDir()
	deep := filepath.Join(ws, ".loom", "skills", "a", "b", "c", "d")
	writeSkill(t, deep, "at-limit", "d")
	writeSkill(t, filepath.Join(deep, "e"), "too-deep", "d")

	cat := loadAll(t, NewLoader(ws, nil, nil))
	if cat.Find("at-limit") == nil {
		t.Fatal("skill at depth limit not discovered")
	}
	if cat.Find("too-deep") != nil {
		t.Fatal("skill beyond depth limit discovered, want skipped")
	}
}

func TestLoaderIssuesDoNotBlock(t *testing.T) {
	ws := t.TempDir()
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "good"), "good", "d")
	bad := filepath.Join(ws, ".loom", "skills", "bad")
	if err := os.MkdirAll(bad, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bad, FileName), []byte("---\nname: bad\n---\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	cat := loadAll(t, NewLoader(ws, nil, nil))
	if cat.Find("good") == nil {
		t.Fatal("valid skill blocked by a broken sibling")
	}
	if len(cat.Issues()) != 1 {
		t.Fatalf("Issues() = %v, want 1", cat.Issues())
	}
}

func TestLoaderEmptyWorkspaceSkipsRepoRoots(t *testing.T) {
	home := t.TempDir()
	writeSkill(t, filepath.Join(home, ".agents", "skills", "u"), "u", "d")
	// A repo-convention directory under the process cwd must NOT be
	// scanned when the loader has no workspace root (joining "" with
	// .loom/skills would otherwise produce a cwd-relative root).
	cwd := t.TempDir()
	writeSkill(t, filepath.Join(cwd, ".loom", "skills", "stray"), "stray", "d")
	t.Chdir(cwd)

	cat := loadAll(t, NewLoader("", defaultUserRoots(home), nil))
	if cat.Find("u") == nil {
		t.Fatal("user skill missing")
	}
	if cat.Find("stray") != nil {
		t.Fatal("cwd-relative repo root scanned with empty workspace root")
	}
}

func TestLoaderDisabledFilter(t *testing.T) {
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "weather"), "weather", "repo copy")
	writeSkill(t, filepath.Join(home, ".agents", "skills", "weather"), "weather", "user copy")
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "keep"), "keep", "d")

	loader := NewLoader(ws, defaultUserRoots(home), nil)
	loader.SetDisabled([]string{"weather"})
	cat := loadAll(t, loader)
	// Disable is by name across scopes: neither copy is visible, and the
	// shadowing conflict between them is not reported either.
	if cat.Find("weather") != nil {
		t.Fatal("disabled skill present in catalog")
	}
	if cat.Find("keep") == nil {
		t.Fatal("unrelated skill filtered out")
	}
	if len(cat.Issues()) != 0 {
		t.Fatalf("Issues() = %v, want none", cat.Issues())
	}

	// Clearing the set restores discovery (repo wins the conflict).
	loader.SetDisabled(nil)
	cat = loadAll(t, loader)
	if got := cat.Find("weather"); got == nil || got.Description != "repo copy" {
		t.Fatalf("weather = %+v, want repo copy after re-enable", got)
	}
}

func TestLoaderDeleteRemovesDirectory(t *testing.T) {
	ws := t.TempDir()
	dir := filepath.Join(ws, ".loom", "skills", "weather")
	path := writeSkill(t, dir, "weather", "d")
	// Supporting files go with the directory.
	if err := os.WriteFile(filepath.Join(dir, "run.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	loader := NewLoader(ws, nil, nil)
	ok, err := loader.Delete(canonical(t, path))
	if err != nil || !ok {
		t.Fatalf("Delete = %v, %v; want true, nil", ok, err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("skill dir still exists: %v", err)
	}
	if got := loadAll(t, loader).Names(); len(got) != 0 {
		t.Fatalf("Names() = %v after delete, want empty", got)
	}
}

func TestLoaderDeleteSymlinkUnlinksOnly(t *testing.T) {
	ws := t.TempDir()
	realDir := filepath.Join(t.TempDir(), "real-weather")
	path := writeSkill(t, realDir, "weather", "d")
	root := filepath.Join(ws, ".loom", "skills")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "linked")
	if err := os.Symlink(realDir, link); err != nil {
		t.Fatal(err)
	}

	loader := NewLoader(ws, nil, nil)
	ok, err := loader.Delete(canonical(t, path))
	if err != nil || !ok {
		t.Fatalf("Delete = %v, %v; want true, nil", ok, err)
	}
	// The link under the root is gone; the target directory survives.
	if _, err := os.Lstat(link); !os.IsNotExist(err) {
		t.Fatalf("symlink still present: %v", err)
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("symlink target removed: %v", err)
	}
}

func TestLoaderDeleteSkillFileDirectlyInRoot(t *testing.T) {
	ws := t.TempDir()
	root := filepath.Join(ws, ".loom", "skills")
	path := writeSkill(t, root, "at-root", "d")
	sibling := writeSkill(t, filepath.Join(root, "sibling"), "sibling", "d")

	loader := NewLoader(ws, nil, nil)
	ok, err := loader.Delete(canonical(t, path))
	if err != nil || !ok {
		t.Fatalf("Delete = %v, %v; want true, nil", ok, err)
	}
	// Only the SKILL.md is removed — the root (and its other skills)
	// must survive.
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("SKILL.md still present: %v", err)
	}
	if _, err := os.Stat(sibling); err != nil {
		t.Fatalf("sibling skill removed: %v", err)
	}
	if got := loadAll(t, loader).Names(); len(got) != 1 || got[0] != "sibling" {
		t.Fatalf("Names() = %v, want [sibling]", got)
	}
}

func TestLoaderDeleteUnknownPath(t *testing.T) {
	ws := t.TempDir()
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "keep"), "keep", "d")
	// A loadable SKILL.md outside every discovery root must not match.
	outside := writeSkill(t, filepath.Join(t.TempDir(), "x"), "x", "d")

	loader := NewLoader(ws, nil, nil)
	ok, err := loader.Delete(canonical(t, outside))
	if err != nil || ok {
		t.Fatalf("Delete = %v, %v; want false, nil", ok, err)
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatalf("outside-root skill removed: %v", err)
	}
}

func TestLoaderExtraRootDeduplicated(t *testing.T) {
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(home, ".loom", "skills", "x"), "x", "d")
	// Passing the same root again (via symlink-free path) must not produce a
	// spurious conflict issue.
	extra := canonical(t, filepath.Join(home, ".loom", "skills"))
	cat := loadAll(t, NewLoader(ws, append(defaultUserRoots(home), extra), nil))
	if len(cat.Skills()) != 1 || len(cat.Issues()) != 0 {
		t.Fatalf("Skills/Issues = %d/%d, want 1/0 (duplicate root deduped)", len(cat.Skills()), len(cat.Issues()))
	}
}
