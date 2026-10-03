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
// Created: 2026/10/03

package app

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// egressLogCapture is a slog.Handler that keeps every "egress
// connection" record's attributes for assertions.
type egressLogCapture struct {
	mu   sync.Mutex
	recs []map[string]string
}

func (h *egressLogCapture) Enabled(context.Context, slog.Level) bool { return true }

func (h *egressLogCapture) Handle(_ context.Context, r slog.Record) error {
	if r.Message != "egress connection" {
		return nil
	}
	attrs := map[string]string{}
	r.Attrs(func(a slog.Attr) bool {
		attrs[a.Key] = a.Value.String()
		return true
	})
	h.mu.Lock()
	h.recs = append(h.recs, attrs)
	h.mu.Unlock()
	return nil
}

func (h *egressLogCapture) WithAttrs([]slog.Attr) slog.Handler { return h }
func (h *egressLogCapture) WithGroup(string) slog.Handler      { return h }

// waitFor polls until at least n egress records arrived or the deadline
// passes (the plain-HTTP leg logs after the response is written).
func (h *egressLogCapture) waitFor(n int) []map[string]string {
	deadline := time.Now().Add(3 * time.Second)
	for {
		h.mu.Lock()
		recs := append([]map[string]string(nil), h.recs...)
		h.mu.Unlock()
		if len(recs) >= n || time.Now().After(deadline) {
			return recs
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// findRecord returns the first record with the given host, or nil.
func findRecord(recs []map[string]string, host string) map[string]string {
	for _, r := range recs {
		if r["host"] == host {
			return r
		}
	}
	return nil
}

// TestBootstrapEgressProxyEndToEnd is the assembly-level end-to-end
// proof for sandbox.network=proxy:
// a full NewProcessRuntime + NewWorkspaceBootstrap assembly, then REAL
// sandboxed curl commands through the runner — the proxy env injection,
// the PackageSet policy bridge, the resolved-address guard, and the
// connection log are all exercised through the production wiring, with
// a loopback upstream so no external network is needed.
func TestBootstrapEgressProxyEndToEnd(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("seatbelt sandbox is macOS-only")
	}
	if _, err := exec.LookPath("curl"); err != nil {
		t.Skip("curl unavailable")
	}
	// Nested sandboxing is denied inside `bazel test`'s own darwin
	// sandbox (same probe as process.requireSeatbelt); run the test
	// binary directly — `bazel run //go/pl/loom/internal/app:app_test --
	// -test.run TestBootstrapEgressProxyEndToEnd` — to execute it for real.
	if _, err := os.Stat("/usr/bin/sandbox-exec"); err != nil {
		t.Skip("sandbox-exec unavailable")
	}
	probe := exec.Command("/usr/bin/sandbox-exec", "-p", "(version 1) (allow default)", "/usr/bin/true")
	if out, err := probe.CombinedOutput(); err != nil {
		t.Skipf("seatbelt cannot be applied here: %v (%s)", err, strings.TrimSpace(string(out)))
	}
	ctx := context.Background()

	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("hello"))
	}))
	t.Cleanup(upstream.Close)
	var port int
	if _, err := fmt.Sscanf(strings.TrimPrefix(upstream.URL, "http://127.0.0.1:"), "%d", &port); err != nil {
		t.Fatalf("parse upstream port: %v", err)
	}

	capture := &egressLogCapture{}
	resolved := testResolvedConfig(fakes.NewFakeModel())
	resolved.Storage = config.ResolvedStorage{BaseDir: t.TempDir()}
	// The default posture (unmatched=allow): the allow-listed loopback
	// literal reaches the local upstream, an explicit deny rule answers
	// denied.example, and the guard stops the metadata literal — all
	// without touching the external network.
	resolved.Sandbox = config.ResolvedSandbox{
		Network:             config.SandboxNetworkProxy,
		ProxyUnmatchedAllow: true,
	}
	if err := os.MkdirAll(resolved.Storage.SessionsDir(), 0o755); err != nil {
		t.Fatalf("mkdir sessions dir: %v", err)
	}
	proc, err := NewProcessRuntime(ctx, resolved, ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(t.TempDir(), "artifacts"),
		Logger:      slog.New(capture),
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	t.Cleanup(proc.Close)

	workspaceRoot := t.TempDir()
	b, err := NewWorkspaceBootstrap(ctx, proc, BootstrapConfig{WorkspaceRoot: workspaceRoot})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	t.Cleanup(b.Close)

	// Rules are added AFTER bootstrap: AttachPackages reloads the
	// declarative layers during bootstrap and would replace anything
	// added earlier. The egress policy reads the live PackageSet per
	// connection, so post-bootstrap rules take effect immediately.
	proc.Packages.Add(
		permission.Package{
			Bind:          permission.Binding{Kind: permission.BindHost, Host: "127.0.0.1"},
			Decision:      domain.DecisionAllow,
			Scope:         permission.ScopeUser,
			Justification: "local dev upstream",
		},
		permission.Package{
			Bind:          permission.Binding{Kind: permission.BindHost, Host: "denied.example"},
			Decision:      domain.DecisionDeny,
			Scope:         permission.ScopeBuiltin,
			Justification: "request capture service",
		},
	)

	run := func(args ...string) process.Result {
		t.Helper()
		res, err := b.Runner.Run(ctx, process.CommandSpec{
			Program: args[0],
			Args:    args[1:],
			Cwd:     workspaceRoot,
			Timeout: 30 * time.Second,
		})
		if err != nil {
			t.Fatalf("runner error for %v: %v", args, err)
		}
		return res
	}

	// 1. The proxy environment is injected into sandboxed commands.
	envRes := run("sh", "-c", "echo \"$HTTP_PROXY|$NO_PROXY\"")
	out := strings.TrimSpace(string(envRes.Stdout))
	if !strings.Contains(out, "http://loom:") || !strings.Contains(out, "@127.0.0.1:") {
		t.Fatalf("HTTP_PROXY = %q, want the injected proxy URL with token (exit %d, stderr %q, isolation %s)",
			out, envRes.ExitCode, envRes.Stderr, envRes.Isolation)
	}
	if !strings.Contains(out, "localhost") {
		t.Fatalf("NO_PROXY = %q, want localhost entry", out)
	}

	// 2. The allow-listed loopback literal is reachable THROUGH the proxy
	// (--noproxy '' overrides the NO_PROXY bypass so curl rides the
	// proxy; the literal's explicit allow rule exempts it at the guard).
	allowRes := run("curl", "-sS", "-m", "10", "--noproxy", "", fmt.Sprintf("http://127.0.0.1:%d/", port))
	if strings.TrimSpace(string(allowRes.Stdout)) != "hello" {
		t.Fatalf("proxied fetch stdout = %q, want hello (stderr: %s)", allowRes.Stdout, allowRes.Stderr)
	}

	// 3. An explicit deny rule answers with the policy's 403 body.
	denyRes := run("curl", "-sS", "-m", "10", "--noproxy", "", "http://denied.example/")
	if body := string(denyRes.Stdout); !strings.Contains(body, "blocked by loom egress policy: request capture service") {
		t.Fatalf("denied fetch body = %q, want the policy 403 (stderr: %s)", body, denyRes.Stderr)
	}

	// 4. The unmatched-allow posture still cannot reach the metadata
	// literal: the guard's denied classes apply to IP literals.
	guardRes := run("curl", "-sS", "-m", "10", "--noproxy", "", "http://169.254.169.254/")
	if body := string(guardRes.Stdout); !strings.Contains(body, "blocked: resolved to a link-local address") {
		t.Fatalf("metadata fetch body = %q, want the guard denial (stderr: %s)", body, guardRes.Stderr)
	}

	// 5. Bypassing the proxy env stays fail-closed: seatbelt denies the
	// direct non-loopback dial (no external reachability needed — the
	// connection never leaves the sandbox).
	directRes := run("curl", "-sS", "-m", "5", "--noproxy", "*", "http://93.184.216.34/")
	if directRes.ExitCode == 0 {
		t.Fatalf("direct fetch unexpectedly succeeded: stdout = %q", directRes.Stdout)
	}

	// 6. Every attempt is logged with its decision through the SAME
	// logger the process runtime was built with.
	recs := capture.waitFor(3)
	if rec := findRecord(recs, "127.0.0.1"); rec == nil || rec["allow"] != "true" || rec["rule"] != "user" || rec["dialed"] != "127.0.0.1" {
		t.Fatalf("allow record = %v (all: %v)", rec, recs)
	}
	if rec := findRecord(recs, "denied.example"); rec == nil || rec["allow"] != "false" || rec["rule"] != "builtin" {
		t.Fatalf("deny record = %v (all: %v)", rec, recs)
	}
	if rec := findRecord(recs, "169.254.169.254"); rec == nil || rec["allow"] != "true" || rec["rule"] != "unmatched-allow" || rec["error"] == "" {
		t.Fatalf("guard record = %v (all: %v)", rec, recs)
	}
}
