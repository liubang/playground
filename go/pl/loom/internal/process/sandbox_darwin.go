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

//go:build darwin

package process

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const sandboxExecPath = "/usr/bin/sandbox-exec"

// NewPlatformSandbox returns a seatbelt sandbox when sandbox-exec is available.
func NewPlatformSandbox(opts PlatformSandboxOptions) Sandbox {
	info, err := os.Stat(sandboxExecPath)
	if err != nil || info.IsDir() {
		return UnsupportedSandbox{Reason: sandboxExecPath + " is unavailable"}
	}
	return SeatbeltSandbox{
		allowNetwork:  opts.AllowNetwork,
		writablePaths: append([]string(nil), opts.WritablePaths...),
	}
}

// SeatbeltSandbox wraps execution in macOS sandbox-exec.
type SeatbeltSandbox struct {
	allowNetwork  bool
	writablePaths []string
}

// Isolation reports the active seatbelt isolation mode.
func (s SeatbeltSandbox) Isolation() Isolation { return SeatbeltIsolation }

// widenSandbox clones the seatbelt sandbox with additional capabilities
// (docs/PERMISSION_DESIGN.md §3.2). Other sandbox types are returned
// unchanged: widening never manufactures isolation that does not exist.
func widenSandbox(base Sandbox, networkFull bool, extraWritable []string) Sandbox {
	s, ok := base.(SeatbeltSandbox)
	if !ok {
		return base
	}
	return SeatbeltSandbox{
		allowNetwork:  s.allowNetwork || networkFull,
		writablePaths: uniqueCleanPaths(append(append([]string(nil), s.writablePaths...), extraWritable...)),
	}
}

// protectedWorkspaceSubpaths are metadata locations under the writable
// workspace root that stay read-only: modifying them lets repository
// content escalate the agent beyond its sandbox (git hooks, hooksPath
// redirection, loom rule injection). Both the literal and the subpath
// forms are excluded so first-time creation is blocked as well, mirroring
// codex's WritableRoot protected-metadata handling.
var protectedWorkspaceSubpaths = []string{".git/hooks", ".git/config", ".loom"}

// Prepare creates a temporary seatbelt profile and wraps the child process.
func (s SeatbeltSandbox) Prepare(spec SandboxSpec) (SandboxLaunch, error) {
	profile, err := s.profile(spec)
	if err != nil {
		return SandboxLaunch{}, fmt.Errorf("%w: %v", ErrSandboxUnavailable, err)
	}
	profileFile, err := os.CreateTemp("", "loom-seatbelt-*.sb")
	if err != nil {
		return SandboxLaunch{}, fmt.Errorf("%w: create seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	if _, err := profileFile.WriteString(profile); err != nil {
		_ = profileFile.Close()
		_ = os.Remove(profileFile.Name())
		return SandboxLaunch{}, fmt.Errorf("%w: write seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	if err := profileFile.Close(); err != nil {
		_ = os.Remove(profileFile.Name())
		return SandboxLaunch{}, fmt.Errorf("%w: close seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	args := []string{"-f", profileFile.Name(), spec.ExecutablePath}
	args = append(args, spec.Args...)
	return SandboxLaunch{
		Program: sandboxExecPath,
		Args:    args,
		Env:     append([]string(nil), spec.Env...),
		Cleanup: func() error { return os.Remove(profileFile.Name()) },
	}, nil
}

func (s SeatbeltSandbox) profile(spec SandboxSpec) (string, error) {
	if strings.TrimSpace(spec.ExecutablePath) == "" {
		return "", fmt.Errorf("executable path is required")
	}
	if !filepath.IsAbs(spec.ExecutablePath) || !filepath.IsAbs(spec.WorkingDir) || !filepath.IsAbs(spec.WorkspaceRoot) {
		return "", fmt.Errorf("seatbelt requires absolute paths")
	}

	writePaths := []string{canonicalWritePath(spec.WorkspaceRoot)}
	for _, path := range spec.WritablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, canonicalWritePath(path))
	}
	for _, path := range s.writablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, canonicalWritePath(path))
	}
	writePaths = append(writePaths, canonicalWritePath(os.TempDir()))
	writePaths = uniqueCleanPaths(writePaths)

	var lines []string
	lines = append(
		lines,
		"(version 1)",
		"(deny default)",
		"(allow process-exec)",
		"(allow process-fork)",
		"(allow signal (target self))",
		"(allow sysctl-read)",
		// Modern runtimes (Go, Rust) cannot start under a path-scoped read
		// policy: dyld loads metadata/xattrs across the system and stack-guard
		// allocation fails under restrictive subpath rules (verified against
		// ripgrep and the Go toolchain). Reads are therefore allowed broadly,
		// while credential-like locations stay explicitly denied below.
		"(allow file-read*)",
		// Loopback networking is allowed in both directions so dev servers
		// can bind and be probed locally (verified against sandbox-exec:
		// bind/inbound filter on the local endpoint, outbound on the remote).
		// Public egress and DNS resolution stay denied by the default-deny.
		"(allow network-bind (local ip \"localhost:*\"))",
		"(allow network-inbound (local ip \"localhost:*\"))",
		"(allow network-outbound (remote ip \"localhost:*\"))",
	)
	for _, rule := range sensitiveReadDenies() {
		lines = append(lines, rule)
	}
	for _, path := range writePaths {
		lines = append(lines, fmt.Sprintf("(allow file-write* (subpath %s))", seatbeltQuote(path)))
	}
	if s.allowNetwork {
		lines = append(lines, "(allow network*)")
	}
	// Destructive-write denies come AFTER every allow (seatbelt's last
	// match wins): sensitive paths stay un-renameable even when a widened
	// write root covers them (rename-then-read would bypass the read
	// deny), and protected workspace metadata stays read-only even when
	// the workspace sits inside another writable root (e.g. TMPDIR).
	for _, rule := range sensitiveUnlinkDenies() {
		lines = append(lines, rule)
	}
	workspace := canonicalWritePath(spec.WorkspaceRoot)
	for _, rel := range protectedWorkspaceSubpaths {
		protected := seatbeltQuote(filepath.Join(workspace, rel))
		lines = append(lines,
			fmt.Sprintf("(deny file-write* (literal %s))", protected),
			fmt.Sprintf("(deny file-write* (subpath %s))", protected),
		)
	}
	return strings.Join(lines, "\n") + "\n", nil
}

