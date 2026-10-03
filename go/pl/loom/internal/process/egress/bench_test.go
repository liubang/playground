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
	"encoding/base64"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
)

// The benchmarks here are hermetic: loopback-only upstreams, a fake
// resolver, and no sandbox-exec — they quantify the proxy's own cost
// (extra hop, relay copies, policy/guard work) against direct baselines.
//
// Cycle benchmarks burn one or two ephemeral ports per iteration, and
// TIME_WAIT holds them for ~15s afterwards, so a duration-based run
// exhausts the port range ("can't assign requested address") no matter
// how fast the proxy is. Run with fixed iteration counts instead:
//
//	bazel run //go/pl/loom/internal/process/egress:egress_test -- \
//	  -test.run='^$' -test.bench=. -test.benchtime=3000x
//
// Client-side connections are closed with SO_LINGER=0 (RST, no
// TIME_WAIT) to halve the pressure; the proxy-side upstream connection
// still lingers, which is why the count cap matters.

// benchPolicy allows every destination and exempts loopback, so the
// guard passes the loopback upstreams benchmarks dial.
type benchPolicy struct{}

func (benchPolicy) Decide(string, int) Decision {
	return Decision{
		Allow: true, Matched: true, Reason: "bench", Rule: "bench",
		ExemptLiterals: []netip.Addr{netip.MustParseAddr("127.0.0.1")},
	}
}

// newBenchServer starts a proxy with a fake resolver mapping every name
// to loopback. realLocalAddrs selects the production InterfaceAddrs
// probe (to surface its per-connection cost) instead of a stub.
func newBenchServer(b *testing.B, realLocalAddrs bool) *Server {
	b.Helper()
	cfg := Config{
		Policy: benchPolicy{},
		LookupIP: func(context.Context, string) ([]netip.Addr, error) {
			return []netip.Addr{netip.MustParseAddr("127.0.0.1")}, nil
		},
		LocalAddrs: func() ([]netip.Addr, error) { return nil, nil },
	}
	if realLocalAddrs {
		cfg.LocalAddrs = nil
	}
	server, err := NewServer(cfg)
	if err != nil {
		b.Fatalf("NewServer: %v", err)
	}
	b.Cleanup(func() { _ = server.Close() })
	return server
}

// startEchoUpstream runs a loopback TCP echo server and returns its
// host:port.
func startEchoUpstream(b *testing.B) string {
	b.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		b.Fatalf("listen: %v", err)
	}
	b.Cleanup(func() { _ = ln.Close() })
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
	return ln.Addr().String()
}

// bufferedConn reads through the bufio.Reader that consumed the proxy
// response headers, so no tunneled byte is stranded in the buffer.
type bufferedConn struct {
	net.Conn
	r *bufio.Reader
}

func (c bufferedConn) Read(p []byte) (int, error) { return c.r.Read(p) }

// rstOnClose arms SO_LINGER=0 so benchmark-close sends RST instead of
// lingering in TIME_WAIT (the payload is fully read by then).
func rstOnClose(conn net.Conn) net.Conn {
	if tc, ok := conn.(*net.TCPConn); ok {
		_ = tc.SetLinger(0)
	}
	return conn
}

// openTunnel establishes one CONNECT tunnel through the proxy.
func openTunnel(server *Server, target string) (net.Conn, error) {
	conn, err := net.Dial("tcp", server.Addr())
	if err != nil {
		return nil, fmt.Errorf("dial proxy: %w", err)
	}
	auth := base64.StdEncoding.EncodeToString([]byte("loom:" + server.Token()))
	if _, err := fmt.Fprintf(conn, "CONNECT %s HTTP/1.1\r\nProxy-Authorization: Basic %s\r\n\r\n", target, auth); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("write CONNECT: %w", err)
	}
	br := bufio.NewReader(conn)
	line, err := br.ReadString('\n')
	if err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("read status: %w", err)
	}
	if !strings.Contains(line, "200") {
		_ = conn.Close()
		return nil, fmt.Errorf("CONNECT status = %q, want 200", strings.TrimSpace(line))
	}
	// Drain the remaining header lines.
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			_ = conn.Close()
			return nil, fmt.Errorf("read headers: %w", err)
		}
		if line == "\r\n" {
			break
		}
	}
	return bufferedConn{Conn: rstOnClose(conn), r: br}, nil
}

// echoRoundTrip writes payload and reads exactly len(payload) back.
func echoRoundTrip(conn net.Conn, payload []byte) error {
	if _, err := conn.Write(payload); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	if _, err := io.ReadFull(conn, payload); err != nil {
		return fmt.Errorf("read: %w", err)
	}
	return nil
}

// connectCycle is one full connection lifecycle (dial or CONNECT
// tunnel, one echo, close); the error return keeps it usable inside
// RunParallel (whose goroutines must not call b.Fatal).
func connectCycle(server *Server, target string, payload []byte) error {
	var conn net.Conn
	var err error
	if server != nil {
		conn, err = openTunnel(server, target) // already RST-armed
	} else if conn, err = net.Dial("tcp", target); err == nil {
		conn = rstOnClose(conn)
	}
	if err != nil {
		return err
	}
	defer func() { _ = conn.Close() }()
	return echoRoundTrip(conn, payload)
}

// failOnFirst records the first error from parallel goroutines.
type failOnFirst struct{ v atomic.Value }

func (f *failOnFirst) record(err error) {
	if err != nil {
		f.v.CompareAndSwap(nil, err.Error())
	}
}

func (f *failOnFirst) check(b *testing.B) {
	b.Helper()
	if err := f.v.Load(); err != nil {
		b.Fatalf("parallel cycle failed: %v", err)
	}
}

