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

package egress

import (
	"context"
	"errors"
	"net"
	"net/netip"
	"strings"
	"testing"
	"time"
)

func addr(t *testing.T, s string) netip.Addr {
	t.Helper()
	a, err := netip.ParseAddr(s)
	if err != nil {
		t.Fatalf("ParseAddr(%q): %v", s, err)
	}
	return a
}

func TestEmbeddedIPv4(t *testing.T) {
	cases := []struct {
		in   string
		want string // empty means no embedded form
	}{
		{"::ffff:127.0.0.1", "127.0.0.1"}, // IPv4-mapped
		{"64:ff9b::7f00:1", "127.0.0.1"},  // NAT64 well-known
		{"2002:7f00:1::", "127.0.0.1"},    // 6to4
		{"::127.0.0.1", "127.0.0.1"},      // IPv4-compatible
		{"2001:db8::1", ""},               // plain IPv6
		{"127.0.0.1", ""},                 // plain IPv4
	}
	for _, c := range cases {
		got, ok := embeddedIPv4(addr(t, c.in))
		if c.want == "" {
			if ok {
				t.Errorf("embeddedIPv4(%s) = %v, want none", c.in, got)
			}
			continue
		}
		if !ok || got.String() != c.want {
			t.Errorf("embeddedIPv4(%s) = %v, %v; want %s", c.in, got, ok, c.want)
		}
	}
}

func newTestGuard(local []netip.Addr) *addressGuard {
	return &addressGuard{
		lookupIP:   func(context.Context, string) ([]netip.Addr, error) { return nil, nil },
		dial:       func(context.Context, netip.AddrPort) (net.Conn, error) { return nil, errors.New("no dial") },
		localAddrs: func() ([]netip.Addr, error) { return local, nil },
	}
}

func TestCheckAddrDeniedClasses(t *testing.T) {
	g := newTestGuard([]netip.Addr{addr(t, "192.0.2.10")})
	cases := []struct {
		addr   string
		reason string // substring; empty means permitted
	}{
		{"93.184.216.34", ""},
		{"127.0.0.1", "loopback"},
		{"::1", "loopback"},
		{"0.0.0.0", "unspecified"},
		{"169.254.169.254", "link-local"},
		{"224.0.0.1", "multicast"},
		{"255.255.255.255", "broadcast"},
		{"100.100.100.200", "cloud metadata"},
		{"fd20:ce::254", "cloud metadata"},
		{"192.0.2.10", "this host"},                  // local interface address
		{"64:ff9b::a9fe:a9fe", "link-local"},         // NAT64 of 169.254.169.254
		{"2002:7f00:1::", "loopback"},                // 6to4 of 127.0.0.1
		{"::ffff:100.100.100.200", "cloud metadata"}, // 4-in-6 metadata
		{"fe80::1%en0", "link-local"},                // zone stripped before classify
		{"64:ff9b:1::7f00:1", "reserved"},            // local-use NAT64 (RFC 8215)
		{"100::1", "reserved"},                       // discard-only block (RFC 6666)
		{"64:ff9b::a9fe:a9fe", "link-local"},         // well-known NAT64 still decodes
	}
	for _, c := range cases {
		reason := g.checkAddr(addr(t, c.addr), nil, mustLocal(t, g))
		if c.reason == "" {
			if reason != "" {
				t.Errorf("checkAddr(%s) = %q, want permitted", c.addr, reason)
			}
			continue
		}
		if !strings.Contains(reason, c.reason) {
			t.Errorf("checkAddr(%s) = %q, want substring %q", c.addr, reason, c.reason)
		}
	}
}

func mustLocal(t *testing.T, g *addressGuard) []netip.Addr {
	t.Helper()
	local, err := g.localAddrs()
	if err != nil {
		t.Fatalf("localAddrs: %v", err)
	}
	return local
}

func TestCheckAddrExemption(t *testing.T) {
	g := newTestGuard(nil)
	exempt := []netip.Addr{addr(t, "127.0.0.1")}
	if reason := g.checkAddr(addr(t, "127.0.0.1"), exempt, nil); reason != "" {
		t.Errorf("exempt literal denied: %q", reason)
	}
	// The decoded form must never EARN an exemption: ::1's
	// IPv4-compatible decode is 0.0.0.1 — exempting 0.0.0.1 must not
	// rescue ::1 from the loopback class.
	spoof := []netip.Addr{addr(t, "0.0.0.1")}
	if reason := g.checkAddr(addr(t, "::1"), spoof, nil); !strings.Contains(reason, "loopback") {
		t.Errorf("decoded-form exemption spoof: checkAddr(::1) = %q, want loopback denial", reason)
	}
	// 4-in-6 spelling of an exempt IPv4 still exempts (same address).
	if reason := g.checkAddr(addr(t, "::ffff:127.0.0.1"), exempt, nil); reason != "" {
		t.Errorf("4-in-6 spelling of exempt literal denied: %q", reason)
	}
}

