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
	"encoding/base64"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func basicAuth(user, pass string) string {
	return base64.StdEncoding.EncodeToString([]byte(user + ":" + pass))
}

// readStatusLine reads one HTTP response status line from the proxy.
func readStatusLine(t *testing.T, conn net.Conn) string {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil {
		t.Fatalf("read status line: %v", err)
	}
	return strings.TrimSpace(line)
}

// connectRequest issues a CONNECT and asserts the 200; the returned
// reader wraps the connection and must be used for any further reads
// (it may already hold buffered tunnel bytes).
func connectRequest(t *testing.T, conn net.Conn, server *Server, host string, port int, extra ...string) *bufio.Reader {
	t.Helper()
	br := bufio.NewReader(conn)
	var sb strings.Builder
	fmt.Fprintf(&sb, "CONNECT %s:%d HTTP/1.1\r\nProxy-Authorization: %s\r\n", host, port, server.authHeader())
	for _, h := range extra {
		sb.WriteString(h)
	}
	sb.WriteString("\r\n")
	if _, err := conn.Write([]byte(sb.String())); err != nil {
		t.Fatalf("write CONNECT: %v", err)
	}
	status, err := br.ReadString('\n')
	if err != nil {
		t.Fatalf("read CONNECT status: %v", err)
	}
	if !strings.Contains(status, "200") {
		t.Fatalf("CONNECT status = %q, want 200", status)
	}
	// Drain the header terminator.
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			t.Fatalf("read CONNECT headers: %v", err)
		}
		if line == "\r\n" {
			break
		}
	}
	return br
}

func TestCONNECTTunnelEcho(t *testing.T) {
	echoPort := startEcho(t)
	server, logger := newTestServer(t, serverOpts{policy: mapPolicy{"example.test": allowExempt()}})
	conn := dialProxy(t, server)
	br := connectRequest(t, conn, server, "example.test", echoPort)
	if _, err := conn.Write([]byte("ping")); err != nil {
		t.Fatalf("write payload: %v", err)
	}
	buf := make([]byte, 4)
	if _, err := io.ReadFull(br, buf); err != nil {
		t.Fatalf("read echo: %v", err)
	}
	if string(buf) != "ping" {
		t.Fatalf("echo = %q, want %q", buf, "ping")
	}
	rec := logger.last()
	if rec.Proto != "connect" || rec.Host != "example.test" || rec.Port != echoPort {
		t.Fatalf("record = %+v", rec)
	}
	if !rec.Decision.Allow || rec.Dialed != "127.0.0.1" || rec.Err != "" {
		t.Fatalf("record = %+v, want allowed dial of 127.0.0.1", rec)
	}
}

// TestCONNECTPipelinedFirstFlight: payload that arrives in the same
// write as the CONNECT header must be forwarded, not stranded in the
// header-phase buffer.
func TestCONNECTPipelinedFirstFlight(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{"example.test": allowExempt()}})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	head := fmt.Sprintf("CONNECT example.test:%d HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\npipelined",
		echoPort, server.authHeader())
	if _, err := conn.Write([]byte(head)); err != nil {
		t.Fatalf("write: %v", err)
	}
	status, err := br.ReadString('\n')
	if err != nil || !strings.Contains(status, "200") {
		t.Fatalf("status = %q, %v", status, err)
	}
	if line, err := br.ReadString('\n'); err != nil || line != "\r\n" {
		t.Fatalf("header terminator = %q, %v", line, err)
	}
	buf := make([]byte, len("pipelined"))
	if _, err := io.ReadFull(br, buf); err != nil {
		t.Fatalf("read pipelined echo: %v", err)
	}
	if string(buf) != "pipelined" {
		t.Fatalf("pipelined echo = %q", buf)
	}
}

func TestCONNECTPolicyDeny(t *testing.T) {
	server, logger := newTestServer(t, serverOpts{policy: mapPolicy{
		"webhook.site": {Allow: false, Matched: true, Reason: "request capture service", Rule: "builtin"},
	}})
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "CONNECT webhook.site:443 HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", server.authHeader())
	status := readStatusLine(t, conn)
	if !strings.Contains(status, "403") {
		t.Fatalf("status = %q, want 403", status)
	}
	rec := logger.last()
	if rec.Decision.Allow || rec.Decision.Rule != "builtin" || rec.Dialed != "" {
		t.Fatalf("record = %+v, want denied builtin hit", rec)
	}
}

