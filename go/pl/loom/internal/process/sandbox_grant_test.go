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
// Created: 2026/07/27

// Grant widening and workspace-metadata protection only exist on darwin
// (SeatbeltSandbox), so these tests are platform-gated like the other
// seatbelt tests.
//
//go:build darwin

package process

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// TestSeatbeltProfileProtectsWorkspaceMetadata is the regression guard for
// the P0 fix (PERMISSION_DESIGN §6.3): the writable workspace root must
// exclude git hooks/config and the loom rule directory, in both literal
// and subpath forms so first-time creation is blocked too.
func TestSeatbeltProfileProtectsWorkspaceMetadata(t *testing.T) {
	sandbox := SeatbeltSandbox{}
	profile, err := sandbox.profile(SandboxSpec{
		ExecutablePath: "/bin/echo",
		WorkingDir:     "/tmp/ws",
		WorkspaceRoot:  "/tmp/ws",
	})
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	// /tmp is a symlink into /private on macOS and seatbelt matches
	// canonical paths, so expectations go through workspacepkg.Canonicalize too.
	canonicalWS := workspacepkg.Canonicalize("/tmp/ws")
	for _, rel := range []string{".git/hooks", ".git/config", ".loom"} {
		for _, form := range []string{"literal", "subpath"} {
			rule := `(deny file-write* (` + form + ` "` + canonicalWS + `/` + rel + `"))`
			if !strings.Contains(profile, rule) {
				t.Fatalf("profile missing protection %q:\n%s", rule, profile)
			}
		}
	}
	// The workspace itself must remain writable, and the protection
	// denies must come AFTER it (seatbelt's last match wins, so the deny
	// also holds when the workspace sits inside another writable root).
	allow := `(allow file-write* (subpath "` + canonicalWS + `"))`
	if !strings.Contains(profile, allow) {
		t.Fatalf("profile missing workspace write allow:\n%s", profile)
	}
	if strings.LastIndex(profile, allow) > strings.LastIndex(profile, "(deny file-write*") {
		t.Fatalf("protection denies must follow the write allows:\n%s", profile)
	}
}

// TestSeatbeltProfileDeniesSensitiveUnlink checks that credential paths
// cannot be probed through destructive filesystem operations — and that
// the unlink denies come AFTER the write allows (seatbelt's last match
// wins; emitted earlier they are dead rules whenever a widened write
// root covers the sensitive path).
func TestSeatbeltProfileDeniesSensitiveUnlink(t *testing.T) {
	// sensitiveReadDenies resolves the user's home; hermetic test runners
	// (bazel) do not set $HOME.
	t.Setenv("HOME", t.TempDir())
	sandbox := SeatbeltSandbox{}
	profile, err := sandbox.profile(SandboxSpec{
		ExecutablePath: "/bin/echo",
		WorkingDir:     "/tmp",
		WorkspaceRoot:  "/tmp",
	})
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	if !strings.Contains(profile, "(deny file-write-unlink") {
		t.Fatalf("profile missing unlink denies:\n%s", profile)
	}
	lastAllow := strings.LastIndex(profile, "(allow file-write*")
	lastUnlinkDeny := strings.LastIndex(profile, "(deny file-write-unlink")
	if lastAllow < 0 || lastUnlinkDeny < lastAllow {
		t.Fatalf("unlink denies must follow the write allows (allow@%d, deny@%d):\n%s", lastAllow, lastUnlinkDeny, profile)
	}
}

// TestSeatbeltRenameThenReadBypassDenied is the end-to-end proof for the
// H1 review finding: even with a write root covering the sensitive path
// (a grant.write over the fake home), renaming ~/.ssh aside and reading
// it stays denied.
func TestSeatbeltRenameThenReadBypassDenied(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)
	home := t.TempDir()
	t.Setenv("HOME", home)
	if err := os.MkdirAll(filepath.Join(home, ".ssh"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(home, ".ssh", "id_rsa"), []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	script := `import os; os.rename(os.path.expanduser("~/.ssh"), os.path.expanduser("~/.ssh_moved"))`
	result, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3", Args: []string{"-c", script}, Cwd: root, Timeout: 15 * time.Second,
	}, Grant{WritablePaths: []string{home}})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if result.ExitCode == 0 {
		t.Fatal("rename of a sensitive dir must stay denied even under a covering write grant")
	}
	if _, err := os.Stat(filepath.Join(home, ".ssh", "id_rsa")); err != nil {
		t.Fatalf("sensitive file moved despite the protection: %v", err)
	}
}

