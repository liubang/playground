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

	cat := loadAll(t, NewLoader(ws, home, []string{extra}, nil))
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
	writeSkill(t, filepath.Join(home, ".loom", "skills", "pandora"), "pandora", "user copy")
	writeSkill(t, filepath.Join(ws, ".agents", "skills", "pandora"), "pandora", "repo copy")
	// Name conflict within one scope: first root (.loom) wins over .agents.
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "dup"), "dup", "loom first")
	writeSkill(t, filepath.Join(ws, ".agents", "skills", "dup"), "dup", "agents second")
	// Ordering: repo skills sort before user skills, then by name.
	writeSkill(t, filepath.Join(home, ".agents", "skills", "beta"), "beta", "d")
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "zeta"), "zeta", "d")

	cat := loadAll(t, NewLoader(ws, home, nil, nil))
	if got := cat.Find("pandora"); got == nil || got.Description != "repo copy" {
		t.Fatalf("pandora = %+v, want repo copy", got)
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
	// repo scope first (dup, pandora, zeta), then user scope (beta); the
	// user-scope "pandora" lost the name conflict and is excluded.
	want := []string{"dup", "pandora", "zeta", "beta"}
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
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(ws, ".loom", "skills", ".hidden", "x"), "hidden-skill", "d")
	lower := filepath.Join(ws, ".loom", "skills", "lower")
	if err := os.MkdirAll(lower, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(lower, "skill.md"), []byte("---\nname: lower\ndescription: d\n---\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	cat := loadAll(t, NewLoader(ws, home, nil, nil))
	if len(cat.Skills()) != 0 {
		t.Fatalf("Skills() = %v, want empty (hidden dir + lowercase filename skipped)", cat.Names())
	}
}

func TestLoaderFollowsSymlinks(t *testing.T) {
	ws, home := newEnv(t)
	realDir := filepath.Join(t.TempDir(), "real-pandora")
	realPath := writeSkill(t, realDir, "pandora", "d")
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

	cat := loadAll(t, NewLoader(ws, home, nil, nil))
	pandora := cat.Find("pandora")
	if pandora == nil {
		t.Fatal("symlinked skill directory not discovered")
	}
	if pandora.Path != canonical(t, realPath) {
		t.Fatalf("pandora.Path = %q, want canonical %q", pandora.Path, canonical(t, realPath))
	}
	// The aliased SKILL.md resolves to the host skill directory.
	if got := cat.Find("host"); got == nil {
		t.Fatal("symlinked SKILL.md not discovered")
	}
}

func TestLoaderDepthLimit(t *testing.T) {
	ws, home := newEnv(t)
	deep := filepath.Join(ws, ".loom", "skills", "a", "b", "c", "d")
	writeSkill(t, deep, "at-limit", "d")
	writeSkill(t, filepath.Join(deep, "e"), "too-deep", "d")

	cat := loadAll(t, NewLoader(ws, home, nil, nil))
	if cat.Find("at-limit") == nil {
		t.Fatal("skill at depth limit not discovered")
	}
	if cat.Find("too-deep") != nil {
		t.Fatal("skill beyond depth limit discovered, want skipped")
	}
}

func TestLoaderIssuesDoNotBlock(t *testing.T) {
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "good"), "good", "d")
	bad := filepath.Join(ws, ".loom", "skills", "bad")
	if err := os.MkdirAll(bad, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bad, FileName), []byte("---\nname: bad\n---\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	cat := loadAll(t, NewLoader(ws, home, nil, nil))
	if cat.Find("good") == nil {
		t.Fatal("valid skill blocked by a broken sibling")
	}
	if len(cat.Issues()) != 1 {
		t.Fatalf("Issues() = %v, want 1", cat.Issues())
	}
}

func TestLoaderExtraRootDeduplicated(t *testing.T) {
	ws, home := newEnv(t)
	writeSkill(t, filepath.Join(home, ".loom", "skills", "x"), "x", "d")
	// Passing the same root again (via symlink-free path) must not produce a
	// spurious conflict issue.
	extra := canonical(t, filepath.Join(home, ".loom", "skills"))
	cat := loadAll(t, NewLoader(ws, home, []string{extra}, nil))
	if len(cat.Skills()) != 1 || len(cat.Issues()) != 0 {
		t.Fatalf("Skills/Issues = %d/%d, want 1/0 (duplicate root deduped)", len(cat.Skills()), len(cat.Issues()))
	}
}