func TestResolveLoopbackNames(t *testing.T) {
	exempt := []netip.Addr{addr(t, "127.0.0.1")}
	cases := []struct {
		host    string
		answers []string
		wantErr string // empty means success
	}{
		{"localhost", []string{"127.0.0.1"}, ""},
		{"foo.localhost", []string{"::1", "127.0.0.1"}, ""},
		{"localhost", []string{"93.184.216.34"}, "non-loopback"},
		{"localhost", []string{"10.0.0.1"}, "non-loopback"},
	}
	for _, c := range cases {
		g := &addressGuard{
			lookupIP: func(context.Context, string) ([]netip.Addr, error) {
				out := make([]netip.Addr, 0, len(c.answers))
				for _, s := range c.answers {
					out = append(out, addr(t, s))
				}
				return out, nil
			},
			dial:       func(context.Context, netip.AddrPort) (net.Conn, error) { return nil, errors.New("no dial") },
			localAddrs: func() ([]netip.Addr, error) { return nil, nil },
		}
		kept, err := g.resolve(context.Background(), c.host, exempt)
		if c.wantErr == "" {
			if err != nil {
				t.Errorf("resolve(%s) err = %v, want success", c.host, err)
			} else if len(kept) == 0 {
				t.Errorf("resolve(%s) kept nothing", c.host)
			}
			continue
		}
		var denied *guardDeniedError
		if !errors.As(err, &denied) || !strings.Contains(err.Error(), c.wantErr) {
			t.Errorf("resolve(%s) err = %v, want guard denial containing %q", c.host, err, c.wantErr)
		}
	}
}

func TestDialGuardedLiteralTargets(t *testing.T) {
	var dialedAddr string
	dial := func(_ context.Context, ap netip.AddrPort) (net.Conn, error) {
		dialedAddr = ap.String()
		return nil, errors.New("stop after dial attempt")
	}
	newGuard := func() *addressGuard {
		return &addressGuard{
			lookupIP:   func(context.Context, string) ([]netip.Addr, error) { return nil, nil },
			dial:       dial,
			localAddrs: func() ([]netip.Addr, error) { return nil, nil },
		}
	}

	// Explicitly allow-listed literal: exempt from the guard, dialed.
	g := newGuard()
	_, _, err := g.dialGuarded(context.Background(),
		Decision{Allow: true, Matched: true, ExemptLiterals: []netip.Addr{addr(t, "127.0.0.1")}},
		"127.0.0.1", 3000)
	if err == nil || dialedAddr != "127.0.0.1:3000" {
		t.Errorf("allow-listed literal: err = %v, dialed = %q", err, dialedAddr)
	}

	// Unmatched-allow posture: public literal passes, metadata literal
	// must be stopped by the guard even though the policy allowed it.
	dialedAddr = ""
	g = newGuard()
	_, _, err = g.dialGuarded(context.Background(),
		Decision{Allow: true, Matched: false}, "93.184.216.34", 443)
	if err == nil || dialedAddr != "93.184.216.34:443" {
		t.Errorf("public literal: err = %v, dialed = %q", err, dialedAddr)
	}
	g = newGuard()
	_, _, err = g.dialGuarded(context.Background(),
		Decision{Allow: true, Matched: false}, "169.254.169.254", 80)
	var denied *guardDeniedError
	if !errors.As(err, &denied) {
		t.Errorf("metadata literal under unmatched-allow: err = %v, want guard denial", err)
	}
	g = newGuard()
	_, _, err = g.dialGuarded(context.Background(),
		Decision{Allow: true, Matched: false}, "127.0.0.1", 8080)
	if !errors.As(err, &denied) {
		t.Errorf("loopback literal under unmatched-allow: err = %v, want guard denial", err)
	}
}

func TestDialGuardedSelfConnect(t *testing.T) {
	g := &addressGuard{
		lookupIP:   func(context.Context, string) ([]netip.Addr, error) { return nil, nil },
		dial:       func(context.Context, netip.AddrPort) (net.Conn, error) { return nil, errors.New("no dial") },
		localAddrs: func() ([]netip.Addr, error) { return nil, nil },
		selfPort:   9090,
	}
	// Even an explicitly allow-listed loopback literal cannot make the
	// proxy connect to its own listener (recursive tunnel).
	_, _, err := g.dialGuarded(context.Background(),
		Decision{Allow: true, Matched: true, ExemptLiterals: []netip.Addr{addr(t, "127.0.0.1")}},
		"127.0.0.1", 9090)
	if !errors.Is(err, errTargetIsProxy) {
		t.Errorf("self-connect: err = %v, want errTargetIsProxy", err)
	}
}

