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
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/textproto"
	"strings"
	"time"
)

// errHeaderTooLarge is produced by the capped header reader once the
// request head exceeds maxHeaderBytes (http.ReadRequest and the
// textproto CONNECT parse have no built-in limit).
var errHeaderTooLarge = errors.New("request header too large")

// cappedReader limits bytes read during the header phase. uncap()
// switches it to passthrough for the tunnel phase, so the connection's
// single bufio.Reader keeps working — buffered bytes (a TLS ClientHello
// that arrived with the CONNECT) are never stranded, and the cap never
// cuts tunnel traffic.
type cappedReader struct {
	r      io.Reader
	left   int
	capped bool
}

func newCappedReader(r io.Reader, limit int) *cappedReader {
	return &cappedReader{r: r, left: limit, capped: true}
}

func (c *cappedReader) Read(p []byte) (int, error) {
	if !c.capped {
		return c.r.Read(p)
	}
	if c.left <= 0 {
		return 0, errHeaderTooLarge
	}
	if len(p) > c.left {
		p = p[:c.left]
	}
	n, err := c.r.Read(p)
	c.left -= n
	return n, err
}

func (c *cappedReader) uncap() { c.capped = false }

// serveHTTP handles the HTTP leg: CONNECT (manually parsed — ReadRequest
// mangles CONNECT authorities) or plain-HTTP absolute-URI forwarding.
func (s *Server) serveHTTP(conn net.Conn, br *bufio.Reader, capped *cappedReader) {
	if isConnect(br) {
		s.serveCONNECT(conn, br, capped)
		return
	}
	s.servePlainHTTP(conn, br)
}

// isConnect peeks the request line's method token.
func isConnect(br *bufio.Reader) bool {
	const prefix = "CONNECT "
	head, err := br.Peek(len(prefix))
	return err == nil && string(head) == prefix
}

// serveCONNECT implements one CONNECT exchange: parse, authenticate,
// decide, guard, dial, then hand the connection to the tunnel.
func (s *Server) serveCONNECT(conn net.Conn, br *bufio.Reader, capped *cappedReader) {
	tp := textproto.NewReader(br)
	line, err := tp.ReadLine()
	if err != nil {
		s.writeHeaderError(conn, err)
		return
	}
	parts := strings.SplitN(line, " ", 3)
	if len(parts) != 3 || parts[0] != "CONNECT" || !strings.HasPrefix(parts[2], "HTTP/1.") {
		writeStatusLine(conn, "400 Bad Request")
		return
	}
	mime, err := tp.ReadMIMEHeader()
	if err != nil {
		s.writeHeaderError(conn, err)
		return
	}
	if !s.checkAuth(conn, mime.Get("Proxy-Authorization")) {
		return
	}
	host, port, err := parseAuthority(parts[1], 443)
	if err != nil {
		writeStatusLine(conn, "400 Bad Request")
		return
	}
	s.decideAndTunnel(conn, br, capped, "connect", host, port)
}

