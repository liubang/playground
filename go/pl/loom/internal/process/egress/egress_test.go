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
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"strings"
	"sync"
	"testing"
	"time"
)

var loopback4 = netip.MustParseAddr("127.0.0.1")

// mapPolicy answers Decide from a host-keyed table; absent hosts get
// the unmatched-allow default posture.
type mapPolicy map[string]Decision

func (m mapPolicy) Decide(host string, _ int) Decision {
	if d, ok := m[host]; ok {
		return d
	}
	return Decision{Allow: true, Reason: "no rule matched", Rule: "unmatched-allow"}
}

// allowExempt is the decision a host needs for its loopback test
// resolution to pass the guard.
func allowExempt() Decision {
	return Decision{
		Allow: true, Matched: true, Reason: "allowed", Rule: "builtin",
		ExemptLiterals: []netip.Addr{loopback4},
	}
}

type recordLogger struct {
	mu   sync.Mutex
	recs []ConnRecord
}

func (l *recordLogger) LogConn(r ConnRecord) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.recs = append(l.recs, r)
}

func (l *recordLogger) last() ConnRecord {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.recs) == 0 {
		return ConnRecord{}
	}
	return l.recs[len(l.recs)-1]
}

// startEcho runs a TCP echo server on loopback and returns its port.
func startEcho(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("echo listen: %v", err)
	}
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer func() { _ = conn.Close() }()
				_, _ = io.Copy(conn, conn)
			}()
		}
	}()
	t.Cleanup(func() { _ = ln.Close() })
	return ln.Addr().(*net.TCPAddr).Port
}

type serverOpts struct {
	policy      Policy
	socksPolicy Policy
	// lookup overrides hostname → answers; absent names resolve to
	// 127.0.0.1 (the guard then needs an exempt literal to pass).
	lookup    map[string][]string
	handshake time.Duration
	maxConns  int
}

func newTestServer(t *testing.T, opts serverOpts) (*Server, *recordLogger) {
	t.Helper()
	policy := opts.policy
	if policy == nil {
		policy = mapPolicy{}
	}
	logger := &recordLogger{}
	server, err := NewServer(Config{
		Policy:           policy,
		SOCKSPolicy:      opts.socksPolicy,
		Logger:           logger,
		MaxConns:         opts.maxConns,
		HandshakeTimeout: opts.handshake,
		LookupIP: func(_ context.Context, host string) ([]netip.Addr, error) {
			answers := []string{"127.0.0.1"}
			if opts.lookup != nil {
				if override, ok := opts.lookup[host]; ok {
					answers = override
				}
			}
			out := make([]netip.Addr, 0, len(answers))
			for _, s := range answers {
				a, err := netip.ParseAddr(s)
				if err != nil {
					return nil, err
				}
				out = append(out, a)
			}
			return out, nil
		},
		LocalAddrs: func() ([]netip.Addr, error) { return nil, nil },
	})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	t.Cleanup(func() { _ = server.Close() })
	return server, logger
}

// dialProxy opens a client connection to the proxy.
func dialProxy(t *testing.T, server *Server) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp", server.Addr(), 2*time.Second)
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return conn
}

func (s *Server) authHeader() string {
	return "Basic " + basicAuth("loom", s.Token())
}

// TestMuxRejectsUnknownProtocol: neither SOCKS5 nor an HTTP method.
func TestMuxRejectsUnknownProtocol(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{})
	conn := dialProxy(t, server)
	if _, err := conn.Write([]byte{0x04, 0x01, 0x00}); err != nil { // SOCKS4-ish
		t.Fatalf("write: %v", err)
	}
	buf := make([]byte, 64)
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := conn.Read(buf)
	if err != nil && !errors.Is(err, io.EOF) {
		t.Fatalf("read: %v", err)
	}
	if n == 0 {
		t.Fatal("expected a 400 response, got EOF")
	}
	if got := string(buf[:n]); !strings.Contains(got, "400 Bad Request") {
		t.Fatalf("response = %q, want 400 Bad Request", got)
	}
}

// TestSilentConnTimedOut: a connection that never sends a byte must not
// hold a descriptor past the handshake timeout.
func TestSilentConnTimedOut(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{handshake: 60 * time.Millisecond})
	conn := dialProxy(t, server)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	start := time.Now()
	_, err := conn.Read(make([]byte, 1))
	if err == nil {
		t.Fatal("silent connection was not closed")
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("silent connection closed after %v, want ~handshake timeout", elapsed)
	}
}