func TestParseAuthority(t *testing.T) {
	good := []struct {
		in      string
		defPort int
		host    string
		port    int
	}{
		{"example.com", 443, "example.com", 443},
		{"Example.COM.", 443, "example.com", 443},
		{"api.example.com:8443", 443, "api.example.com", 8443},
		{"127.0.0.1:3000", 80, "127.0.0.1", 3000},
		{"[::1]:443", 443, "::1", 443},
		{"[2001:db8::1]", 443, "2001:db8::1", 443},
		{"2001:db8::1", 443, "2001:db8::1", 443},
	}
	for _, c := range good {
		host, port, err := parseAuthority(c.in, c.defPort)
		if err != nil || host != c.host || port != c.port {
			t.Errorf("parseAuthority(%q) = %q, %d, %v; want %q, %d", c.in, host, port, err, c.host, c.port)
		}
	}
	bad := []string{
		"", "example.com:0", "example.com:65536", "example.com:-1", "example.com:abc",
		"user@example.com:443", "example.com:443/path", "example.com:443?q=1",
		"[fe80::1%en0]:443", "fe80::1%en0", "[::1", "::1]extra", "exa mple.com",
		"example.com:443:80",
	}
	for _, in := range bad {
		if host, port, err := parseAuthority(in, 443); err == nil {
			t.Errorf("parseAuthority(%q) = %q, %d, nil; want error", in, host, port)
		}
	}
}

func TestValidateHostControlChars(t *testing.T) {
	if err := validateHost("evil\r\nhost"); err == nil {
		t.Error("CRLF host accepted")
	}
	if err := validateHost("evil\x00host"); err == nil {
		t.Error("NUL host accepted")
	}
	if err := validateHost("ok.example.com"); err != nil {
		t.Errorf("normal host rejected: %v", err)
	}
}

// TestValidateHostPrintableASCII: spaces and non-ASCII (IDN) spellings
// are rejected — an IDNA-aware resolver could otherwise resolve a
// homograph the policy's deny rules never matched.
func TestValidateHostPrintableASCII(t *testing.T) {
	if err := validateHost("exa mple.com"); err == nil {
		t.Error("space-carrying host accepted")
	}
	if err := validateHost("еxample.com"); err == nil { // Cyrillic e
		t.Error("IDN homograph host accepted")
	}
	if err := validateHost("xn--xample-9of.com"); err != nil {
		t.Errorf("punycode host rejected: %v", err)
	}
}

// TestCheckAddrPrivateClass: DenyPrivate bars private ranges while
// explicit allow-listed literals stay exempt; without the flag the
// same addresses pass.
func TestCheckAddrPrivateClass(t *testing.T) {
	private := []string{"10.0.0.1", "172.16.5.5", "192.168.1.1", "100.64.1.1", "fd12::1"}
	g := newTestGuard(nil)
	g.denyPrivate = true
	for _, p := range private {
		if reason := g.checkAddr(addr(t, p), nil, nil); !strings.Contains(reason, "private") {
			t.Errorf("denyPrivate checkAddr(%s) = %q, want private denial", p, reason)
		}
	}
	// Exemption semantics are unchanged: an explicit allow-listed
	// literal still punches through.
	exempt := []netip.Addr{addr(t, "10.0.0.1")}
	if reason := g.checkAddr(addr(t, "10.0.0.1"), exempt, nil); reason != "" {
		t.Errorf("exempt private literal denied: %q", reason)
	}
	// Public space unaffected.
	if reason := g.checkAddr(addr(t, "93.184.216.34"), nil, nil); reason != "" {
		t.Errorf("public address denied under denyPrivate: %q", reason)
	}
	// Without the flag the private ranges pass (documented default).
	gOff := newTestGuard(nil)
	for _, p := range private {
		if reason := gOff.checkAddr(addr(t, p), nil, nil); reason != "" {
			t.Errorf("default checkAddr(%s) = %q, want permitted", p, reason)
		}
	}
}

// TestDialGuardedResolveTimeout: a resolver that never answers is cut
// off by the guard's overall timeout instead of pinning the
// connection slot on the resolver's internal retry budget.
func TestDialGuardedResolveTimeout(t *testing.T) {
	g := newTestGuard(nil)
	g.timeout = 50 * time.Millisecond
	g.lookupIP = func(ctx context.Context, _ string) ([]netip.Addr, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	}
	start := time.Now()
	_, _, err := g.dialGuarded(context.Background(), Decision{Allow: true, Matched: true}, "slow.test", 443)
	if err == nil || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("err = %v, want context deadline exceeded", err)
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("resolve bounded after %v, want ~guard timeout", elapsed)
	}
}