// servePlainHTTP forwards one absolute-URI request upstream. The
// connection is closed after the response (one request per proxied
// connection keeps the state machine trivial; keep-alive is a v2 item).
func (s *Server) servePlainHTTP(conn net.Conn, br *bufio.Reader) {
	req, err := http.ReadRequest(br)
	if err != nil {
		s.writeHeaderError(conn, err)
		return
	}
	defer func() { _ = req.Body.Close() }()
	if !s.checkAuth(conn, req.Header.Get("Proxy-Authorization")) {
		return
	}
	if !req.URL.IsAbs() || req.URL.Scheme != "http" {
		// origin-form is not forward-proxy usage; absolute-form https
		// would be a downgrade ambiguity (TLS belongs in CONNECT).
		writeStatusLine(conn, "400 Bad Request")
		return
	}
	host, port, err := parseAuthority(req.URL.Host, 80)
	if err != nil {
		writeStatusLine(conn, "400 Bad Request")
		return
	}
	rec := ConnRecord{Time: time.Now(), Proto: "http", Host: host, Port: port}
	dec := s.policy.Decide(host, port)
	rec.Decision = dec
	if !dec.Allow {
		writeStatusLineWithBody(conn, "403 Forbidden", "blocked by loom egress policy: "+dec.Reason)
		s.log(rec)
		return
	}
	// The upstream leg MUST NOT follow redirects: a redirect target is a
	// new policy question, and the client re-issues it as a fresh proxy
	// request that goes through Decide again. Transport.RoundTrip has
	// exactly that semantics; http.Client would silently chase an allow
	// domain's 302 into a denied exfil channel.
	var dialed string
	transport := &http.Transport{
		DisableCompression: true, // forward bytes as upstream sent them
		DisableKeepAlives:  true, // one RoundTrip per transport
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			upstream, addr, err := s.guard.dialGuarded(ctx, dec, host, port)
			dialed = addr
			return upstream, err
		},
	}
	defer transport.CloseIdleConnections()
	// Rebuild the request from the normalized spelling; never replay the
	// client's raw URL/Host (parser-differential defense).
	req.URL.Host = urlHostFor(host, port)
	req.Host = ""
	req.RequestURI = "" // RoundTrip rejects a non-empty RequestURI
	stripHopByHop(req.Header)
	req.Close = true
	resp, err := transport.RoundTrip(req)
	rec.Dialed = dialed
	if err != nil {
		rec.Err = err.Error()
		writeStatusLineWithBody(conn, statusFor(err), "loom egress upstream error: "+rec.Err)
		s.log(rec)
		return
	}
	defer func() { _ = resp.Body.Close() }()
	stripHopByHop(resp.Header)
	resp.Close = true // emit Connection: close; we do not keep the client conn
	if err := resp.Write(conn); err != nil {
		rec.Err = err.Error()
	}
	s.log(rec)
}

// decideAndTunnel is the shared CONNECT/SOCKS5 tail: policy decision,
// guarded dial, success reply, then the relay. On success it returns
// only when the tunnel closes; the caller's defer closes conn.
func (s *Server) decideAndTunnel(conn net.Conn, br *bufio.Reader, capped *cappedReader, proto, host string, port int) {
	rec := ConnRecord{Time: time.Now(), Proto: proto, Host: host, Port: port}
	policy := s.policy
	if proto == "socks5" {
		policy = s.socksPolicy
	}
	dec := policy.Decide(host, port)
	rec.Decision = dec
	if !dec.Allow {
		s.writeRefusal(conn, proto, "403 Forbidden", "blocked by loom egress policy: "+dec.Reason)
		s.log(rec)
		return
	}
	upstream, dialed, err := s.guard.dialGuarded(context.Background(), dec, host, port)
	if err != nil {
		rec.Err = err.Error()
		s.writeRefusal(conn, proto, statusFor(err), err.Error())
		s.log(rec)
		return
	}
	rec.Dialed = dialed
	s.log(rec)
	if proto == "connect" {
		if _, err := io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
			_ = upstream.Close()
			return
		}
	} else {
		if err := writeSOCKSReply(conn, socksRepSucceeded); err != nil {
			_ = upstream.Close()
			return
		}
	}
	// Tunnel established: the handshake deadline and the header cap must
	// not apply to long-lived tunnels (SSE/websockets are legitimate).
	_ = conn.SetReadDeadline(time.Time{})
	capped.uncap()
	relay(conn, br, upstream)
}

// writeRefusal maps a refusal onto the wire shape of the protocol.
func (s *Server) writeRefusal(conn net.Conn, proto, status, message string) {
	if proto == "socks5" {
		if status == "403 Forbidden" {
			_ = writeSOCKSReply(conn, socksRepNotAllowed)
			return
		}
		_ = writeSOCKSReply(conn, socksRepFailure)
		return
	}
	writeStatusLineWithBody(conn, status, message)
}

