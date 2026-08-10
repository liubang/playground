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
// Created: 2026/07/26

// SeatbeltSandbox and sandboxExecPath only exist on darwin, so these tests
// must be platform-gated — otherwise the package fails to compile on Linux
// CI (undefined symbols), not merely skip at runtime.
//
//go:build darwin

package process

import (
	"context"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestSeatbeltProfileAllowsLoopbackOnly(t *testing.T) {
	sandbox := SeatbeltSandbox{}
	profile, err := sandbox.profile(SandboxSpec{
		ExecutablePath: "/bin/echo",
		WorkingDir:     "/tmp",
		WorkspaceRoot:  "/tmp",
	})
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	for _, rule := range []string{
		`(allow network-bind (local ip "localhost:*"))`,
		`(allow network-inbound (local ip "localhost:*"))`,
		`(allow network-outbound (remote ip "localhost:*"))`,
	} {
		if !strings.Contains(profile, rule) {
			t.Fatalf("profile missing loopback rule %q:\n%s", rule, profile)
		}
	}
	if strings.Contains(profile, "(allow network*)") {
		t.Fatalf("profile must not allow full network by default:\n%s", profile)
	}
}

// requireSeatbelt skips the test when a seatbelt profile cannot actually be
// applied here (e.g. nested inside another sandbox such as `bazel test`,
// where sandbox_apply is denied with "Operation not permitted").
func requireSeatbelt(t *testing.T) {
	t.Helper()
	if _, err := os.Stat(sandboxExecPath); err != nil {
		t.Skip("sandbox-exec not available")
	}
	cmd := exec.Command(sandboxExecPath, "-p", "(version 1) (allow default)", "/usr/bin/true")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Skipf("seatbelt cannot be applied here: %v (%s)", err, strings.TrimSpace(string(out)))
	}
}

func TestSeatbeltLoopbackRoundtrip(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "python3",
		Args: []string{
			"-c",
			"import socket, threading; srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(('127.0.0.1', 0)); srv.listen(1); port = srv.getsockname()[1]; accept = lambda: (lambda c: (c.recv(16), c.sendall(b'pong'), c.close()))(srv.accept()[0]); threading.Thread(target=accept, daemon=True).start(); cli = socket.create_connection(('127.0.0.1', port), timeout=5); cli.sendall(b'ping'); print(cli.recv(16).decode())",
		},
		Cwd:     root,
		Timeout: 15 * time.Second,
	})
	if err != nil {
		t.Fatalf("sandboxed loopback run error = %v (stderr: %s)", err, result.Stderr)
	}
	if strings.TrimSpace(string(result.Stdout)) != "pong" {
		t.Fatalf("loopback roundtrip stdout = %q, want pong (stderr: %s)", result.Stdout, result.Stderr)
	}
}

func TestSeatbeltPublicOutboundStillDenied(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "python3",
		Args:    []string{"-c", "import socket; socket.create_connection(('1.1.1.1', 80), timeout=5)"},
		Cwd:     root,
		Timeout: 15 * time.Second,
	})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if result.ExitCode == 0 {
		t.Fatal("public outbound must stay denied by the default-deny profile")
	}
}

// TestSeatbeltGUIOpenGrant is the live probe for the gui_open capability
// (docs/BROWSER_DESIGN.md §4): the default profile must make `open` fail
// (LaunchServices/Apple Events denied), and the GUIOpen grant must make
// it succeed. It fails loudly when a macOS release renames the private
// mach global-names the rules rely on. Note: the granted half opens one
// background browser tab on the machine running the test.
func TestSeatbeltGUIOpenGrant(t *testing.T) {
	requireSeatbelt(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: SeatbeltSandbox{}})
	spec := CommandSpec{
		Program: "open",
		Args:    []string{"-g", "https://example.com"},
		Cwd:     root,
		Timeout: 15 * time.Second,
	}

	denied, err := runner.RunWithGrant(context.Background(), spec, Grant{})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if denied.ExitCode == 0 {
		t.Fatal("open must fail under the default-deny profile")
	}

	allowed, err := runner.RunWithGrant(context.Background(), spec, Grant{GUIOpen: true})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if allowed.ExitCode != 0 {
		t.Fatalf("gui_open grant must let open succeed (exit %d): %s", allowed.ExitCode, allowed.Stderr)
	}
}
