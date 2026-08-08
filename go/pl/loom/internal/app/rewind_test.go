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

package app

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// sha256Sum returns the hex-encoded SHA256 of data.
func sha256Sum(data []byte) string {
	h := sha256.Sum256(data)
	return fmt.Sprintf("%x", h)
}

func TestRestoreRewindChanges_RestoreExistingFile(t *testing.T) {
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	book := workspace.NewFileStateBook()

	original := []byte("original content\n")
	changed := []byte("changed content\n")
	name := "main.go"
	path := filepath.Join(ws, name)
	if err := os.WriteFile(path, changed, 0o644); err != nil {
		t.Fatal(err)
	}

	// LatestAfterHash must match the current on-disk content so the restore
	// does not report a conflict.
	changedHash := sha256Sum(changed)

	out := restoreRewindChanges(validator, book, []session.FileChange{{
		Path:            name,
		BeforeExisted:   true,
		BeforeContent:   original,
		AfterHash:       changedHash,
		Restorable:      true,
		LatestAfterHash: changedHash,
	}})

	if len(out.Restored) != 1 || out.Restored[0] != name {
		t.Fatalf("restored = %v, want [main.go]", out.Restored)
	}
	if len(out.Deleted) != 0 || len(out.Conflicts) != 0 || len(out.Skipped) != 0 {
		t.Fatalf("deleted/conflicts/skipped = %v/%v/%v, want empty", out.Deleted, out.Conflicts, out.Skipped)
	}
	got, _ := os.ReadFile(path)
	if string(got) != string(original) {
		t.Fatalf("file content = %q, want %q", got, original)
	}
}

func TestRestoreRewindChanges_DeleteCreatedFile(t *testing.T) {
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	book := workspace.NewFileStateBook()

	name := "new_file.go"
	path := filepath.Join(ws, name)
	content := []byte("new content\n")
	if err := os.WriteFile(path, content, 0o644); err != nil {
		t.Fatal(err)
	}

	contentHash := sha256Sum(content)

	out := restoreRewindChanges(validator, book, []session.FileChange{{
		Path:            name,
		BeforeExisted:   false,
		BeforeContent:   nil,
		AfterHash:       contentHash,
		Restorable:      true,
		LatestAfterHash: contentHash,
	}})

	if len(out.Deleted) != 1 || out.Deleted[0] != name {
		t.Fatalf("deleted = %v, want [new_file.go]", out.Deleted)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("file should have been deleted")
	}
}

func TestRestoreRewindChanges_SkipsUnrestorable(t *testing.T) {
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	book := workspace.NewFileStateBook()

	out := restoreRewindChanges(validator, book, []session.FileChange{{
		Path:       "big_file.go",
		Restorable: false,
	}})

	if len(out.Skipped) != 1 {
		t.Fatalf("skipped = %v, want 1 entry", out.Skipped)
	}
}

func TestRestoreRewindChanges_ConflictOnHashMismatch(t *testing.T) {
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	book := workspace.NewFileStateBook()

	name := "main.go"
	path := filepath.Join(ws, name)
	currentContent := []byte("externally modified\n")
	// Current on-disk content differs from LatestAfterHash → conflict.
	if err := os.WriteFile(path, currentContent, 0o644); err != nil {
		t.Fatal(err)
	}

	// Use a deliberately wrong LatestAfterHash that does NOT match the
	// on-disk content hash.
	out := restoreRewindChanges(validator, book, []session.FileChange{{
		Path:            name,
		BeforeExisted:   true,
		BeforeContent:   []byte("original\n"),
		AfterHash:       "aaaa",
		Restorable:      true,
		LatestAfterHash: "cccc", // does not match current on-disk hash
	}})

	if len(out.Restored) != 1 {
		t.Fatalf("restored = %v, want 1 (still rewound)", out.Restored)
	}
	if len(out.Conflicts) != 1 {
		t.Fatalf("conflicts = %v, want 1", out.Conflicts)
	}
}

func TestRestoreRewindChanges_EmptyFileSentinel(t *testing.T) {
	ws := t.TempDir()
	validator, err := workspace.NewPathValidator(ws)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	book := workspace.NewFileStateBook()

	name := "empty.go"
	path := filepath.Join(ws, name)
	currentContent := []byte("not empty\n")
	// File currently has content; rewind to empty.
	if err := os.WriteFile(path, currentContent, 0o644); err != nil {
		t.Fatal(err)
	}

	currentHash := sha256Sum(currentContent)

	// BeforeContent is nil (empty-file sentinel — driver may return nil
	// for an empty blob); restorable=true means it should be treated as
	// empty.
	out := restoreRewindChanges(validator, book, []session.FileChange{{
		Path:            name,
		BeforeExisted:   true,
		BeforeContent:   nil,
		AfterHash:       currentHash,
		Restorable:      true,
		LatestAfterHash: currentHash,
	}})

	if len(out.Restored) != 1 {
		t.Fatalf("restored = %v, want 1", out.Restored)
	}
	got, _ := os.ReadFile(path)
	if len(got) != 0 {
		t.Fatalf("file content = %q, want empty", got)
	}
}