// BenchmarkConnectCycle measures the FULL per-connection cost: dial,
// CONNECT handshake (parse + auth + policy + guard + upstream dial),
// one tiny echo, close. This is the fixed tax every proxied command
// pays per connection.
func BenchmarkConnectCycle(b *testing.B) {
	upstream := startEchoUpstream(b)
	// Domain target: the proxy cycles must exercise the production
	// resolve() path (fake lookup + guard), not the literal shortcut.
	target := "bench.test:" + upstream[strings.LastIndexByte(upstream, ':')+1:]
	server := newBenchServer(b, false)
	payload := []byte("ping")

	b.Run("direct", func(b *testing.B) {
		for b.Loop() {
			if err := connectCycle(nil, upstream, payload); err != nil {
				b.Fatal(err)
			}
		}
	})
	b.Run("proxy", func(b *testing.B) {
		for b.Loop() {
			if err := connectCycle(server, target, payload); err != nil {
				b.Fatal(err)
			}
		}
	})
	// Same, but with the production InterfaceAddrs probe in the guard:
	// isolates what resolve()'s per-connection host-address check costs.
	b.Run("proxy-real-localaddrs", func(b *testing.B) {
		server := newBenchServer(b, true)
		for b.Loop() {
			if err := connectCycle(server, target, payload); err != nil {
				b.Fatal(err)
			}
		}
	})
}

// BenchmarkTunnelThroughput measures steady-state relay throughput on
// one persistent connection: 32 KiB echo round-trips.
func BenchmarkTunnelThroughput(b *testing.B) {
	upstream := startEchoUpstream(b)
	server := newBenchServer(b, false)
	payload := make([]byte, 32<<10)

	b.Run("direct", func(b *testing.B) {
		conn, err := net.Dial("tcp", upstream)
		if err != nil {
			b.Fatalf("dial: %v", err)
		}
		defer func() { _ = conn.Close() }()
		b.SetBytes(int64(len(payload)))
		b.ResetTimer()
		for b.Loop() {
			if err := echoRoundTrip(conn, payload); err != nil {
				b.Fatal(err)
			}
		}
	})
	b.Run("proxy", func(b *testing.B) {
		target := "bench.test:" + upstream[strings.LastIndexByte(upstream, ':')+1:]
		conn, err := openTunnel(server, target)
		if err != nil {
			b.Fatal(err)
		}
		defer func() { _ = conn.Close() }()
		b.SetBytes(int64(len(payload)))
		b.ResetTimer()
		for b.Loop() {
			if err := echoRoundTrip(conn, payload); err != nil {
				b.Fatal(err)
			}
		}
	})
}

// BenchmarkConcurrentCycle measures aggregate ops/sec when many
// goroutines run full connect cycles in parallel — the shape of a
// build tool fanning out package downloads.
func BenchmarkConcurrentCycle(b *testing.B) {
	upstream := startEchoUpstream(b)
	target := "bench.test:" + upstream[strings.LastIndexByte(upstream, ':')+1:]
	server := newBenchServer(b, false)
	payload := []byte("ping")

	b.Run("direct", func(b *testing.B) {
		var failures failOnFirst
		b.RunParallel(func(pb *testing.PB) {
			for pb.Next() {
				failures.record(connectCycle(nil, upstream, payload))
			}
		})
		failures.check(b)
	})
	b.Run("proxy", func(b *testing.B) {
		var failures failOnFirst
		b.RunParallel(func(pb *testing.PB) {
			for pb.Next() {
				failures.record(connectCycle(server, target, payload))
			}
		})
		failures.check(b)
	})
}

// BenchmarkPlainHTTP measures per-request latency of the plain-HTTP
// leg (one upstream connection per request, no redirect chasing —
// documented v1 shape) against a direct client.
func BenchmarkPlainHTTP(b *testing.B) {
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("hello"))
	}))
	b.Cleanup(upstream.Close)
	server := newBenchServer(b, false)

	newClient := func(proxy bool) *http.Client {
		transport := &http.Transport{
			DisableKeepAlives: true,
			// RST on close keeps benchmark iterations out of TIME_WAIT.
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				conn, err := (&net.Dialer{}).DialContext(ctx, network, addr)
				return rstOnClose(conn), err
			},
		}
		if proxy {
			transport.Proxy = http.ProxyURL(mustParseURL(b, server.ProxyURL()))
		}
		return &http.Client{Transport: transport}
	}
	get := func(b *testing.B, client *http.Client, url string) {
		b.Helper()
		resp, err := client.Get(url)
		if err != nil {
			b.Fatalf("GET: %v", err)
		}
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
	}

	b.Run("direct", func(b *testing.B) {
		client := newClient(false)
		for b.Loop() {
			get(b, client, upstream.URL)
		}
	})
	b.Run("proxy", func(b *testing.B) {
		client := newClient(true)
		// The fake resolver maps every name to loopback; the upstream
		// port is preserved from the request URL.
		url := strings.Replace(upstream.URL, "127.0.0.1", "bench.test", 1)
		for b.Loop() {
			get(b, client, url)
		}
	})
}

func mustParseURL(b *testing.B, raw string) *url.URL {
	b.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		b.Fatalf("parse proxy URL: %v", err)
	}
	return u
}

// BenchmarkInterfaceAddrs sizes the guard's per-connection
// net.InterfaceAddrs probe (called on every resolve and literal check).
func BenchmarkInterfaceAddrs(b *testing.B) {
	for b.Loop() {
		if _, err := net.InterfaceAddrs(); err != nil {
			b.Fatalf("InterfaceAddrs: %v", err)
		}
	}
}