// TestSeatbeltProtectedMetadataWriteDenied is the end-to-end proof: a
// sandboxed process must not be able to install a git hook inside the
// writable workspace.
func TestSeatbeltProtectedMetadataWriteDenied(t *testing.T) {
	requireSeatbelt(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	hookPath := filepath.Join(root, ".git", "hooks", "pre-commit")
	if err := os.MkdirAll(filepath.Dir(hookPath), 0o755); err != nil {
		t.Fatal(err)
	}
	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "/bin/sh",
		Args:    []string{"-c", "echo pwned > .git/hooks/pre-commit"},
		Cwd:     root,
		Timeout: 10 * time.Second,
	})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if result.ExitCode == 0 {
		t.Fatal("writing a git hook inside the sandbox must be denied")
	}
	if data, _ := os.ReadFile(hookPath); len(data) != 0 {
		t.Fatal("hook file was written despite the protection")
	}
}

// TestSeatbeltGitCommitStillWorks guards the everyday dev workflow: the
// metadata protection targets hooks/config, so plain git commits keep
// working inside the sandbox.
func TestSeatbeltGitCommitStillWorks(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git not available")
	}
	requireSeatbelt(t)
	validator, root := newValidator(t)

	// Repo setup happens OUTSIDE the sandbox (git init creates
	// .git/hooks samples, which the protection deliberately blocks).
	setup := func(args ...string) {
		t.Helper()
		cmd := exec.Command("git", args...)
		cmd.Dir = root
		cmd.Env = append(os.Environ(),
			"GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_SYSTEM=/dev/null")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v (%s)", args, err, out)
		}
	}
	setup("init")
	setup("config", "commit.gpgsign", "false")
	// Identity comes from repo-local config, not the environment: the
	// seatbelt sandbox runs git with the runner's minimal allowlisted env
	// (GIT_AUTHOR_*/GIT_COMMITTER_* overrides are filtered out) and
	// ambient ~/.gitconfig is disabled by GIT_CONFIG_GLOBAL=/dev/null, so
	// the repo config is the only identity source that survives into the
	// sandbox. Mirrors configureGitRepo in gittools_test.go.
	setup("config", "user.name", "Loom Test")
	setup("config", "user.email", "loom@example.com")
	if err := os.WriteFile(filepath.Join(root, "a.txt"), []byte("one"), 0o644); err != nil {
		t.Fatal(err)
	}
	setup("add", "a.txt")
	setup("commit", "-m", "init")

	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	commit := func(content string) error {
		if err := os.WriteFile(filepath.Join(root, "a.txt"), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
		spec := CommandSpec{
			Program: "git",
			Cwd:     root,
			Timeout: 15 * time.Second,
			// Only allowlisted keys survive the sandbox's minimal env; the
			// author/committer identity now comes from the repo-local config
			// set during setup. Mirrors gitCommandEnv (gittools/common.go).
			Env: map[string]string{
				"LANG":                "C",
				"LC_ALL":              "C",
				"GIT_CONFIG_NOSYSTEM": "1",
				"GIT_CONFIG_GLOBAL":   "/dev/null",
				"GIT_TERMINAL_PROMPT": "0",
			},
		}
		if r, err := runner.Run(context.Background(), withArgs(spec, "add", "a.txt")); err != nil || r.ExitCode != 0 {
			return err
		}
		r, err := runner.Run(context.Background(), withArgs(spec, "commit", "-m", content))
		if err != nil {
			return err
		}
		if r.ExitCode != 0 {
			t.Fatalf("sandboxed git commit failed (exit %d): %s", r.ExitCode, r.Stderr)
		}
		return nil
	}
	if err := commit("two"); err != nil {
		t.Fatalf("sandboxed git commit error = %v", err)
	}
}

func withArgs(spec CommandSpec, args ...string) CommandSpec {
	spec.Args = args
	return spec
}

// TestWidenSandboxClones checks the widening semantics: the clone gains
// network and writable paths, the base sandbox is unchanged, and
// non-seatbelt sandboxes pass through untouched.
func TestWidenSandboxClones(t *testing.T) {
	base := SeatbeltSandbox{}
	widened := widenSandbox(base, Grant{NetworkFull: true, WritablePaths: []string{"/tmp/extra"}, GUIOpen: true})
	s, ok := widened.(SeatbeltSandbox)
	if !ok {
		t.Fatalf("widenSandbox returned %T, want SeatbeltSandbox", widened)
	}
	if !s.allowNetwork {
		t.Fatal("widened sandbox must allow network")
	}
	if !s.allowGUIOpen {
		t.Fatal("widened sandbox must allow GUI open")
	}
	if base.allowNetwork || base.allowGUIOpen {
		t.Fatal("base sandbox must be unchanged")
	}
	profile, err := s.profile(SandboxSpec{ExecutablePath: "/bin/echo", WorkingDir: "/tmp", WorkspaceRoot: "/tmp"})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(profile, "(allow network*)") {
		t.Fatalf("widened profile missing full network:\n%s", profile)
	}
	if !strings.Contains(profile, `(allow file-write* (subpath "`+workspacepkg.Canonicalize("/tmp/extra")+`"))`) {
		t.Fatalf("widened profile missing extra writable path:\n%s", profile)
	}
	for _, rule := range guiOpenAllowRules {
		if !strings.Contains(profile, rule) {
			t.Fatalf("widened profile missing GUI-open rule %q:\n%s", rule, profile)
		}
	}
	// Non-seatbelt sandboxes pass through (fail-closed preserved).
	if got := widenSandbox(DirectSandbox{}, Grant{NetworkFull: true}); got != (DirectSandbox{}) {
		t.Fatalf("widenSandbox(DirectSandbox) = %v, want unchanged", got)
	}
	if got := widenSandbox(UnsupportedSandbox{Reason: "x"}, Grant{NetworkFull: true, WritablePaths: []string{"/tmp"}}); got != (UnsupportedSandbox{Reason: "x"}) {
		t.Fatalf("widenSandbox(UnsupportedSandbox) = %v, want unchanged", got)
	}
}

