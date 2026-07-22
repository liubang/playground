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
// Created: 2026/07/22 21:10

package workspace

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPathValidatorValidPath(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	resolved, err := v.Validate("test.go")
	if err != nil {
		t.Fatalf("Validate error: %v", err)
	}
	expected := filepath.Join(v.Root(), "test.go")
	if resolved != expected {
		t.Fatalf("expected %s, got %s", expected, resolved)
	}
}

func TestPathValidatorAbsolutePath(t *testing.T) {
	dir := t.TempDir()
	resolvedDir, err := filepath.EvalSymlinks(dir)
	if err != nil {
		t.Fatalf("eval symlinks: %v", err)
	}
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	resolved, err := v.Validate(filepath.Join(resolvedDir, "test.go"))
	if err != nil {
		t.Fatalf("Validate error: %v", err)
	}
	expected := filepath.Join(v.Root(), "test.go")
	if resolved != expected {
		t.Fatalf("expected %s, got %s", expected, resolved)
	}
}

func TestPathValidatorPathTraversal(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	_, err = v.Validate("../../../etc/passwd")
	if err == nil {
		t.Fatal("expected error for path traversal")
	}
}

func TestPathValidatorNullByte(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	_, err = v.Validate("test\x00.go")
	if err == nil {
		t.Fatal("expected error for null byte")
	}
}

func TestPathValidatorEscapeViaDotDot(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	_, err = v.Validate("../../secret")
	if err == nil {
		t.Fatal("expected error for escaping path")
	}
}

func TestPathValidatorSymlinkEscape(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	linkPath := filepath.Join(dir, "escape")
	_ = os.Symlink("/etc/passwd", linkPath)

	_, err = v.Validate("escape")
	if err == nil {
		t.Fatal("expected error for symlink escaping workspace")
	}
}

func TestPathValidatorValidSymlink(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	realFile := filepath.Join(v.Root(), "real.go")
	_ = os.WriteFile(filepath.Join(dir, "real.go"), []byte("hi"), 0o644)
	linkPath := filepath.Join(dir, "link.go")
	_ = os.Symlink("real.go", linkPath)

	resolved, err := v.Validate("link.go")
	if err != nil {
		t.Fatalf("Validate error: %v", err)
	}
	if resolved != realFile {
		t.Fatalf("expected %s, got %s", realFile, resolved)
	}
}

func TestIsSensitive(t *testing.T) {
	tests := []struct {
		path   string
		expect bool
	}{
		{".git", true},
		{".ssh", true},
		{".env", true},
		{"credentials.json", true},
		{"service-account.json", true},
		{"main.go", false},
		{"README.md", false},
		{".gitignore", false},
	}

	for _, tt := range tests {
		got := IsSensitive(tt.path)
		if got != tt.expect {
			t.Errorf("IsSensitive(%q) = %v, want %v", tt.path, got, tt.expect)
		}
	}
}

func TestPathValidatorNonExistentSubdirectory(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	resolved, err := v.Validate(filepath.Join("pkg", "new_file.go"))
	if err != nil {
		t.Fatalf("Validate error: %v", err)
	}
	expected := filepath.Join(v.Root(), "pkg", "new_file.go")
	if resolved != expected {
		t.Fatalf("expected %s, got %s", expected, resolved)
	}
}

func TestPathValidatorRootItself(t *testing.T) {
	dir := t.TempDir()
	v, err := NewPathValidator(dir)
	if err != nil {
		t.Fatalf("NewPathValidator error: %v", err)
	}

	resolved, err := v.Validate(".")
	if err != nil {
		t.Fatalf("Validate error: %v", err)
	}
	if resolved != v.Root() {
		t.Fatalf("expected %s, got %s", v.Root(), resolved)
	}
}

func TestResolveLexicalRejectsSensitiveAndEscape(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	if _, err := v.ResolveLexical(filepath.Join(root, ".git", "config")); err == nil {
		t.Fatal("expected sensitive path rejection")
	}
	if _, err := v.ResolveLexical("../outside.txt"); err == nil {
		t.Fatal("expected workspace escape rejection")
	}
}

