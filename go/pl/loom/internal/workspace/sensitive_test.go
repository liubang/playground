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
// Created: 2026/08/12

package workspace

import (
	"os"
	"path/filepath"
	"testing"
)

func TestIsSensitiveAbsolute(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	canonicalHome := Canonicalize(home)

	tests := []struct {
		path   string
		expect bool
	}{
		// Home-rooted directories: everything beneath them is sensitive.
		{filepath.Join(canonicalHome, ".ssh"), true},
		{filepath.Join(canonicalHome, ".ssh", "id_rsa"), true},
		{filepath.Join(canonicalHome, ".aws", "credentials"), true},
		{filepath.Join(canonicalHome, ".kube", "config"), true},
		{filepath.Join(canonicalHome, ".config", "gcloud", "credentials.db"), true},
		{filepath.Join(canonicalHome, "Library", "Keychains", "login.keychain-db"), true},
		// Home-rooted files.
		{filepath.Join(canonicalHome, ".netrc"), true},
		{filepath.Join(canonicalHome, ".npmrc"), true},
		// Component names are sensitive wherever they appear.
		{"/etc/.env", true},
		{"/srv/repo/.git/config", true},
		{"/srv/secrets/credentials.json", true},
		// Ordinary locations stay readable.
		{"/etc/hosts", false},
		{"/etc/passwd", false},
		{filepath.Join(canonicalHome, ".config", "fish", "config.fish"), false},
		{filepath.Join(canonicalHome, "documents", "notes.md"), false},
		// Relative input is not an absolute verdict.
		{".ssh/id_rsa", false},
	}
	for _, tt := range tests {
		if got := IsSensitiveAbsolute(tt.path); got != tt.expect {
			t.Errorf("IsSensitiveAbsolute(%q) = %v, want %v", tt.path, got, tt.expect)
		}
	}
}

// TestIsSensitiveAbsoluteFollowsHomeOverride proves the canonicalized home
// is cached keyed by the raw $HOME: a later override must still take
// effect (the seatbelt profile tests rely on t.Setenv).
func TestIsSensitiveAbsoluteFollowsHomeOverride(t *testing.T) {
	first := t.TempDir()
	t.Setenv("HOME", first)
	if !IsSensitiveAbsolute(filepath.Join(Canonicalize(first), ".ssh", "id_rsa")) {
		t.Fatal("expected sensitivity under the first HOME")
	}
	second := t.TempDir()
	t.Setenv("HOME", second)
	if !IsSensitiveAbsolute(filepath.Join(Canonicalize(second), ".aws", "credentials")) {
		t.Fatal("expected sensitivity under the overridden HOME")
	}
}

