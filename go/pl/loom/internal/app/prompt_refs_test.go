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
// Created: 2026/08/22

package app

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/skill"
)

// writeFile writes a file under the temp workspace and returns rel.
func writeFile(t *testing.T, root, rel, content string) string {
	t.Helper()
	abs := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(abs), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(abs, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return rel
}

func TestResolvePromptRefsFileContent(t *testing.T) {
	root := t.TempDir()
	writeFile(t, root, "main.go", "package main\n")

	out := resolvePromptRefs(root, nil, "看下 @main.go 这个文件", nil)
	if !strings.Contains(out, "看下 @main.go 这个文件") {
		t.Fatalf("original prompt missing: %q", out)
	}
	if !strings.Contains(out, LoomContextMark) || !strings.Contains(out, `<file path="main.go"`) {
		t.Fatalf("file block missing: %q", out)
	}
	if !strings.Contains(out, "package main") {
		t.Fatalf("file content not inlined: %q", out)
	}
}

func TestResolvePromptRefsTrailingPunctuation(t *testing.T) {
	root := t.TempDir()
	writeFile(t, root, "a.txt", "hello")

	// the CJK full stop is sentence punctuation, not part of the path
	out := resolvePromptRefs(root, nil, "读 @a.txt。", nil)
	if !strings.Contains(out, `<file path="a.txt"`) {
		t.Fatalf("trailing CJK period broke resolution: %q", out)
	}
}

func TestResolvePromptRefsUnknownLeftAlone(t *testing.T) {
	root := t.TempDir()
	prompt := "@nope.txt 存在吗"
	if out := resolvePromptRefs(root, nil, prompt, nil); out != prompt {
		t.Fatalf("unknown ref should stay plain text, got %q", out)
	}
}

func TestResolvePromptRefsEscapeRejected(t *testing.T) {
	root := t.TempDir()
	outside := filepath.Join(t.TempDir(), "secret.txt")
	if err := os.WriteFile(outside, []byte("nope"), 0o644); err != nil {
		t.Fatal(err)
	}
	prompt := "读 @" + outside
	if out := resolvePromptRefs(root, nil, prompt, nil); out != prompt {
		t.Fatalf("out-of-workspace ref must not be resolved, got %q", out)
	}
}

func TestResolvePromptRefsDirectory(t *testing.T) {
	root := t.TempDir()
	writeFile(t, root, "pkg/a.go", "package pkg\n")
	writeFile(t, root, "pkg/b.go", "package pkg\n")

	out := resolvePromptRefs(root, nil, "看下 @pkg/", nil)
	if !strings.Contains(out, `<directory path="pkg"`) ||
		!strings.Contains(out, "- a.go") || !strings.Contains(out, "- b.go") {
		t.Fatalf("directory listing missing: %q", out)
	}
}

func TestResolvePromptRefsBinarySkipped(t *testing.T) {
	root := t.TempDir()
	abs := filepath.Join(root, "bin.dat")
	if err := os.WriteFile(abs, []byte{'a', 0, 'b'}, 0o644); err != nil {
		t.Fatal(err)
	}
	out := resolvePromptRefs(root, nil, "@bin.dat", nil)
	if !strings.Contains(out, `<file path="bin.dat" skipped="binary"/>`) {
		t.Fatalf("binary file should be marked skipped: %q", out)
	}
}

func TestResolvePromptRefsSkill(t *testing.T) {
	ws := t.TempDir()
	skillRoot := t.TempDir()
	dir := filepath.Join(skillRoot, "demo-skill")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	body := "---\nname: demo-skill\ndescription: demo\n---\n做演示用\n"
	if err := os.WriteFile(filepath.Join(dir, "SKILL.md"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	var catalog skill.AtomicCatalog
	catalog.Store(skill.NewLoader(ws, []string{skillRoot}, nil).Load(context.Background()))

	out := resolvePromptRefs(ws, &catalog, "/demo-skill 跑一下", nil)
	if !strings.Contains(out, `<skill name="demo-skill"`) || !strings.Contains(out, "做演示用") {
		t.Fatalf("skill body not inlined: %q", out)
	}
}

func TestResolvePromptRefsUnknownSkillUntouched(t *testing.T) {
	var catalog skill.AtomicCatalog
	catalog.Store(skill.NewLoader(t.TempDir(), nil, nil).Load(context.Background()))
	prompt := "/not-a-skill 干嘛"
	if out := resolvePromptRefs(t.TempDir(), &catalog, prompt, nil); out != prompt {
		t.Fatalf("unknown skill should stay plain text, got %q", out)
	}
}
