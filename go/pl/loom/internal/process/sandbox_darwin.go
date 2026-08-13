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
	"strings"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
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
	allowGUIOpen  bool
	writablePaths []string
}

// Isolation reports the active seatbelt isolation mode.
func (s SeatbeltSandbox) Isolation() Isolation { return SeatbeltIsolation }

// widenSandbox clones the seatbelt sandbox with additional capabilities
// (docs/PERMISSION_DESIGN.md §3.2). Other sandbox types are returned
// unchanged: widening never manufactures isolation that does not exist.
func widenSandbox(base Sandbox, grant Grant) Sandbox {
	s, ok := base.(SeatbeltSandbox)
	if !ok {
		return base
	}
	return SeatbeltSandbox{
		allowNetwork:  s.allowNetwork || grant.NetworkFull,
		allowGUIOpen:  s.allowGUIOpen || grant.GUIOpen,
		writablePaths: uniqueCleanPaths(append(append([]string(nil), s.writablePaths...), grant.WritablePaths...)),
	}
}

// guiOpenAllowRules are the minimal seatbelt rules that let a sandboxed
// command drive macOS GUI applications via `open`
// (docs/BROWSER_DESIGN.md §4.1, verified by controlled experiment on
// 2026-08-10): LaunchServices binding resolution plus Apple Event
// delivery. They are appended ONLY for calls granted the gui_open
// capability — appleevent-send lets the process message ANY running
// application, so the default profile must never include them. The
// global-names are private interfaces and may shift across macOS
// releases; runner_seatbelt_test.go carries a live probe that fails
// loudly when they do.
var guiOpenAllowRules = []string{
	`(allow mach-lookup (global-name "com.apple.coreservices.launchservicesd"))`,
	`(allow mach-lookup (global-name "com.apple.lsd.mapdb"))`,
	`(allow mach-lookup (global-name "com.apple.lsd.modifydb"))`,
	`(allow mach-lookup (global-name "com.apple.coreservices.appleevents"))`,
	`(allow appleevent-send)`,
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

	writePaths := []string{workspacepkg.Canonicalize(spec.WorkspaceRoot)}
	for _, path := range spec.WritablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, workspacepkg.Canonicalize(path))
	}
	for _, path := range s.writablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, workspacepkg.Canonicalize(path))
	}
	// Scratch dirs ($TMPDIR, /tmp) and regenerable toolchain caches are
	// writable by every sandboxed command — the single source is
	// ExtraWritableDirs, shared with the file-tool path validator.
	writePaths = append(writePaths, ExtraWritableDirs()...)
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
		// Countless tools redirect output to /dev/null (git included) and
		// read randomness at startup; denying them breaks everyday commands
		// for no security gain.
		"(allow file-read* file-write* (literal \"/dev/null\"))",
		"(allow file-read* (literal \"/dev/zero\") (literal \"/dev/random\") (literal \"/dev/urandom\"))",
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
	if s.allowGUIOpen {
		lines = append(lines, guiOpenAllowRules...)
	}
	// Destructive-write denies come AFTER every allow (seatbelt's last
	// match wins): sensitive paths stay un-renameable even when a widened
	// write root covers them (rename-then-read would bypass the read
	// deny), and protected workspace metadata stays read-only even when
	// the workspace sits inside another writable root (e.g. TMPDIR).
	for _, rule := range sensitiveUnlinkDenies() {
		lines = append(lines, rule)
	}
	workspace := workspacepkg.Canonicalize(spec.WorkspaceRoot)
	for _, rel := range protectedWorkspaceSubpaths {
		protected := seatbeltQuote(filepath.Join(workspace, rel))
		lines = append(
			lines,
			fmt.Sprintf("(deny file-write* (literal %s))", protected),
			fmt.Sprintf("(deny file-write* (subpath %s))", protected),
		)
	}
	return strings.Join(lines, "\n") + "\n", nil
}

// The sensitive locations denied below come from the workspace package —
// the single source of truth shared with the builtin file tools
// (workspace/sensitive.go). Reads are denied up front
// (sensitiveReadDenies); destructive writes are denied AFTER the write
// allows (sensitiveUnlinkDenies) so widened write roots cannot enable a
// rename-then-read bypass.

// sensitiveReadDenies returns seatbelt rules denying reads of
// credential-like locations under the user's home directory. Writes
// remain scoped to the workspace, and the workspace PathValidator
// independently rejects these components inside the workspace, so the
// sandbox only needs to cover the home-level secrets the broad read
// policy would otherwise expose.
func sensitiveReadDenies() []string {
	home := workspacepkg.SensitiveHome()
	if home == "" {
		return nil
	}
	subpaths := workspacepkg.SensitiveHomeSubpaths()
	literals := workspacepkg.SensitiveHomeLiterals()
	rules := make([]string, 0, len(subpaths)+len(literals))
	for _, rel := range subpaths {
		rules = append(rules, fmt.Sprintf("(deny file-read* (subpath %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	for _, rel := range literals {
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
	home := workspacepkg.SensitiveHome()
	if home == "" {
		return nil
	}
	subpaths := workspacepkg.SensitiveHomeSubpaths()
	literals := workspacepkg.SensitiveHomeLiterals()
	rules := make([]string, 0, len(subpaths)+len(literals))
	for _, rel := range subpaths {
		rules = append(rules, fmt.Sprintf("(deny file-write-unlink (subpath %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	for _, rel := range literals {
		rules = append(rules, fmt.Sprintf("(deny file-write-unlink (literal %s))", seatbeltQuote(filepath.Join(home, rel))))
	}
	return rules
}

func seatbeltQuote(path string) string {
	replacer := strings.NewReplacer(`\\`, `\\\\`, `"`, `\\"`)
	return `"` + replacer.Replace(path) + `"`
}
