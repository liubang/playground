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
// Created: 2026/08/09

package server

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"sync"
	"time"
)

// ShareEndpointState is the runtime state of the LAN share listener,
// reported by GET /v1/share/endpoint. Error carries the last bind/start
// failure (the listener stays down until the next Apply).
type ShareEndpointState struct {
	Enabled bool   `json:"enabled"`
	Listen  string `json:"listen"`
	URL     string `json:"url,omitempty"`
	Error   string `json:"error,omitempty"`
}

// ShareManager owns the optional LAN share listener (docs/DESKTOP_DESIGN.md
// §5): a minimal, share-only Server started and stopped at runtime —
// from the /v1/share/endpoint API and from config hot-apply. The bind
// address comes from the share.listen config key: a fixed port lets share
// links survive restarts (share tokens are persisted in the session
// store).
type ShareManager struct {
	// newSrv builds the share-only server for one bind address. It is a
	// factory (not a stored server) because the server must be rebuilt
	// when the address changes.
	newSrv func(listen string) (*Server, error)
	logger *slog.Logger

	mu      sync.Mutex
	srv     *Server
	listen  string
	public  string
	lastErr string
}

// NewShareManager creates a manager around the share-server factory; nil
// logger discards. Nothing listens until Apply enables the endpoint.
func NewShareManager(newSrv func(listen string) (*Server, error), logger *slog.Logger) *ShareManager {
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	return &ShareManager{newSrv: newSrv, logger: logger}
}

// Apply reconciles the listener to the desired state: enabled → serve on
// listen (restarting when the address changed); disabled → stop. A start
// failure is recorded in the state and returned, but the manager stays
// usable — the next Apply retries from scratch.
func (m *ShareManager) Apply(enabled bool, listen string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if !enabled {
		m.stopLocked()
		m.listen = listen
		m.lastErr = ""
		return nil
	}
	if m.srv != nil && m.listen == listen {
		return nil // already serving the desired address
	}
	m.stopLocked()
	srv, err := m.newSrv(listen)
	if err != nil {
		m.lastErr = err.Error()
		return err
	}
	if err := srv.Listen(); err != nil {
		m.lastErr = err.Error()
		return fmt.Errorf("share listen %s: %w", listen, err)
	}
	base, err := publicBaseFor(srv.Addr())
	if err != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
		m.lastErr = err.Error()
		return err
	}
	m.srv = srv
	m.listen = listen
	m.public = base
	m.lastErr = ""
	go func() {
		if err := srv.Serve(); err != nil {
			m.logger.Error("share listener died", "error", err)
		}
	}()
	m.logger.Info("loom share endpoint", "base", base)
	return nil
}

// State reports the current listener state.
func (m *ShareManager) State() ShareEndpointState {
	m.mu.Lock()
	defer m.mu.Unlock()
	return ShareEndpointState{
		Enabled: m.srv != nil,
		Listen:  m.listen,
		URL:     m.public,
		Error:   m.lastErr,
	}
}

// PublicBase is the externally reachable base URL of the share listener
// ("" while it is down). Share minting embeds it so links handed out
// from the loopback UI are reachable beyond this machine.
func (m *ShareManager) PublicBase() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.public
}

// Close stops the listener (process shutdown path); idempotent.
func (m *ShareManager) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.stopLocked()
}

func (m *ShareManager) stopLocked() {
	if m.srv == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := m.srv.Shutdown(ctx); err != nil {
		m.logger.Warn("share listener shutdown", "error", err)
	}
	m.srv = nil
	m.public = ""
}

// publicBaseFor resolves the externally reachable base URL for share
// links from the bound address: loopback → the loopback URL (loopback-only
// sharing), unspecified (0.0.0.0/::) → the outbound interface address, a
// specific address → used as-is.
func publicBaseFor(boundAddr string) (string, error) {
	host, port, err := net.SplitHostPort(boundAddr)
	if err != nil {
		return "", fmt.Errorf("parse bound address %q: %w", boundAddr, err)
	}
	if ip := net.ParseIP(host); ip != nil {
		switch {
		case ip.IsLoopback():
			return "http://127.0.0.1:" + port, nil
		case ip.IsUnspecified():
			out := outboundIP()
			if out == "" {
				return "", errors.New("cannot determine the LAN address of this machine; bind a specific interface in share.listen")
			}
			return "http://" + out + ":" + port, nil
		default:
			return "http://" + host + ":" + port, nil
		}
	}
	if host == "localhost" {
		return "http://127.0.0.1:" + port, nil
	}
	return "http://" + host + ":" + port, nil
}

// outboundIP finds the preferred outbound IPv4 without sending traffic: a
// UDP "dial" to a documentation prefix only performs a routing-table
// lookup.
func outboundIP() string {
	conn, err := net.Dial("udp4", "192.0.2.1:80")
	if err != nil {
		return ""
	}
	defer conn.Close()
	addr, ok := conn.LocalAddr().(*net.UDPAddr)
	if !ok {
		return ""
	}
	return addr.IP.String()
}
