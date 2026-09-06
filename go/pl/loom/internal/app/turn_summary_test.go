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
// Created: 2026/09/06

package app

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func newStatValidator(t *testing.T) (*workspace.PathValidator, string) {
	t.Helper()
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	return validator, ws
}

func TestBuildRunFileStat_ModifiedFileDiff(t *testing.T) {
	validator, ws := newStatValidator(t)
	if err := os.WriteFile(filepath.Join(ws, "a.txt"), []byte("v2\n"), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "a.txt",
		BeforeExisted: true,
		BeforeContent: []byte("v1\n"),
		Restorable:    true,
	}, 3)
	if stat.NotComparable != "" {
		t.Fatalf("NotComparable = %q, want empty", stat.NotComparable)
	}
	if stat.Added != 1 || stat.Removed != 1 {
		t.Fatalf("added/removed = %d/%d, want 1/1", stat.Added, stat.Removed)
	}
	if !strings.Contains(stat.Diff, "-v1") || !strings.Contains(stat.Diff, "+v2") {
		t.Fatalf("diff = %q, want - v1/+ v2 lines", stat.Diff)
	}
	if stat.BeforeSize != 3 || stat.AfterSize != 3 {
		t.Fatalf("sizes = %d/%d, want 3/3", stat.BeforeSize, stat.AfterSize)
	}
	if stat.Edits != 3 {
		t.Fatalf("Edits = %d, want 3", stat.Edits)
	}
}

func TestBuildRunFileStat_NoGitRequired(t *testing.T) {
	// The whole point of the review projection: compare ledger-before vs
	// current disk content. A workspace without .git must behave the same
	// as a tracked one (this test's TempDir is intentionally not a repo).
	validator, ws := newStatValidator(t)
	if _, err := os.Stat(filepath.Join(ws, ".git")); !os.IsNotExist(err) {
		t.Fatalf("workspace unexpectedly is a git repo")
	}
	if err := os.WriteFile(filepath.Join(ws, "new.txt"), []byte("hello\n"), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "new.txt",
		BeforeExisted: false, // the turn created the file
		BeforeContent: []byte{},
		Restorable:    true,
	}, 1)
	if !stat.Created {
		t.Fatalf("Created = false, want true for a turn-created file")
	}
	if stat.Added != 1 || stat.Removed != 0 {
		t.Fatalf("added/removed = %d/%d, want 1/0 for a created file", stat.Added, stat.Removed)
	}
	if !strings.Contains(stat.Diff, "+hello") {
		t.Fatalf("diff = %q, want an all-addition unified diff", stat.Diff)
	}
}

func TestBuildRunFileStat_DeletedAfterTurnIsAllRemoval(t *testing.T) {
	validator, _ := newStatValidator(t)
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "gone.txt",
		BeforeExisted: true,
		BeforeContent: []byte("a\nb\n"),
		Restorable:    true,
	}, 1)
	if stat.NotComparable != "" {
		t.Fatalf("NotComparable = %q, want empty (deletion is a comparable state)", stat.NotComparable)
	}
	if stat.Added != 0 || stat.Removed != 2 {
		t.Fatalf("added/removed = %d/%d, want 0/2", stat.Added, stat.Removed)
	}
	if !strings.Contains(stat.Diff, "-a") || !strings.Contains(stat.Diff, "-b") {
		t.Fatalf("diff = %q, want an all-removal diff", stat.Diff)
	}
	// All-removal stats are exact even for huge inputs (no LCS involved).
	if stat.DiffTruncated {
		t.Fatalf("DiffTruncated = true, want false for the deletion path")
	}
}

func TestBuildRunFileStat_CreatedThenDeletedIsNotComparable(t *testing.T) {
	validator, _ := newStatValidator(t)
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "flash.txt",
		BeforeExisted: false, // created by the turn...
		BeforeContent: []byte{},
		Restorable:    true,
	}, 1)
	// ...and deleted again: the before side is empty and the after side is
	// gone, so there is no content on either side to compare.
	if !strings.Contains(stat.NotComparable, "删除") {
		t.Fatalf("NotComparable = %q, want a deletion reason", stat.NotComparable)
	}
}

func TestBuildRunFileStat_UncapturedBeforeContent(t *testing.T) {
	validator, ws := newStatValidator(t)
	if err := os.WriteFile(filepath.Join(ws, "big.txt"), []byte("current\n"), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "big.txt",
		BeforeExisted: true,
		Restorable:    false, // oversized at record time: never captured
	}, 1)
	if stat.NotComparable == "" {
		t.Fatalf("NotComparable = \"\", want a reason (content never captured)")
	}
	if stat.BeforeSize != -1 {
		t.Fatalf("BeforeSize = %d, want -1 (unknown)", stat.BeforeSize)
	}
	// The current size is still reported even when the diff is not.
	if stat.AfterSize != int64(len("current\n")) {
		t.Fatalf("AfterSize = %d, want %d", stat.AfterSize, len("current\n"))
	}
}

func TestBuildRunFileStat_IdenticalContentHasNoDiff(t *testing.T) {
	validator, ws := newStatValidator(t)
	if err := os.WriteFile(filepath.Join(ws, "same.txt"), []byte("x\n"), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "same.txt",
		BeforeExisted: true,
		BeforeContent: []byte("x\n"),
		Restorable:    true,
	}, 1)
	if stat.NotComparable != "" {
		t.Fatalf("NotComparable = %q, want empty", stat.NotComparable)
	}
	if stat.Diff != "" || stat.Added != 0 || stat.Removed != 0 {
		t.Fatalf("identical content must produce empty diff/0 counts, got %q +%d/%d",
			stat.Diff, stat.Added, stat.Removed)
	}
}

func TestBuildRunFileStat_BinaryIsNotComparable(t *testing.T) {
	validator, ws := newStatValidator(t)
	if err := os.WriteFile(filepath.Join(ws, "blob.bin"), []byte{0x00, 0xff, 0x01}, 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	stat := buildRunFileStat(validator, session.FileChange{
		Path:          "blob.bin",
		BeforeExisted: false,
		BeforeContent: []byte{},
		Restorable:    true,
	}, 1)
	if !strings.Contains(stat.NotComparable, "二进制") {
		t.Fatalf("NotComparable = %q, want a binary reason", stat.NotComparable)
	}
}
