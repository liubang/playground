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

	"github.com/liubang/playground/go/pl/loom/internal/process/egress"
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
		proxy:         opts.Proxy,
	}
}

// SeatbeltSandbox wraps execution in macOS sandbox-exec.
type SeatbeltSandbox struct {
	allowNetwork  bool
	allowGUIOpen  bool
	writablePaths []string
	// proxy, when set, is the egress proxy wired into this sandbox: the
	// profile keeps direct outbound denied, grants the trustd exception
	// Go TLS verification needs, and Prepare appends the proxy
	// environment. The proxy URL carries the session token; only
	// sandboxed children ever see it.
	proxy *egress.ProxyEnv
}

// Isolation reports the active seatbelt isolation mode: seatbelt+proxy
// when the egress proxy is wired in, so audit and UI can tell the two
// network postures apart.
func (s SeatbeltSandbox) Isolation() Isolation {
	if s.proxy != nil {
		return SeatbeltProxyIsolation
	}
	return SeatbeltIsolation
}

// widenSandbox clones the seatbelt sandbox with additional capabilities
// (docs/PERMISSION_DESIGN.md §3.2). Other sandbox types are returned
// unchanged: widening never manufactures isolation that does not exist.
func widenSandbox(base Sandbox, grant Grant) Sandbox {
	s, ok := base.(SeatbeltSandbox)
	if !ok {
		return base
	}
	// The proxy field rides the copy untouched (a dropped proxy would
	// silently produce a sandbox with neither proxy env nor network).
	// NetworkFull is a no-op under the proxy: the proxy IS the network
	// answer, and a full allow would route around its policy and logs.
	return SeatbeltSandbox{
		allowNetwork:  s.allowNetwork || (grant.NetworkFull && s.proxy == nil),
		allowGUIOpen:  s.allowGUIOpen || grant.GUIOpen,
		writablePaths: uniqueCleanPaths(append(append([]string(nil), s.writablePaths...), grant.WritablePaths...)),
		proxy:         s.proxy,
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
	env := append([]string(nil), spec.Env...)
	if s.proxy != nil {
		// Appended last: exec dedupes environment keys last-wins, and the
		// model's own env never carries these keys (they are outside the
		// runner allowlist), so the proxy assignment always stands.
		env = append(env, s.proxy.EnvVars()...)
	}
	return SandboxLaunch{
		Program: sandboxExecPath,
		Args:    args,
		Env:     env,
		Cleanup: func() error { return os.Remove(profileFile.Name()) },
	}, nil
}

// sandboxLiteralRule renders seatbelt literal filters for a list of
// exact paths, e.g. (literal "/dev/null").
func sandboxLiteralRule(paths []string) string {
	parts := make([]string, 0, len(paths))
	for _, p := range paths {
		parts = append(parts, "(literal "+seatbeltQuote(p)+")")
	}
	return strings.Join(parts, " ")
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
		// for no security gain. The writable literals come from
		// SandboxWritableLiterals — the single source the permission
		// derivation also consults.
		"(allow file-read* file-write* "+sandboxLiteralRule(SandboxWritableLiterals)+")",
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
	if s.proxy != nil {
		// trustd performs TLS trust evaluation for anything linked against
		// the Security framework — including every Go binary verifying a
		// certificate through the CONNECT tunnel. Without this lookup,
		// sandboxed Go programs cannot complete any TLS handshake. The
		// trade (trustd is a potential, if awkward, exfiltration channel)
		// is a deliberate, accepted exception.
		lines = append(lines, `(allow mach-lookup (global-name "com.apple.trustd.agent"))`)
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
