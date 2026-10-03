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

// Package egress implements loom's egress proxy: a loopback-only,
// policy-filtered forward proxy that sandboxed commands reach via the
// standard proxy environment variables.
// The seatbelt sandbox stays the boundary — direct outbound connections
// from the sandbox remain denied — while every proxied connection is
// decided by a domain Policy, hardened by the resolved-address guard,
// and recorded through the Logger hook.
package egress

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"sync"
	"time"
)

const (
	// defaultMaxConns caps concurrent connections (tunnels included).
	// The proxy lives inside the loom host process: an unbounded fd /
	// goroutine footprint would let a sandboxed command DoS all of loom.
	defaultMaxConns = 512
	// defaultHandshakeTimeout bounds the pre-tunnel phase (first byte,
	// header read, policy decision) from accept(2) onward, so silent or
	// trickling clients cannot hold descriptors.
	defaultHandshakeTimeout = 10 * time.Second
	// dialTimeout bounds a single upstream dial attempt.
	dialTimeout = 10 * time.Second
	// maxHeaderBytes caps the request header block (http.ReadRequest and
	// the textproto CONNECT parse have no built-in limit).
	maxHeaderBytes = 256 << 10
	// closeDrainTimeout is how long Close waits for active tunnels to
	// finish before force-closing them.
	closeDrainTimeout = 5 * time.Second

	tokenBytes = 32
)

// Decision is the policy verdict for one connection. It carries
// everything the resolved-address guard needs, snapshotted under one
// lock at decision time, so a ruleset mutation mid-connection never
// makes the guard disagree with the decision.
type Decision struct {
	Allow   bool
	Matched bool   // an explicit rule matched (not the unmatched default)
	Reason  string // human-readable provenance (rule justification or default posture)
	Rule    string // "builtin" | "user" | "project" | "session" | "unmatched-allow" | "unmatched-deny"
	// ExemptLiterals are the IP literals named by explicit allow rules in
	// the same snapshot (host rules carry no port dimension). The guard
	// exempts exactly these addresses from its denied classes.
	ExemptLiterals []netip.Addr
}

// Policy decides whether a connection to host:port may proceed. host is
// the normalized spelling produced by the proxy (lowercased, brackets
// and trailing dot stripped). Implementations must be safe for
// concurrent use.
type Policy interface {
	Decide(host string, port int) Decision
}

// ConnRecord describes one connection attempt, allowed or not.
type ConnRecord struct {
	Time     time.Time
	Proto    string // "connect" | "http" | "socks5"
	Host     string
	Port     int
	Decision Decision
	Dialed   string // the IP actually dialed (empty on refusal/failure)
	Err      string // resolution/dial/forward error (empty on success)
}

// Logger receives one ConnRecord per connection attempt. It must not
// block; a nil Logger drops records (not recommended — visibility is
// the point of the proxy).
type Logger interface {
	LogConn(ConnRecord)
}

// Config configures a Server.
type Config struct {
	Policy Policy
	// SOCKSPolicy decides SOCKS5 connections, which arrive unauthenticated
	// (ProxyCommand clients like `nc -X 5` cannot authenticate). Nil falls
	// back to Policy. The app layer wires a conservative adapter here:
	// only global-scope explicit allow rules, never the unmatched default.
	SOCKSPolicy Policy
	Logger      Logger
	// Token authenticates HTTP proxy requests; empty generates a random
	// one (crypto/rand, 32 bytes hex).
	Token string
	// MaxConns caps concurrent connections; 0 selects defaultMaxConns.
	MaxConns int
	// HandshakeTimeout bounds the pre-tunnel phase per connection;
	// 0 selects defaultHandshakeTimeout. Exposed for tests.
	HandshakeTimeout time.Duration
	// LookupIP resolves a hostname to addresses (resolved-address guard
	// seam). Nil uses net.DefaultResolver.
	LookupIP func(ctx context.Context, host string) ([]netip.Addr, error)
	// Dial dials one resolved address (test seam). Nil uses net.Dialer
	// with dialTimeout.
	Dial func(ctx context.Context, addr netip.AddrPort) (net.Conn, error)
	// LocalAddrs returns this host's interface addresses, read fresh on
	// every resolution (test seam). Nil uses net.InterfaceAddrs.
	LocalAddrs func() ([]netip.Addr, error)
}

// Server is the egress proxy: one muxed loopback listener serving HTTP
// CONNECT, plain-HTTP forwarding, and SOCKS5.
type Server struct {
	ln          net.Listener
	token       string
	policy      Policy
	socksPolicy Policy
	logger      Logger
	guard       *addressGuard
	handshake   time.Duration

	sem    chan struct{} // connection accounting (MaxConns)
	mu     sync.Mutex
	conns  map[net.Conn]struct{}
	wg     sync.WaitGroup
	closed bool
}

