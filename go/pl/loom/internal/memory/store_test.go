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

package memory

import (
	"context"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

func TestOpenStoreCreatesDirs(t *testing.T) {
	root := t.TempDir()
	memDir := filepath.Join(root, "memories")
	s, err := OpenStore(memDir)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	if s.Root() != memDir {
		t.Errorf("Root() = %q, want %q", s.Root(), memDir)
	}
	// Check sub-directories were created.
	for _, dir := range []string{RolloutDir, SkillsDir, NotesDir} {
		p := filepath.Join(memDir, dir)
		if fi, err := os.Stat(p); err != nil || !fi.IsDir() {
			t.Errorf("subdirectory %q not created", dir)
		}
	}
}

func TestOpenStoreRequiresRoot(t *testing.T) {
	// The store performs no path conventions of its own: an empty root is
	// an error, and the caller derives it from the loom home.
	if _, err := OpenStore(""); err == nil {
		t.Fatal("OpenStore with empty root = nil error, want error")
	}
}

func TestSummaryRoundTrip(t *testing.T) {
	s := newTestStore(t)
	content := "# Memory Summary\n\nUser prefers Go over Python."
	if err := s.WriteSummary(content); err != nil {
		t.Fatalf("WriteSummary: %v", err)
	}
	got, err := s.ReadSummary()
	if err != nil {
		t.Fatalf("ReadSummary: %v", err)
	}
	if got != content {
		t.Errorf("ReadSummary() = %q, want %q", got, content)
	}
}

func TestSummaryNotFound(t *testing.T) {
	s := newTestStore(t)
	got, err := s.ReadSummary()
	if err != nil {
		t.Fatalf("ReadSummary: %v", err)
	}
	if got != "" {
		t.Errorf("ReadSummary() = %q, want empty", got)
	}
}

func TestMainRoundTrip(t *testing.T) {
	s := newTestStore(t)
	content := "# MEMORY.md\n\n## User Preferences\n- Go"
	if err := s.WriteMain(content); err != nil {
		t.Fatalf("WriteMain: %v", err)
	}
	got, err := s.ReadMain()
	if err != nil {
		t.Fatalf("ReadMain: %v", err)
	}
	if got != content {
		t.Errorf("ReadMain() = %q, want %q", got, content)
	}
}

func TestRawAppend(t *testing.T) {
	s := newTestStore(t)
	if err := s.AppendRaw("first line\n"); err != nil {
		t.Fatalf("AppendRaw: %v", err)
	}
	if err := s.AppendRaw("second line\n"); err != nil {
		t.Fatalf("AppendRaw: %v", err)
	}
	got, err := s.ReadRaw()
	if err != nil {
		t.Fatalf("ReadRaw: %v", err)
	}
	want := "first line\nsecond line\n"
	if got != want {
		t.Errorf("ReadRaw() = %q, want %q", got, want)
	}
}

func TestRolloutSummary(t *testing.T) {
	s := newTestStore(t)
	slug := "2026-08-02T12-00-00-abc12345"
	content := "## Rollout Summary\n\nFixed bazel build."
	if err := s.WriteRolloutSummary(slug, content); err != nil {
		t.Fatalf("WriteRolloutSummary: %v", err)
	}
	got, err := s.ReadRolloutSummary(slug)
	if err != nil {
		t.Fatalf("ReadRolloutSummary: %v", err)
	}
	if got != content {
		t.Errorf("ReadRolloutSummary() = %q, want %q", got, content)
	}
}

func TestListRolloutSummaries(t *testing.T) {
	s := newTestStore(t)
	slugs := []string{"alpha", "beta", "gamma"}
	for _, slug := range slugs {
		if err := s.WriteRolloutSummary(slug, "content"); err != nil {
			t.Fatalf("WriteRolloutSummary(%s): %v", slug, err)
		}
	}
	got, err := s.ListRolloutSummaries()
	if err != nil {
		t.Fatalf("ListRolloutSummaries: %v", err)
	}
	sort.Strings(got)
	if len(got) != 3 {
		t.Errorf("got %d summaries, want 3", len(got))
	}
}

func TestAddNoteAndList(t *testing.T) {
	s := newTestStore(t)
	filename := "2026-08-02T12-00-00-prefer-go.md"
	content := "User prefers Go over Python"
	if err := s.AddNote(filename, content); err != nil {
		t.Fatalf("AddNote: %v", err)
	}
	notes, err := s.ListNotes()
	if err != nil {
		t.Fatalf("ListNotes: %v", err)
	}
	if len(notes) != 1 || notes[0] != filename {
		t.Errorf("ListNotes() = %v, want [%s]", notes, filename)
	}
	got, err := s.ReadNote(filename)
	if err != nil {
		t.Fatalf("ReadNote: %v", err)
	}
	if got != content {
		t.Errorf("ReadNote() = %q, want %q", got, content)
	}
}

func TestListDirectory(t *testing.T) {
	s := newTestStore(t)
	// Write some files to populate the directory.
	s.WriteSummary("summary")
	s.WriteMain("main")
	s.WriteRolloutSummary("test-rollout", "content")

	entries, err := s.List("", 100)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	// Should see at least the files and directories we created.
	names := make(map[string]bool)
	for _, e := range entries {
		names[e.Path] = true
	}
	if !names["memory_summary.md"] {
		t.Error("expected memory_summary.md in listing")
	}
	if !names["MEMORY.md"] {
		t.Error("expected MEMORY.md in listing")
	}
	if !names[RolloutDir] {
		t.Errorf("expected %s in listing", RolloutDir)
	}
}

func TestReadFileWithOffset(t *testing.T) {
	s := newTestStore(t)
	content := "line1\nline2\nline3\nline4\nline5"
	if err := s.WriteMain(content); err != nil {
		t.Fatalf("WriteMain: %v", err)
	}
	got, total, err := s.ReadFile(MainFile, 2, 2)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if total != 5 {
		t.Errorf("total = %d, want 5", total)
	}
	// lines[1]="line2", lines[2]="line3", join with newline
	if got != "line2\nline3" {
		t.Errorf("ReadFile(2,2) = %q, want %q", got, "line2\nline3")
	}
}

func TestSearch(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("# Preferences\n\nUser prefers Go\nUser dislikes Java\n")
	s.WriteSummary("Summary: Go preference")

	matches, err := s.Search("Go", 100)
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if len(matches) < 2 {
		t.Errorf("expected at least 2 matches, got %d", len(matches))
	}
}

func TestSearchLimit(t *testing.T) {
	s := newTestStore(t)
	// Create a file with many matching lines.
	var lines []string
	for i := 0; i < 100; i++ {
		lines = append(lines, "keyword match "+strings.Repeat("x", i))
	}
	s.WriteMain(strings.Join(lines, "\n"))
	matches, err := s.Search("keyword", 5)
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if len(matches) != 5 {
		t.Errorf("expected 5 matches, got %d", len(matches))
	}
}

func TestCleanup(t *testing.T) {
	s := newTestStore(t)
	// Create an old rollout summary.
	oldSlug := "old-summary"
	if err := s.WriteRolloutSummary(oldSlug, "old content"); err != nil {
		t.Fatalf("WriteRolloutSummary: %v", err)
	}
	// Backdate the file.
	oldPath := filepath.Join(s.Root(), RolloutDir, oldSlug+".md")
	oldTime := time.Now().Add(-48 * time.Hour)
	if err := os.Chtimes(oldPath, oldTime, oldTime); err != nil {
		t.Fatalf("Chtimes: %v", err)
	}
	// Create a new rollout summary.
	if err := s.WriteRolloutSummary("new-summary", "new content"); err != nil {
		t.Fatalf("WriteRolloutSummary: %v", err)
	}
	// Cleanup with 24h max age.
	removed, err := s.Cleanup(24 * time.Hour)
	if err != nil {
		t.Fatalf("Cleanup: %v", err)
	}
	if removed != 1 {
		t.Errorf("removed = %d, want 1", removed)
	}
	// Verify old is gone, new remains.
	if _, err := os.Stat(oldPath); !os.IsNotExist(err) {
		t.Error("old summary should be removed")
	}
}

func TestPathEscapes(t *testing.T) {
	s := newTestStore(t)
	_, err := s.List("../../etc", 10)
	if err == nil {
		t.Error("expected error for path escape")
	}
	_, _, err = s.ReadFile("../../../etc/passwd", 0, 0)
	if err == nil {
		t.Error("expected error for path escape in ReadFile")
	}
}

func TestReadFileOffsetBeyondTotal(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("line1\nline2\nline3")
	// offset=500 but file only has 3 lines — should return empty, not the whole file.
	got, total, err := s.ReadFile(MainFile, 500, 0)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if total != 3 {
		t.Errorf("total = %d, want 3", total)
	}
	if got != "" {
		t.Errorf("ReadFile(500,0) = %q, want empty string", got)
	}
}

func TestReadFileRejectsGitPath(t *testing.T) {
	s := newTestStore(t)
	// Create a .git directory inside the memory root (as InitGit would).
	gitDir := filepath.Join(s.Root(), ".git")
	if err := os.MkdirAll(gitDir, 0o755); err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(gitDir, "config")
	if err := os.WriteFile(configPath, []byte("[core]\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, _, err := s.ReadFile(".git/config", 0, 0)
	if err == nil {
		t.Error("expected error for .git path in ReadFile")
	}
}

func TestListSkipGitDoesNotAffectCount(t *testing.T) {
	s := newTestStore(t)
	// Write two files and init .git so there are 3 entries (2 files + .git).
	s.WriteSummary("s")
	s.WriteMain("m")
	// InitGit creates the .git directory.
	ctx := context.Background()
	_ = s.InitGit(ctx) // error OK — we just need .git to exist

	entries, err := s.List("", 10)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	// Should get exactly 2 non-.git entries + rollout_summaries + skills + extensions dirs.
	// The important thing is that .git is not counted.
	for _, e := range entries {
		if strings.HasPrefix(e.Path, ".git") {
			t.Errorf("List returned .git entry: %s", e.Path)
		}
	}
}

func TestSearchEmptyQuery(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("some content")
	matches, err := s.Search("", 10)
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if len(matches) != 0 {
		t.Errorf("Search('') = %d matches, want 0", len(matches))
	}
}

func TestSearchDefaultMaxResults(t *testing.T) {
	s := newTestStore(t)
	s.WriteMain("content")
	// maxResults=0 should default to DefaultSearchMaxResults, not return 0 matches.
	matches, err := s.Search("content", 0)
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if len(matches) != 1 {
		t.Errorf("Search with maxResults=0 got %d matches, want 1", len(matches))
	}
}

func TestAddNotePathEscape(t *testing.T) {
	s := newTestStore(t)
	err := s.AddNote("../../etc/evil.md", "pwned")
	if err == nil {
		t.Error("expected error for path escape in AddNote")
	}
}

func TestReadNotePathEscape(t *testing.T) {
	s := newTestStore(t)
	_, err := s.ReadNote("../../../etc/passwd")
	if err == nil {
		t.Error("expected error for path escape in ReadNote")
	}
}

func newTestStore(t *testing.T) *Store {
	t.Helper()
	root := t.TempDir()
	s, err := OpenStore(root)
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	return s
}