// TestSeatbeltProfileDeniesGUIOpenByDefault locks the default-deny side
// of the gui_open capability (docs/BROWSER_DESIGN.md §4): the five
// GUI-open rules must appear ONLY in a widened profile — the default
// profile never carries appleevent-send.
func TestSeatbeltProfileDeniesGUIOpenByDefault(t *testing.T) {
	sandbox := SeatbeltSandbox{}
	profile, err := sandbox.profile(SandboxSpec{
		ExecutablePath: "/bin/echo",
		WorkingDir:     "/tmp",
		WorkspaceRoot:  "/tmp",
	})
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	for _, rule := range guiOpenAllowRules {
		if strings.Contains(profile, rule) {
			t.Fatalf("default profile must not contain GUI-open rule %q:\n%s", rule, profile)
		}
	}
}

// TestSeatbeltProfileAllowsScratchAndToolchainCaches locks the default
// writable scope (PERMISSION_DESIGN §6.3.2): scratch dirs and toolchain
// caches get write allows in every profile, canonicalized.
func TestSeatbeltProfileAllowsScratchAndToolchainCaches(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	sandbox := SeatbeltSandbox{}
	profile, err := sandbox.profile(SandboxSpec{
		ExecutablePath: "/bin/echo",
		WorkingDir:     "/tmp",
		WorkspaceRoot:  "/tmp",
	})
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	for _, dir := range ExtraWritableDirs() {
		rule := `(allow file-write* (subpath "` + dir + `"))`
		if !strings.Contains(profile, rule) {
			t.Errorf("profile missing write allow for %q", dir)
		}
	}
}

// TestRunWithGrantNetworkFullAllowsOutbound is the end-to-end grant proof:
// the default sandbox denies public outbound, the network grant allows it.
func TestRunWithGrantNetworkFullAllowsOutbound(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)
	// Pre-check internet availability outside the sandbox; skip instead of
	// failing on offline machines.
	if err := exec.Command("python3", "-c", "import socket; socket.create_connection(('1.1.1.1', 80), timeout=3).close()").Run(); err != nil {
		t.Skip("no outbound internet on this machine")
	}
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	connect := `import socket; socket.create_connection(('1.1.1.1', 80), timeout=5).close()`

	denied, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3", Args: []string{"-c", connect}, Cwd: root, Timeout: 15 * time.Second,
	}, Grant{})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if denied.ExitCode == 0 {
		t.Fatal("zero grant must keep public outbound denied")
	}

	allowed, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3", Args: []string{"-c", connect}, Cwd: root, Timeout: 15 * time.Second,
	}, Grant{NetworkFull: true})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if allowed.ExitCode != 0 {
		t.Fatalf("network grant must allow public outbound (exit %d): %s", allowed.ExitCode, allowed.Stderr)
	}
}

// TestRunWithGrantWritablePath proves per-call writable widening: writes
// outside the workspace fail with the zero grant and succeed with the
// path granted.
func TestRunWithGrantWritablePath(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)
	validator, root := newValidator(t)
	// The target dir must sit outside BOTH the workspace and the default
	// writable temp dir: a throwaway directory in the user's home (created
	// here, outside the sandbox).
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no home dir")
	}
	outside, err := os.MkdirTemp(home, ".loom-grant-test-")
	if err != nil {
		t.Fatalf("create outside dir: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	target := filepath.Join(outside, "out.txt")
	write := `open(` + strconvQuote(target) + `, "w").write("ok")`
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})

	denied, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3", Args: []string{"-c", write}, Cwd: root, Timeout: 15 * time.Second,
	}, Grant{})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if denied.ExitCode == 0 {
		t.Fatal("zero grant must keep writes outside the workspace denied")
	}

	allowed, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3", Args: []string{"-c", write}, Cwd: root, Timeout: 15 * time.Second,
	}, Grant{WritablePaths: []string{outside}})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if allowed.ExitCode != 0 {
		t.Fatalf("write grant must allow the granted path (exit %d): %s", allowed.ExitCode, allowed.Stderr)
	}
	if data, _ := os.ReadFile(target); string(data) != "ok" {
		t.Fatalf("target = %q, want ok", data)
	}
}

func strconvQuote(s string) string {
	return `"` + strings.ReplaceAll(s, `"`, `\"`) + `"`
}