func TestSnapshotReportsMetadata(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	path := filepath.Join(root, "file.txt")
	data := []byte("hello world\n")
	if err := os.WriteFile(path, data, 0o640); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}

	snapshot, err := v.Snapshot("file.txt")
	if err != nil {
		t.Fatalf("Snapshot() error = %v", err)
	}
	if snapshot.Path != "file.txt" {
		t.Fatalf("snapshot.Path = %q, want file.txt", snapshot.Path)
	}
	if snapshot.SHA256 != hexSHA256(data) {
		t.Fatalf("snapshot.SHA256 = %q, want %q", snapshot.SHA256, hexSHA256(data))
	}
	if snapshot.Size != int64(len(data)) {
		t.Fatalf("snapshot.Size = %d, want %d", snapshot.Size, len(data))
	}
	if snapshot.Mode.Perm() != 0o640 {
		t.Fatalf("snapshot.Mode = %o, want 640", snapshot.Mode.Perm())
	}
	if snapshot.MTime.IsZero() {
		t.Fatal("snapshot.MTime should be set")
	}
}

func TestSnapshotRejectsSymlinkAndSensitivePath(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	if err := os.WriteFile(filepath.Join(root, "target.txt"), []byte("x"), 0o644); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
	if err := os.Symlink("target.txt", filepath.Join(root, "link.txt")); err != nil {
		t.Fatalf("os.Symlink() error = %v", err)
	}
	if _, err := v.Snapshot("link.txt"); err == nil || !strings.Contains(err.Error(), "symlink") {
		t.Fatalf("expected symlink error, got %v", err)
	}
	if _, err := v.Snapshot(filepath.Join(root, ".git", "config")); err == nil {
		t.Fatal("expected sensitive path error")
	}
}

func TestAtomicWriteExistingFilePreservesPermissions(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	path := filepath.Join(root, "file.txt")
	oldData := []byte("before\n")
	if err := os.WriteFile(path, oldData, 0o600); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}

	result, err := v.AtomicWrite("file.txt", []byte("after\n"), AtomicWriteOptions{
		ExpectedHash: hexSHA256(oldData),
		SyncParent:   true,
	})
	if err != nil {
		t.Fatalf("AtomicWrite() error = %v", err)
	}
	if result.Mode.Perm() != 0o600 {
		t.Fatalf("result.Mode = %o, want 600", result.Mode.Perm())
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "after\n" {
		t.Fatalf("content = %q, want after", string(content))
	}
}

func TestAtomicWriteNewFileRequiresEmptyHash(t *testing.T) {
	v, _ := newWorkspaceValidator(t)
	if _, err := v.AtomicWrite("new.txt", []byte("hello"), AtomicWriteOptions{ExpectedHash: hexSHA256([]byte("not-empty"))}); err == nil {
		t.Fatal("expected new file expected-hash rejection")
	}
	result, err := v.AtomicWrite("new.txt", []byte("hello"), AtomicWriteOptions{ExpectedHash: EmptyFileSHA256})
	if err != nil {
		t.Fatalf("AtomicWrite() new file error = %v", err)
	}
	if result.Path != "new.txt" {
		t.Fatalf("result.Path = %q, want new.txt", result.Path)
	}
}

func TestAtomicWriteRejectsConcurrentModificationAndNoTempResidue(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	path := filepath.Join(root, "race.txt")
	if err := os.WriteFile(path, []byte("old"), 0o644); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}

	err := withTempCreationHook(func() {
		if writeErr := os.WriteFile(path, []byte("other"), 0o644); writeErr != nil {
			t.Fatalf("os.WriteFile() race update error = %v", writeErr)
		}
	}, func() error {
		_, err := v.AtomicWrite("race.txt", []byte("new"), AtomicWriteOptions{ExpectedHash: hexSHA256([]byte("old"))})
		return err
	})
	if err == nil || !strings.Contains(err.Error(), "expected hash mismatch") {
		t.Fatalf("expected hash mismatch, got %v", err)
	}
	entries, err := filepath.Glob(filepath.Join(root, ".race.txt.*.tmp"))
	if err != nil {
		t.Fatalf("filepath.Glob() error = %v", err)
	}
	if len(entries) != 0 {
		t.Fatalf("unexpected temp residue: %v", entries)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "other" {
		t.Fatalf("content = %q, want other", string(content))
	}
}