// sensitiveSubpaths and sensitiveLiterals are the credential-like
// locations under the user's home directory. Reads are denied up front
// (sensitiveReadDenies); destructive writes are denied AFTER the write
// allows (sensitiveUnlinkDenies) so widened write roots cannot enable a
// rename-then-read bypass.
var sensitiveSubpaths = []string{
	".ssh",
	".gnupg",
	".aws",
	".azure",
	".kube",
	".docker",
	".config/gcloud",
	".config/gh",
	".config/snowflake",
	"Library/Keychains",
}

var sensitiveLiterals = []string{
	".netrc",
	".git-credentials",
	".env",
	"credentials.json",
	"service-account.json",
	".npmrc",
	".pypirc",
}

// sensitiveHome returns the canonicalized home directory for sensitive
// path rules. Seatbelt matches canonical paths, so a symlinked HOME
// (common in tests and some managed setups) must be resolved or the
// denies silently match nothing.
func sensitiveHome() string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return ""
	}
	return canonicalWritePath(home)
}

// sensitiveReadDenies returns seatbelt rules denying reads of
// credential-like locations under the user's home directory. Writes
// remain scoped to the workspace, and the workspace PathValidator
// independently rejects these components inside the workspace, so the
// sandbox only needs to cover the home-level secrets the broad read
// policy would otherwise expose.
func sensitiveReadDenies() []string {
	home := sensitiveHome()
	if home == "" {
		return nil
	}
	rules := make([]string, 0, len(sensitiveSubpaths)+len(sensitiveLiterals))
	for _, rel := range sensitiveSubpaths {
		rules = append(rules, fmt.Sprintf("(deny file-read* (subpath %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	for _, rel := range sensitiveLiterals {
		rules = append(rules, fmt.Sprintf("(deny file-read* (literal %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	return rules
}

// sensitiveUnlinkDenies returns destructive-write denies for the same
// locations. They MUST be emitted after all write allows: emitted earlier
// they are dead rules whenever a widened write root (e.g. grant.write
// ["~"] or a home-rooted workspace) covers the sensitive path — the
// trailing allow would win and rename-then-read would bypass the read
// deny.
func sensitiveUnlinkDenies() []string {
	home := sensitiveHome()
	if home == "" {
		return nil
	}
	rules := make([]string, 0, len(sensitiveSubpaths)+len(sensitiveLiterals))
	for _, rel := range sensitiveSubpaths {
		rules = append(rules, fmt.Sprintf("(deny file-write-unlink (subpath %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	for _, rel := range sensitiveLiterals {
		rules = append(rules, fmt.Sprintf("(deny file-write-unlink (literal %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	return rules
}

// canonicalWritePath resolves symlinks in a writable path because
// seatbelt matches CANONICAL paths: on macOS /var and /tmp are symlinks
// into /private, so the /var/folders/... form of TMPDIR never matches a
// (subpath ...) rule — writes silently failed. Paths that do not exist
// yet resolve through their nearest existing ancestor.
func canonicalWritePath(path string) string {
	clean := filepath.Clean(path)
	if resolved, err := filepath.EvalSymlinks(clean); err == nil {
		return resolved
	}
	dir := clean
	var tail []string
	for {
		parent := filepath.Dir(dir)
		if parent == dir {
			return clean
		}
		tail = append([]string{filepath.Base(dir)}, tail...)
		if resolved, err := filepath.EvalSymlinks(parent); err == nil {
			for _, elem := range tail {
				resolved = filepath.Join(resolved, elem)
			}
			return resolved
		}
		dir = parent
	}
}

func uniqueCleanPaths(paths []string) []string {
	seen := map[string]struct{}{}
	result := make([]string, 0, len(paths))
	for _, path := range paths {
		path = filepath.Clean(strings.TrimSpace(path))
		if path == "." || path == "" || !filepath.IsAbs(path) {
			continue
		}
		if _, ok := seen[path]; ok {
			continue
		}
		seen[path] = struct{}{}
		result = append(result, path)
	}
	sort.Strings(result)
	return result
}

func seatbeltQuote(path string) string {
	replacer := strings.NewReplacer(`\\`, `\\\\`, `"`, `\\"`)
	return `"` + replacer.Replace(path) + `"`
}