// TestMaxConns: beyond the cap, new connections are refused cheaply.
func TestMaxConns(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{
		policy:   mapPolicy{"example.test": allowExempt()},
		maxConns: 2,
	})
	hold := make([]net.Conn, 0, 2)
	for range 2 {
		conn := dialProxy(t, server)
		connectRequest(t, conn, server, "example.test", echoPort)
		hold = append(hold, conn)
	}
	third := dialProxy(t, server)
	_ = third.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, err := third.Read(make([]byte, 1))
	if err == nil {
		t.Fatal("over-capacity connection was not refused")
	}
}

// TestMuxAllProtocolsOnePort: one listener interleaves CONNECT, plain
// HTTP, and SOCKS5 across SEPARATE client connections, each getting its
// protocol's policy leg and a correctly-typed connection record. This
// is the shape the sandboxed workload actually produces: curl speaks
// CONNECT/absolute-form while a SOCKS-configured tool speaks SOCKS5,
// all against the same loopback port.
func TestMuxAllProtocolsOnePort(t *testing.T) {
	echoPort := startEcho(t)
	upstream, _ := startHTTPUpstream(t)
	httpPort := upstreamPort(t, upstream)
	server, logger := newTestServer(t, serverOpts{
		policy:      mapPolicy{"example.test": allowExempt()},
		socksPolicy: mapPolicy{"example.test": allowExempt()},
	})

	echoRoundTrip := func(t *testing.T, conn net.Conn, br *bufio.Reader, payload string) {
		t.Helper()
		if _, err := conn.Write([]byte(payload)); err != nil {
			t.Fatalf("write payload: %v", err)
		}
		buf := make([]byte, len(payload))
		if _, err := io.ReadFull(br, buf); err != nil {
			t.Fatalf("read echo: %v", err)
		}
		if string(buf) != payload {
			t.Fatalf("echo = %q, want %q", buf, payload)
		}
	}

	// Leg 1: CONNECT tunnel.
	conn1 := dialProxy(t, server)
	br1 := connectRequest(t, conn1, server, "example.test", echoPort)
	echoRoundTrip(t, conn1, br1, "via-connect")

	// Leg 2: plain HTTP absolute-form against a real HTTP upstream — a
	// full success round-trip (status + body), not just dial proof.
	conn2 := dialProxy(t, server)
	resp := proxyGet(t, conn2, server, fmt.Sprintf("http://example.test:%d/ok", httpPort))
	body, err := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if err != nil || resp.StatusCode != 200 || string(body) != "hello" {
		t.Fatalf("plain HTTP leg: status=%d body=%q err=%v, want 200 hello", resp.StatusCode, body, err)
	}

	// Leg 3: SOCKS5 (consulting the conservative SOCKS policy).
	conn3 := dialProxy(t, server)
	br3 := bufio.NewReader(conn3)
	_ = conn3.SetReadDeadline(time.Now().Add(3 * time.Second))
	socksGreeting(t, conn3, br3)
	socksConnectDomain(t, conn3, "example.test", echoPort)
	if rep := socksReply(t, br3); rep != socksRepSucceeded {
		t.Fatalf("socks rep = %d, want success", rep)
	}
	echoRoundTrip(t, conn3, br3, "via-socks")

	// Three records, one per protocol, all allowed.
	logger.mu.Lock()
	recs := append([]ConnRecord(nil), logger.recs...)
	logger.mu.Unlock()
	wantProtos := []string{"connect", "http", "socks5"}
	if len(recs) < len(wantProtos) {
		t.Fatalf("records = %d, want at least %d (%v)", len(recs), len(wantProtos), wantProtos)
	}
	for i, want := range wantProtos {
		if recs[i].Proto != want || !recs[i].Decision.Allow || recs[i].Host != "example.test" {
			t.Fatalf("record[%d] = %+v, want allowed %s for example.test", i, recs[i], want)
		}
	}
}

// TestLongLivedTunnelSurvivesHandshakeTimeout: the pre-tunnel deadline
// must be cleared once the tunnel is up, or long-lived connections die
// by their own handshake budget.
func TestLongLivedTunnelSurvivesHandshakeTimeout(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{
		policy:    mapPolicy{"example.test": allowExempt()},
		handshake: 200 * time.Millisecond,
	})
	conn := dialProxy(t, server)
	connectRequest(t, conn, server, "example.test", echoPort)
	time.Sleep(400 * time.Millisecond) // well past the handshake budget
	if _, err := conn.Write([]byte("alive")); err != nil {
		t.Fatalf("write after idle: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 5)
	if _, err := io.ReadFull(conn, buf); err != nil {
		t.Fatalf("echo after idle: %v", err)
	}
	if string(buf) != "alive" {
		t.Fatalf("echo = %q, want %q", buf, "alive")
	}
}
