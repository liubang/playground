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

// SeatbeltSandbox only exists on darwin; these tests are platform-gated
// for the same reason as runner_seatbelt_test.go.
//
//go:build darwin

package process

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/process/egress"
)

func proxyEnvForTest() *egress.ProxyEnv {
	return &egress.ProxyEnv{URL: "http://loom:deadbeef@127.0.0.1:18080"}
}

func TestSeatbeltProxyProfileAddsTrustd(t *testing.T) {
	spec := SandboxSpec{ExecutablePath: "/bin/echo", WorkingDir: "/tmp", WorkspaceRoot: "/tmp"}

	plain, err := SeatbeltSandbox{}.profile(spec)
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	if strings.Contains(plain, "trustd") {
		t.Fatalf("default profile must not grant trustd:\n%s", plain)
	}

	proxied, err := SeatbeltSandbox{proxy: proxyEnvForTest()}.profile(spec)
	if err != nil {
		t.Fatalf("profile() error = %v", err)
	}
	if !strings.Contains(proxied, `(allow mach-lookup (global-name "com.apple.trustd.agent"))`) {
		t.Fatalf("proxy profile missing trustd rule:\n%s", proxied)
	}
	// The proxy posture must NOT widen the network rules: direct outbound
	// stays default-denied and the loopback allowances (which cover the
	// proxy port) are unchanged.
	if strings.Contains(proxied, "(allow network*)") {
		t.Fatalf("proxy profile must not allow full network:\n%s", proxied)
	}
}

func TestSeatbeltProxyIsolationLabel(t *testing.T) {
	if got := (SeatbeltSandbox{}).Isolation(); got != SeatbeltIsolation {
		t.Fatalf("plain isolation = %v, want seatbelt", got.Name())
	}
	if got := (SeatbeltSandbox{proxy: proxyEnvForTest()}).Isolation(); got != SeatbeltProxyIsolation {
		t.Fatalf("proxy isolation = %v, want seatbelt+proxy", got.Name())
	}
	if SeatbeltProxyIsolation.Name() != "seatbelt+proxy" {
		t.Fatalf("isolation name = %q", SeatbeltProxyIsolation.Name())
	}
}

// TestWidenSandboxKeepsProxy is the regression guard for the grant
// path: a NetworkFull grant under the proxy must be a no-op (the proxy
// is the network answer), the proxy must survive the clone, and the
// isolation label must stay seatbelt+proxy.
func TestWidenSandboxKeepsProxy(t *testing.T) {
	widened, ok := widenSandbox(SeatbeltSandbox{proxy: proxyEnvForTest()}, Grant{NetworkFull: true}).(SeatbeltSandbox)
	if !ok {
		t.Fatal("widenSandbox changed the sandbox type")
	}
	if widened.proxy == nil {
		t.Fatal("widenSandbox dropped the proxy field")
	}
	if widened.allowNetwork {
		t.Fatal("NetworkFull grant must be a no-op under the proxy")
	}
	if widened.Isolation() != SeatbeltProxyIsolation {
		t.Fatalf("widened isolation = %v, want seatbelt+proxy", widened.Isolation().Name())
	}

	// Without the proxy the same grant still widens (unchanged legacy).
	plain := widenSandbox(SeatbeltSandbox{}, Grant{NetworkFull: true}).(SeatbeltSandbox)
	if !plain.allowNetwork {
		t.Fatal("NetworkFull grant must still widen the plain sandbox")
	}
}