func TestValidateReadAllowsExternalPaths(t *testing.T) {
	root := t.TempDir()
	validator, err := NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}

	// A file outside every validator root: readable after read alignment.
	external := filepath.Join(t.TempDir(), "external.txt")
	if err := os.WriteFile(external, []byte("hi"), 0o644); err != nil {
		t.Fatal(err)
	}
	resolved, err := validator.ValidateRead(external)
	if err != nil {
		t.Fatalf("ValidateRead(external) error = %v", err)
	}
	if resolved != Canonicalize(external) {
		t.Fatalf("ValidateRead(external) = %q, want %q", resolved, Canonicalize(external))
	}

	// A relative escape resolves outside the root but stays readable: the
	// read boundary is the sensitive list, not the workspace root.
	sibling := filepath.Join(filepath.Dir(root), "sibling.txt")
	if err := os.WriteFile(sibling, []byte("hi"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := validator.ValidateRead("../sibling.txt"); err != nil {
		t.Fatalf("ValidateRead(../sibling.txt) error = %v", err)
	}

	// The write boundary is untouched: Validate still rejects escapes.
	if _, err := validator.Validate(external); err == nil {
		t.Fatal("Validate(external) must still fail: read alignment never widens writes")
	}
}

func TestResolveWrite(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	root := t.TempDir()
	validator, err := NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}

	// Workspace-confined: lexical semantics, external=false.
	resolved, external, err := validator.ResolveWrite("dir/file.go")
	if err != nil || external {
		t.Fatalf("ResolveWrite(internal) = external=%v, err=%v", external, err)
	}
	if resolved.Absolute != filepath.Join(validator.Root(), "dir", "file.go") {
		t.Fatalf("internal Absolute = %q", resolved.Absolute)
	}

	// External absolute path: canonical form, external=true.
	ext := filepath.Join(t.TempDir(), "notes", "a.txt")
	resolved, external, err = validator.ResolveWrite(ext)
	if err != nil || !external {
		t.Fatalf("ResolveWrite(external) = external=%v, err=%v", external, err)
	}
	if resolved.Absolute != Canonicalize(ext) || resolved.Display != filepath.ToSlash(Canonicalize(ext)) {
		t.Fatalf("external resolved = %+v", resolved)
	}

	// A relative escape is an external write candidate (policy-gated).
	if _, external, err = validator.ResolveWrite("../sibling.txt"); err != nil || !external {
		t.Fatalf("ResolveWrite(../sibling.txt) = external=%v, err=%v", external, err)
	}

	// Sensitive locations stay denied.
	if _, _, err = validator.ResolveWrite(filepath.Join(home, ".ssh", "config")); err == nil {
		t.Fatal("ResolveWrite(~/.ssh/config) must be denied")
	}
	if _, _, err = validator.ResolveWrite(".git/config"); err == nil {
		t.Fatal("ResolveWrite(.git/config) must be denied")
	}
}

// TestCoversSensitiveLocation proves the writable-grant gate rejects both
// sensitive locations themselves AND their ancestors: a write grant on "~"
// would otherwise open ~/.ssh/authorized_keys to a plain file-write, which
// the seatbelt read/unlink denies do not cover.
func TestCoversSensitiveLocation(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	canonicalHome := Canonicalize(home)

	tests := []struct {
		path   string
		expect bool
	}{
		// Sensitive locations themselves.
		{filepath.Join(canonicalHome, ".ssh"), true},
		{filepath.Join(canonicalHome, ".aws", "credentials"), true},
		{filepath.Join(canonicalHome, ".netrc"), true},
		{filepath.Join(canonicalHome, "Library", "Keychains"), true},
		// Ancestors of sensitive locations: the whole point of the gate.
		{canonicalHome, true},
		{filepath.Join(canonicalHome, ".config"), true}, // covers .config/gcloud
		{filepath.Join(canonicalHome, "Library"), true}, // covers Library/Keychains
		{filepath.Dir(canonicalHome), true},             // covers the home itself
		{"/", true},
		// Ordinary data directories stay grantable. Deliberately not /tmp:
		// t.TempDir() lands under /tmp on Linux CI, so HOME becomes a
		// descendant of /tmp and /tmp is correctly an ancestor of sensitive
		// locations — a platform-independent directory gives a stable verdict.
		{filepath.Join(canonicalHome, "Library", "Logs", "dsx"), false},
		{filepath.Join(canonicalHome, ".talos"), false},
		{filepath.Join(canonicalHome, "projects", "repo"), false},
		{"/opt", false},
		{"/usr/local/share", false},
		// Relative input is not an absolute verdict.
		{".ssh", false},
	}
	for _, tt := range tests {
		if got := CoversSensitiveLocation(tt.path); got != tt.expect {
			t.Errorf("CoversSensitiveLocation(%q) = %v, want %v", tt.path, got, tt.expect)
		}
	}
}

func TestValidateReadDeniesSensitivePaths(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	root := t.TempDir()
	validator, err := NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}

	targets := []string{
		filepath.Join(home, ".ssh", "id_rsa"),
		filepath.Join(home, ".netrc"),
		filepath.Join(t.TempDir(), "project", ".env"),
		filepath.Join(root, ".git", "config"),
	}
	for _, target := range targets {
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(target, []byte("secret"), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := validator.ValidateRead(target); err == nil {
			t.Errorf("ValidateRead(%q) must be denied", target)
		}
	}
}