func TestCONNECTAuth(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{"example.test": allowExempt()}})

	// No credentials → 407 with Proxy-Authenticate. One bufio.Reader for
	// the whole connection: a fresh Reader per read would strand the
	// buffered header bytes.
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "CONNECT example.test:%d HTTP/1.1\r\n\r\n", echoPort)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	status, err := br.ReadString('\n')
	if err != nil || !strings.Contains(status, "407") {
		t.Fatalf("no-auth status = %q, %v; want 407", status, err)
	}
	headers, err := br.ReadString('\n')
	if err != nil || !strings.Contains(headers, "Proxy-Authenticate") {
		t.Fatalf("407 missing Proxy-Authenticate: %q, %v", headers, err)
	}

	// Wrong token → 407.
	conn2 := dialProxy(t, server)
	fmt.Fprintf(conn2, "CONNECT example.test:%d HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n",
		echoPort, "Basic "+basicAuth("loom", "wrong"))
	if status := readStatusLine(t, conn2); !strings.Contains(status, "407") {
		t.Fatalf("wrong-token status = %q, want 407", status)
	}
}

func TestCONNECTBadAuthority(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{})
	bad := []string{
		"example.com:99999",
		"example.com:0",
		"user@example.com:443",
		"example.com:443/path",
		"[fe80::1%en0]:443",
	}
	for _, authority := range bad {
		conn := dialProxy(t, server)
		fmt.Fprintf(conn, "CONNECT %s HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", authority, server.authHeader())
		if status := readStatusLine(t, conn); !strings.Contains(status, "400") {
			t.Fatalf("CONNECT %s: status = %q, want 400", authority, status)
		}
	}
}

// TestCONNECTGuardDenied: the policy allows the name, but its answer is
// a metadata address — the guard must stop it (403, not 502).
func TestCONNECTGuardDenied(t *testing.T) {
	server, logger := newTestServer(t, serverOpts{
		policy: mapPolicy{"meta.test": {Allow: true, Matched: true, Reason: "allowed", Rule: "user"}},
		lookup: map[string][]string{"meta.test": {"169.254.169.254"}},
	})
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "CONNECT meta.test:80 HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", server.authHeader())
	if status := readStatusLine(t, conn); !strings.Contains(status, "403") {
		t.Fatalf("status = %q, want 403", status)
	}
	rec := logger.last()
	if rec.Err == "" || rec.Dialed != "" {
		t.Fatalf("record = %+v, want guard denial", rec)
	}
}

// TestCONNECTLiteralMetadata: under the unmatched-allow posture an IP
// literal still faces the guard's denied classes.
func TestCONNECTLiteralMetadata(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{}})
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "CONNECT 169.254.169.254:80 HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", server.authHeader())
	if status := readStatusLine(t, conn); !strings.Contains(status, "403") {
		t.Fatalf("status = %q, want 403", status)
	}
}

// TestCONNECTSelfConnect: even an explicitly allow-listed loopback
// literal cannot make the proxy dial its own listener.
func TestCONNECTSelfConnect(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{"127.0.0.1": allowExempt()}})
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "CONNECT 127.0.0.1:%d HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", server.Port(), server.authHeader())
	if status := readStatusLine(t, conn); !strings.Contains(status, "403") {
		t.Fatalf("status = %q, want 403", status)
	}
}

// TestHeaderCap: an oversized request head gets a 431, not a hung
// connection or a memory blow-up.
func TestHeaderCap(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{})
	conn := dialProxy(t, server)
	go func() {
		fmt.Fprintf(conn, "CONNECT example.test:443 HTTP/1.1\r\nProxy-Authorization: %s\r\nX-Pad: %s\r\n\r\n",
			server.authHeader(), strings.Repeat("a", maxHeaderBytes*2))
	}()
	if status := readStatusLine(t, conn); !strings.Contains(status, "431") {
		t.Fatalf("status = %q, want 431", status)
	}
}

// startHTTPUpstream records whether Proxy-Authorization leaked and can
// answer a redirect.
func startHTTPUpstream(t *testing.T) (*httptest.Server, *bool) {
	t.Helper()
	var leaked bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Proxy-Authorization") != "" {
			leaked = true
		}
		switch r.URL.Path {
		case "/redirect":
			w.Header().Set("Location", "http://denied.example/x")
			w.WriteHeader(http.StatusFound)
		default:
			_, _ = w.Write([]byte("hello"))
		}
	}))
	t.Cleanup(server.Close)
	return server, &leaked
}

func upstreamPort(t *testing.T, upstream *httptest.Server) int {
	t.Helper()
	var port int
	if _, err := fmt.Sscanf(strings.TrimPrefix(upstream.URL, "http://127.0.0.1:"), "%d", &port); err != nil {
		t.Fatalf("parse upstream port: %v", err)
	}
	return port
}