func TestAtomicWritePreservesXAttrWhenSupported(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	path := filepath.Join(root, "xattr.txt")
	oldData := []byte("before\n")
	if err := os.WriteFile(path, oldData, 0o600); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
	const attrName = "user.loom.test"
	const attrValue = "preserve-me"
	if err := setTestXAttr(path, attrName, []byte(attrValue)); err != nil {
		if errors.Is(err, errXAttrUnsupported) {
			t.Skipf("xattr unsupported: %v", err)
		}
		t.Fatalf("setTestXAttr() error = %v", err)
	}

	_, err := v.AtomicWrite("xattr.txt", []byte("after\n"), AtomicWriteOptions{ExpectedHash: hexSHA256(oldData)})
	if err != nil {
		t.Fatalf("AtomicWrite() error = %v", err)
	}
	value, err := readTestXAttr(path, attrName)
	if err != nil {
		t.Fatalf("readTestXAttr() error = %v", err)
	}
	if string(value) != attrValue {
		t.Fatalf("xattr value = %q, want %q", string(value), attrValue)
	}
}

func TestAtomicWriteRejectsSymlinkAndSensitivePath(t *testing.T) {
	v, root := newWorkspaceValidator(t)
	if err := os.WriteFile(filepath.Join(root, "target.txt"), []byte("x"), 0o644); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
	if err := os.Symlink("target.txt", filepath.Join(root, "link.txt")); err != nil {
		t.Fatalf("os.Symlink() error = %v", err)
	}
	if _, err := v.AtomicWrite("link.txt", []byte("y"), AtomicWriteOptions{ExpectedHash: hexSHA256([]byte("x"))}); err == nil {
		t.Fatal("expected symlink rejection")
	}
	if _, err := v.AtomicWrite(filepath.Join(root, ".git", "config"), []byte("z"), AtomicWriteOptions{ExpectedHash: EmptyFileSHA256}); err == nil {
		t.Fatal("expected sensitive path rejection")
	}
}

func newWorkspaceValidator(t *testing.T) (*PathValidator, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "workspace")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatalf("os.MkdirAll() error = %v", err)
	}
	v, err := NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	return v, root
}

func hexSHA256(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func withTempCreationHook(hook func(), fn func() error) error {
	prev := afterTempFileCreatedHook
	afterTempFileCreatedHook = func(string) {
		if hook != nil {
			hook()
		}
		afterTempFileCreatedHook = nil
	}
	defer func() {
		afterTempFileCreatedHook = prev
	}()
	return fn()
}

var errXAttrUnsupported = errors.New("xattr unsupported")

func setTestXAttr(path, name string, value []byte) error {
	if err := writeFileXAttr(path, name, value); err != nil {
		if strings.Contains(err.Error(), "unsupported") || strings.Contains(err.Error(), "operation not permitted") || strings.Contains(err.Error(), "not supported") {
			return errXAttrUnsupported
		}
		return err
	}
	return nil
}

func readTestXAttr(path, name string) ([]byte, error) {
	entries, err := readFileXAttrs(path)
	if err != nil {
		if strings.Contains(err.Error(), "unsupported") || strings.Contains(err.Error(), "operation not permitted") || strings.Contains(err.Error(), "not supported") {
			return nil, errXAttrUnsupported
		}
		return nil, err
	}
	for _, entry := range entries {
		if entry.name == name {
			return append([]byte(nil), entry.value...), nil
		}
	}
	return nil, os.ErrNotExist
}