// NewServer starts an egress proxy on 127.0.0.1 with a random port.
func NewServer(cfg Config) (*Server, error) {
	if cfg.Policy == nil {
		return nil, errors.New("egress: policy is required")
	}
	token := cfg.Token
	if token == "" {
		raw := make([]byte, tokenBytes)
		if _, err := rand.Read(raw); err != nil {
			return nil, fmt.Errorf("egress: generate token: %w", err)
		}
		token = hex.EncodeToString(raw)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, fmt.Errorf("egress: listen: %w", err)
	}
	maxConns := cfg.MaxConns
	if maxConns <= 0 {
		maxConns = defaultMaxConns
	}
	handshake := cfg.HandshakeTimeout
	if handshake <= 0 {
		handshake = defaultHandshakeTimeout
	}
	socksPolicy := cfg.SOCKSPolicy
	if socksPolicy == nil {
		socksPolicy = cfg.Policy
	}
	s := &Server{
		ln:          ln,
		token:       token,
		policy:      cfg.Policy,
		socksPolicy: socksPolicy,
		logger:      cfg.Logger,
		guard:       newAddressGuard(cfg),
		handshake:   handshake,
		sem:         make(chan struct{}, maxConns),
		conns:       make(map[net.Conn]struct{}),
	}
	// The listener binds :0, so the guard learns the port (for the
	// self-connect refusal) only after NewServer has it.
	s.guard.setSelfPort(s.Port())
	go s.acceptLoop()
	return s, nil
}

// Addr returns the listener address ("127.0.0.1:<port>").
func (s *Server) Addr() string { return s.ln.Addr().String() }

// Port returns the listener port.
func (s *Server) Port() int { return s.ln.Addr().(*net.TCPAddr).Port }

// Token returns the HTTP proxy auth token.
func (s *Server) Token() string { return s.token }

// ProxyURL returns the URL injected into sandboxed commands' proxy
// environment variables, token embedded in the userinfo.
func (s *Server) ProxyURL() string {
	return fmt.Sprintf("http://loom:%s@%s", s.token, s.Addr())
}

// Close stops accepting, waits up to closeDrainTimeout for active
// tunnels to drain, then force-closes the remainder.
func (s *Server) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	s.mu.Unlock()
	err := s.ln.Close()
	if s.waitDrain(closeDrainTimeout) {
		return err
	}
	s.mu.Lock()
	for conn := range s.conns {
		_ = conn.Close()
	}
	s.mu.Unlock()
	s.wg.Wait()
	return err
}

func (s *Server) waitDrain(d time.Duration) bool {
	done := make(chan struct{})
	go func() {
		s.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
		return true
	case <-time.After(d):
		return false
	}
}

func (s *Server) acceptLoop() {
	for {
		conn, err := s.ln.Accept()
		if err != nil {
			return // listener closed
		}
		select {
		case s.sem <- struct{}{}:
		default:
			// Over capacity: refuse cheaply rather than queue a fd the
			// sandbox can hold open forever.
			_ = conn.Close()
			continue
		}
		s.mu.Lock()
		s.conns[conn] = struct{}{}
		s.mu.Unlock()
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			defer func() { <-s.sem }()
			defer func() {
				s.mu.Lock()
				delete(s.conns, conn)
				s.mu.Unlock()
			}()
			s.serve(conn)
		}()
	}
}

// serve sniffs the protocol and dispatches. The deadline armed here
// covers the whole pre-tunnel phase; tunnel establishment clears it.
// The bufio.Reader and the cappedReader beneath it live for the whole
// connection: sniffing peeks without consuming, header parsing is
// byte-capped, and the tunnel phase reads through the same Reader so
// buffered bytes are never stranded.
func (s *Server) serve(conn net.Conn) {
	defer func() { _ = conn.Close() }()
	_ = conn.SetReadDeadline(time.Now().Add(s.handshake))
	capped := newCappedReader(conn, maxHeaderBytes)
	br := bufio.NewReader(capped)
	first, err := br.Peek(1)
	if err != nil {
		return // silent client, EOF, or timeout — nothing to answer
	}
	switch {
	case first[0] == socks5Version:
		s.serveSOCKS(conn, br, capped)
	case first[0] >= 'A' && first[0] <= 'Z':
		s.serveHTTP(conn, br, capped)
	default:
		writeStatusLine(conn, "400 Bad Request")
	}
}

// log records one connection attempt; a nil logger drops it.
func (s *Server) log(rec ConnRecord) {
	if s.logger != nil {
		s.logger.LogConn(rec)
	}
}