func proxyGet(t *testing.T, conn net.Conn, server *Server, url string) *http.Response {
	t.Helper()
	fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: example.test\r\nProxy-Authorization: %s\r\n\r\n", url, server.authHeader())
	req := &http.Request{Method: "GET"}
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	resp, err := http.ReadResponse(bufio.NewReader(conn), req)
	if err != nil {
		t.Fatalf("ReadResponse: %v", err)
	}
	return resp
}

func TestPlainHTTPForward(t *testing.T) {
	upstream, leaked := startHTTPUpstream(t)
	port := upstreamPort(t, upstream)
	server, logger := newTestServer(t, serverOpts{policy: mapPolicy{"example.test": allowExempt()}})
	conn := dialProxy(t, server)
	resp := proxyGet(t, conn, server, fmt.Sprintf("http://example.test:%d/ok", port))
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if string(body) != "hello" {
		t.Fatalf("body = %q, want hello", body)
	}
	if *leaked {
		t.Fatal("Proxy-Authorization leaked to upstream")
	}
	if rec := logger.last(); rec.Proto != "http" || !rec.Decision.Allow || rec.Dialed != "127.0.0.1" {
		t.Fatalf("record = %+v", rec)
	}
}

// TestPlainHTTPRedirectNotFollowed: an allow domain's 302 is handed to
// the client as-is; chasing it inside the proxy would skip the policy
// for the redirect target.
func TestPlainHTTPRedirectNotFollowed(t *testing.T) {
	upstream, _ := startHTTPUpstream(t)
	port := upstreamPort(t, upstream)
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{"example.test": allowExempt()}})
	conn := dialProxy(t, server)
	resp := proxyGet(t, conn, server, fmt.Sprintf("http://example.test:%d/redirect", port))
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusFound {
		t.Fatalf("status = %d, want 302 (redirect surfaced to client)", resp.StatusCode)
	}
	if loc := resp.Header.Get("Location"); loc != "http://denied.example/x" {
		t.Fatalf("Location = %q", loc)
	}
}

func TestPlainHTTPPolicyDeny(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{
		"evil.example": {Allow: false, Matched: true, Reason: "denied by user rule", Rule: "user"},
	}})
	conn := dialProxy(t, server)
	resp := proxyGet(t, conn, server, "http://evil.example/x")
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), "denied by user rule") {
		t.Fatalf("body = %q, want the rule reason", body)
	}
}

func TestURLHostFor(t *testing.T) {
	cases := []struct {
		host string
		port int
		want string
	}{
		{"example.test", 80, "example.test"},
		{"example.test", 8080, "example.test:8080"},
		{"::1", 80, "[::1]"}, // bare IPv6 would be an invalid URL.Host
		{"::1", 8080, "[::1]:8080"},
		{"127.0.0.1", 80, "127.0.0.1"},
	}
	for _, c := range cases {
		if got := urlHostFor(c.host, c.port); got != c.want {
			t.Fatalf("urlHostFor(%q, %d) = %q, want %q", c.host, c.port, got, c.want)
		}
	}
}

// TestPlainHTTPIPv6Literal: an IPv6 literal target survives the
// normalize → rebuild round-trip (upstream on [::1], exempted by an
// explicit allow rule).
func TestPlainHTTPIPv6Literal(t *testing.T) {
	ln, err := net.Listen("tcp", "[::1]:0")
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	upstream := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("v6-ok"))
	})}
	go func() { _ = upstream.Serve(ln) }()
	t.Cleanup(func() { _ = upstream.Close() })
	port := ln.Addr().(*net.TCPAddr).Port

	server, _ := newTestServer(t, serverOpts{policy: mapPolicy{"::1": allowExempt()}})
	conn := dialProxy(t, server)
	resp := proxyGet(t, conn, server, fmt.Sprintf("http://[::1]:%d/ok", port))
	defer func() { _ = resp.Body.Close() }()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK || string(body) != "v6-ok" {
		t.Fatalf("status = %d body = %q, want 200 v6-ok", resp.StatusCode, body)
	}
}

func TestPlainHTTPBadForms(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{})
	// origin-form is not forward-proxy usage.
	conn := dialProxy(t, server)
	fmt.Fprintf(conn, "GET /ok HTTP/1.1\r\nHost: example.test\r\nProxy-Authorization: %s\r\n\r\n", server.authHeader())
	if status := readStatusLine(t, conn); !strings.Contains(status, "400") {
		t.Fatalf("origin-form status = %q, want 400", status)
	}
	// absolute-form https belongs in CONNECT.
	conn2 := dialProxy(t, server)
	fmt.Fprintf(conn2, "GET https://example.test/x HTTP/1.1\r\nProxy-Authorization: %s\r\n\r\n", server.authHeader())
	if status := readStatusLine(t, conn2); !strings.Contains(status, "400") {
		t.Fatalf("https absolute-form status = %q, want 400", status)
	}
}