// TestPrepareAppendsProxyEnv: the proxy environment lands AFTER the
// caller's env, so a smuggled duplicate key cannot win (exec dedupes
// last-wins).
func TestPrepareAppendsProxyEnv(t *testing.T) {
	sandbox := SeatbeltSandbox{proxy: proxyEnvForTest()}
	launch, err := sandbox.Prepare(SandboxSpec{
		ExecutablePath: "/bin/echo",
		Args:           []string{"hi"},
		WorkingDir:     "/tmp",
		WorkspaceRoot:  "/tmp",
		Env:            []string{"PATH=/usr/bin", "HTTP_PROXY=http://evil:1"},
	})
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	t.Cleanup(func() { _ = launch.Cleanup() })
	lastHTTPProxy := -1
	for i, kv := range launch.Env {
		if strings.HasPrefix(kv, "HTTP_PROXY=") {
			lastHTTPProxy = i
		}
	}
	if lastHTTPProxy < 0 || launch.Env[lastHTTPProxy] != "HTTP_PROXY="+proxyEnvForTest().URL {
		t.Fatalf("proxy env not appended last: %v", launch.Env)
	}
	for _, key := range []string{"HTTPS_PROXY", "ALL_PROXY", "GRPC_PROXY", "NO_PROXY"} {
		found := false
		for _, kv := range launch.Env {
			if strings.HasPrefix(kv, key+"=") {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("proxy env missing %s: %v", key, launch.Env)
		}
	}
}

// proxyTestPolicy allows exactly "allowed.test" (exempting the loopback
// test resolution) and denies everything else.
type proxyTestPolicy struct{}

func (proxyTestPolicy) Decide(host string, _ int) egress.Decision {
	if host == "allowed.test" {
		return egress.Decision{
			Allow: true, Matched: true, Reason: "allowed", Rule: "builtin",
			ExemptLiterals: []netip.Addr{netip.MustParseAddr("127.0.0.1")},
		}
	}
	return egress.Decision{Allow: false, Matched: true, Reason: "denied by test policy", Rule: "builtin"}
}

// startProxyForSeatbeltProbe runs a real egress.Server whose resolver
// maps every name to loopback (hermetic), pointed at the test upstream.
func startProxyForSeatbeltProbe(t *testing.T) *egress.Server {
	t.Helper()
	server, err := egress.NewServer(egress.Config{
		Policy: proxyTestPolicy{},
		LookupIP: func(context.Context, string) ([]netip.Addr, error) {
			return []netip.Addr{netip.MustParseAddr("127.0.0.1")}, nil
		},
		LocalAddrs: func() ([]netip.Addr, error) { return nil, nil },
	})
	if err != nil {
		t.Fatalf("egress.NewServer: %v", err)
	}
	t.Cleanup(func() { _ = server.Close() })
	return server
}

// TestSeatbeltProxyLiveProbe is the end-to-end proof on a real
// sandbox-exec: a sandboxed python3 reaches an allow-listed host
// through the injected proxy environment, gets 403 from the policy for
// anything else, and cannot resolve or dial directly.
func TestSeatbeltProxyLiveProbe(t *testing.T) {
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	requireSeatbelt(t)

	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("hello"))
	}))
	t.Cleanup(upstream.Close)
	var port int
	if _, err := fmt.Sscanf(strings.TrimPrefix(upstream.URL, "http://127.0.0.1:"), "%d", &port); err != nil {
		t.Fatalf("parse upstream port: %v", err)
	}
	proxy := startProxyForSeatbeltProbe(t)

	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox: SeatbeltSandbox{proxy: &egress.ProxyEnv{URL: proxy.ProxyURL()}},
	})

	// 1. Allowed host through the proxy: the sandboxed urllib honors the
	// injected HTTP_PROXY; the proxy resolves/dials on the host side.
	allowed, err := runner.Run(context.Background(), CommandSpec{
		Program: "python3",
		Args: []string{"-c", fmt.Sprintf(
			"import urllib.request; print(urllib.request.urlopen('http://allowed.test:%d/', timeout=10).read().decode())", port,
		)},
		Cwd:     root,
		Timeout: 20 * time.Second,
	})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if strings.TrimSpace(string(allowed.Stdout)) != "hello" {
		t.Fatalf("allowed fetch stdout = %q, want hello (stderr: %s)", allowed.Stdout, allowed.Stderr)
	}
	if allowed.Isolation != SeatbeltProxyIsolation.Name() {
		t.Fatalf("isolation = %q, want seatbelt+proxy", allowed.Isolation)
	}

	// 2. Any other host: the proxy policy refuses with 403.
	denied, err := runner.Run(context.Background(), CommandSpec{
		Program: "python3",
		Args: []string{"-c", fmt.Sprintf(
			"import urllib.request; urllib.request.urlopen('http://denied.test:%d/', timeout=10)", port,
		)},
		Cwd:     root,
		Timeout: 20 * time.Second,
	})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if denied.ExitCode == 0 || !strings.Contains(string(denied.Stderr), "403") {
		t.Fatalf("denied fetch: exit = %d, stderr = %q; want a 403 failure", denied.ExitCode, denied.Stderr)
	}

	// 3. Direct connection bypassing the proxy env: name resolution is
	// still denied inside the sandbox (fail-closed).
	direct, err := runner.Run(context.Background(), CommandSpec{
		Program: "python3",
		Args: []string{"-c", fmt.Sprintf(
			"import socket; socket.create_connection(('allowed.test', %d), timeout=5)", port,
		)},
		Cwd:     root,
		Timeout: 20 * time.Second,
	})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if direct.ExitCode == 0 {
		t.Fatal("direct connection must stay fail-closed under the proxy sandbox")
	}

	// 4. A NetworkFull grant must not pry the sandbox open either: the
	// proxy posture holds through the grant path.
	granted, err := runner.RunWithGrant(context.Background(), CommandSpec{
		Program: "python3",
		Args: []string{"-c", fmt.Sprintf(
			"import urllib.request; print(urllib.request.urlopen('http://allowed.test:%d/', timeout=10).read().decode())", port,
		)},
		Cwd:     root,
		Timeout: 20 * time.Second,
	}, Grant{NetworkFull: true})
	if err != nil {
		t.Fatalf("runner error = %v", err)
	}
	if strings.TrimSpace(string(granted.Stdout)) != "hello" {
		t.Fatalf("granted fetch stdout = %q, want hello via proxy (stderr: %s)", granted.Stdout, granted.Stderr)
	}
	if granted.Isolation != SeatbeltProxyIsolation.Name() {
		t.Fatalf("granted isolation = %q, want seatbelt+proxy", granted.Isolation)
	}
}