// statusFor classifies an upstream failure into 403 (policy/guard
// refusal) or 502 (resolution/dial failure).
func statusFor(err error) string {
	var denied *guardDeniedError
	if errors.As(err, &denied) || errors.Is(err, errTargetIsProxy) {
		return "403 Forbidden"
	}
	return "502 Bad Gateway"
}

// checkAuth validates Proxy-Authorization against the session token and
// writes the 407 (with Proxy-Authenticate, or clients never retry with
// credentials) on failure. Only the password half is compared; the
// username is reserved for future per-command attribution.
func (s *Server) checkAuth(conn net.Conn, header string) bool {
	ok := false
	if scheme, credentials, found := strings.Cut(header, " "); found && strings.EqualFold(scheme, "basic") {
		if decoded, err := base64.StdEncoding.DecodeString(strings.TrimSpace(credentials)); err == nil {
			if user, pass, found := strings.Cut(string(decoded), ":"); found && user != "" {
				ok = subtle.ConstantTimeCompare([]byte(pass), []byte(s.token)) == 1
			}
		}
	}
	if !ok {
		_, _ = io.WriteString(conn, "HTTP/1.1 407 Proxy Authentication Required\r\n"+
			`Proxy-Authenticate: Basic realm="loom-egress"`+"\r\n"+
			"Content-Length: 0\r\n\r\n")
	}
	return ok
}

// urlHostFor rebuilds a request URL.Host from the normalized host and
// port: the :port suffix only for non-default ports, and square
// brackets around IPv6 literals (net.JoinHostPort brackets on its own,
// but the bare-host branch must add them by hand).
func urlHostFor(host string, port int) string {
	if port != 80 {
		return net.JoinHostPort(host, fmt.Sprint(port))
	}
	if strings.Contains(host, ":") {
		return "[" + host + "]"
	}
	return host
}

// writeHeaderError answers a malformed/oversized header read.
func (s *Server) writeHeaderError(conn net.Conn, err error) {
	if errors.Is(err, errHeaderTooLarge) {
		writeStatusLine(conn, "431 Request Header Fields Too Large")
		return
	}
	writeStatusLine(conn, "400 Bad Request")
}

func writeStatusLine(conn net.Conn, status string) {
	_, _ = io.WriteString(conn, "HTTP/1.1 "+status+"\r\nContent-Length: 0\r\n\r\n")
}

func writeStatusLineWithBody(conn net.Conn, status, body string) {
	_, _ = io.WriteString(conn, fmt.Sprintf("HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\n\r\n%s",
		status, len(body), body))
}

// hopByHopHeaders are stripped from forwarded requests AND responses,
// alongside anything the Connection header nominates.
var hopByHopHeaders = []string{
	"Connection",
	"Proxy-Connection",
	"Proxy-Authorization", // end-to-end for the proxy, never for upstream
	"Keep-Alive",
	"TE",
	"Trailer",
	"Transfer-Encoding",
	"Upgrade",
}

func stripHopByHop(h http.Header) {
	for _, nominated := range h["Connection"] {
		for field := range strings.SplitSeq(nominated, ",") {
			h.Del(strings.TrimSpace(field))
		}
	}
	for _, name := range hopByHopHeaders {
		h.Del(name)
	}
}

// relay copies bytes in both directions with half-close propagation:
// one side's EOF closes the opposite write direction and lets the other
// direction drain. The client source is the connection's single
// bufio.Reader so bytes buffered during the header phase (a pipelined
// first flight) are forwarded, not stranded.
func relay(client net.Conn, clientSrc *bufio.Reader, upstream net.Conn) {
	defer func() { _ = upstream.Close() }()
	done := make(chan struct{}, 2)
	go func() {
		_, _ = io.Copy(upstream, clientSrc)
		closeWrite(upstream)
		done <- struct{}{}
	}()
	go func() {
		_, _ = io.Copy(client, upstream)
		closeWrite(client)
		done <- struct{}{}
	}()
	<-done
	<-done
}

func closeWrite(conn net.Conn) {
	if tc, ok := conn.(*net.TCPConn); ok {
		_ = tc.CloseWrite()
		return
	}
	_ = conn.Close()
}
