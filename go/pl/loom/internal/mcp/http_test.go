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
// Created: 2026/08/03

package mcp

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// stubHTTPServer is a spec-shaped streamable HTTP MCP endpoint used to
// exercise the transport: it issues a session at initialize, enforces it
// with 404s, checks a bearer token, answers JSON or SSE per request, and
// records the headers every request carried.
type stubHTTPServer struct {
	mu             sync.Mutex
	token          string // required bearer token; empty disables auth
	sse            bool   // answer requests with text/event-stream
	interleaveNote bool   // prepend a notification inside SSE responses
	session        string
	expireSessions bool // treat every session'd request as unknown (404)
	// negotiatedVersion makes initialize answer with an older protocol
	// revision than the client's, exercising version negotiation.
	negotiatedVersion string
	// paginateTools splits tools/list into two nextCursor pages.
	paginateTools bool

	authHeaders      []string // Authorization header per POST
	sessionHeaders   []string // Mcp-Session-Id header per POST
	protocolHeaders  []string // MCP-Protocol-Version header per POST
	toolsListCursors []string // cursor param per tools/list request
	deletedSession   string   // session id carried by DELETE, if any
}

func (s *stubHTTPServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	if s.token != "" && r.Header.Get("Authorization") != "Bearer "+s.token {
		s.mu.Unlock()
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	s.mu.Unlock()

	switch r.Method {
	case http.MethodPost:
		s.handlePost(w, r)
	case http.MethodDelete:
		s.mu.Lock()
		s.deletedSession = r.Header.Get("Mcp-Session-Id")
		s.session = ""
		s.mu.Unlock()
		w.WriteHeader(http.StatusOK)
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *stubHTTPServer) handlePost(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.authHeaders = append(s.authHeaders, r.Header.Get("Authorization"))
	s.sessionHeaders = append(s.sessionHeaders, r.Header.Get("Mcp-Session-Id"))
	s.protocolHeaders = append(s.protocolHeaders, r.Header.Get("MCP-Protocol-Version"))
	session := s.session
	expire := s.expireSessions
	s.mu.Unlock()

	if session != "" && r.Header.Get("Mcp-Session-Id") != session || expire && r.Header.Get("Mcp-Session-Id") != "" {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "read failed", http.StatusBadRequest)
		return
	}
	var req struct {
		ID     *int64          `json:"id,omitempty"`
		Method string          `json:"method"`
		Params json.RawMessage `json:"params,omitempty"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, "malformed", http.StatusBadRequest)
		return
	}
	if req.ID == nil { // notification
		w.WriteHeader(http.StatusAccepted)
		return
	}

	var result any
	switch req.Method {
	case "initialize":
		s.mu.Lock()
		s.session = "stub-session-1"
		version := s.negotiatedVersion
		s.mu.Unlock()
		if version == "" {
			version = protocolVersion
		}
		w.Header().Set("Mcp-Session-Id", "stub-session-1")
		result = map[string]any{
			"protocolVersion": version,
			"serverInfo":      map[string]any{"name": "stub", "version": "1.0"},
			"capabilities":    map[string]any{},
		}
	case "tools/list":
		var params struct {
			Cursor string `json:"cursor"`
		}
		_ = json.Unmarshal(req.Params, &params)
		s.mu.Lock()
		s.toolsListCursors = append(s.toolsListCursors, params.Cursor)
		paginate := s.paginateTools
		s.mu.Unlock()
		tool := func(name string) map[string]any {
			return map[string]any{
				"name":        name,
				"description": name + " back",
				"inputSchema": map[string]any{"type": "object", "properties": map[string]any{}},
			}
		}
		switch {
		case paginate && params.Cursor == "":
			result = map[string]any{"tools": []map[string]any{tool("echo")}, "nextCursor": "page2"}
		case paginate:
			result = map[string]any{"tools": []map[string]any{tool("echo2")}}
		default:
			result = map[string]any{"tools": []map[string]any{tool("echo")}}
		}
	case "tools/call":
		result = map[string]any{"content": []map[string]any{{"type": "text", "text": "pong"}}}
	default:
		result = map[string]any{}
	}
	resp := map[string]any{"jsonrpc": "2.0", "id": *req.ID, "result": result}
	data, _ := json.Marshal(resp)

	s.mu.Lock()
	sse := s.sse
	interleave := s.interleaveNote
	s.mu.Unlock()
	if sse {
		w.Header().Set("Content-Type", "text/event-stream")
		if interleave {
			note, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "method": "notifications/progress"})
			fmt.Fprintf(w, "event: message\ndata: %s\n\n", note)
		}
		fmt.Fprintf(w, "event: message\ndata: %s\n\n", data)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(data)
}

func startStub(t *testing.T, configure func(*stubHTTPServer)) (*stubHTTPServer, string) {
	t.Helper()
	stub := &stubHTTPServer{}
	if configure != nil {
		configure(stub)
	}
	srv := httptest.NewServer(stub)
	t.Cleanup(srv.Close)
	return stub, srv.URL
}

func startHTTPClient(t *testing.T, url string, headers map[string]string) *Client {
	t.Helper()
	client, err := Start(context.Background(), ClientConfig{
		URL:            url,
		Headers:        headers,
		StartupTimeout: 10 * time.Second,
		ToolTimeout:    10 * time.Second,
	})
	if err != nil {
		t.Fatalf("Start() error = %v", err)
	}
	t.Cleanup(func() { _ = client.Close() })
	return client
}

// TestHTTPClientSessionLifecycle covers the happy path: initialize over
// POST, session id capture and replay, the protocol-version header,
// tools/list + tools/call, and the session-terminating DELETE on Close.
func TestHTTPClientSessionLifecycle(t *testing.T) {
	stub, url := startStub(t, func(s *stubHTTPServer) { s.token = "sekret" })
	headers := map[string]string{"Authorization": "Bearer sekret"}

	client, err := Start(context.Background(), ClientConfig{
		URL:            url,
		Headers:        headers,
		StartupTimeout: 10 * time.Second,
		ToolTimeout:    10 * time.Second,
	})
	if err != nil {
		t.Fatalf("Start() error = %v", err)
	}

	specs, err := client.ListTools(context.Background())
	if err != nil {
		t.Fatalf("ListTools() error = %v", err)
	}
	if len(specs) != 1 || specs[0].Name != "echo" {
		t.Fatalf("ListTools() = %+v, want one echo tool", specs)
	}

	result, err := client.CallTool(context.Background(), "echo", json.RawMessage(`{"text":"hi"}`))
	if err != nil {
		t.Fatalf("CallTool() error = %v", err)
	}
	if len(result.Content) != 1 || result.Content[0].Text != "pong" {
		t.Fatalf("CallTool() content = %+v, want pong", result.Content)
	}

	name, version := client.ServerInfo()
	if name != "stub" || version != "1.0" {
		t.Fatalf("ServerInfo() = (%q, %q), want (stub, 1.0)", name, version)
	}

	if err := client.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	stub.mu.Lock()
	defer stub.mu.Unlock()
	// initialize, initialized notification, tools/list, tools/call.
	if len(stub.authHeaders) != 4 {
		t.Fatalf("POST count = %d, want 4", len(stub.authHeaders))
	}
	for i, got := range stub.authHeaders {
		if got != "Bearer sekret" {
			t.Fatalf("authHeaders[%d] = %q, want Bearer sekret", i, got)
		}
	}
	// The first POST (initialize) predates the session; everything after
	// must replay it, alongside the protocol-version header.
	if stub.sessionHeaders[0] != "" {
		t.Fatalf("initialize carried session %q, want none", stub.sessionHeaders[0])
	}
	for i := 1; i < len(stub.sessionHeaders); i++ {
		if stub.sessionHeaders[i] != "stub-session-1" {
			t.Fatalf("sessionHeaders[%d] = %q, want stub-session-1", i, stub.sessionHeaders[i])
		}
		if stub.protocolHeaders[i] != protocolVersion {
			t.Fatalf("protocolHeaders[%d] = %q, want %s", i, stub.protocolHeaders[i], protocolVersion)
		}
	}
	if stub.deletedSession != "stub-session-1" {
		t.Fatalf("DELETE carried session %q, want stub-session-1", stub.deletedSession)
	}
}

// TestHTTPClientSSEResponses verifies the event-stream response shape,
// including skipping an interleaved notification before the response.
func TestHTTPClientSSEResponses(t *testing.T) {
	_, url := startStub(t, func(s *stubHTTPServer) {
		s.sse = true
		s.interleaveNote = true
	})
	client := startHTTPClient(t, url, nil)

	specs, err := client.ListTools(context.Background())
	if err != nil {
		t.Fatalf("ListTools() over SSE error = %v", err)
	}
	if len(specs) != 1 || specs[0].Name != "echo" {
		t.Fatalf("ListTools() = %+v, want one echo tool", specs)
	}
	result, err := client.CallTool(context.Background(), "echo", nil)
	if err != nil {
		t.Fatalf("CallTool() over SSE error = %v", err)
	}
	if len(result.Content) != 1 || result.Content[0].Text != "pong" {
		t.Fatalf("CallTool() content = %+v, want pong", result.Content)
	}
}

// A server negotiating an older protocol revision gets that revision
// replayed on every post-initialize request (REVIEW M23).
func TestHTTPClientAdoptsNegotiatedProtocolVersion(t *testing.T) {
	stub, url := startStub(t, func(s *stubHTTPServer) { s.negotiatedVersion = "2025-03-26" })
	client := startHTTPClient(t, url, nil)

	if _, err := client.ListTools(context.Background()); err != nil {
		t.Fatalf("ListTools() error = %v", err)
	}

	stub.mu.Lock()
	defer stub.mu.Unlock()
	// POSTs: initialize, initialized notification, tools/list — everything
	// after initialize must carry the negotiated revision, not the offered one.
	for i := 1; i < len(stub.protocolHeaders); i++ {
		if stub.protocolHeaders[i] != "2025-03-26" {
			t.Fatalf("protocolHeaders[%d] = %q, want negotiated 2025-03-26", i, stub.protocolHeaders[i])
		}
	}
}

// tools/list must follow nextCursor until pagination is exhausted
// (REVIEW M23).
func TestHTTPClientListToolsPagination(t *testing.T) {
	stub, url := startStub(t, func(s *stubHTTPServer) { s.paginateTools = true })
	client := startHTTPClient(t, url, nil)

	specs, err := client.ListTools(context.Background())
	if err != nil {
		t.Fatalf("ListTools() error = %v", err)
	}
	if len(specs) != 2 || specs[0].Name != "echo" || specs[1].Name != "echo2" {
		t.Fatalf("ListTools() = %+v, want both pages (echo, echo2)", specs)
	}

	stub.mu.Lock()
	defer stub.mu.Unlock()
	if len(stub.toolsListCursors) != 2 || stub.toolsListCursors[0] != "" || stub.toolsListCursors[1] != "page2" {
		t.Fatalf("tools/list cursors = %v, want [\"\" \"page2\"]", stub.toolsListCursors)
	}
}

// TestHTTPClientAuthRejected surfaces a 401 during the handshake.
func TestHTTPClientAuthRejected(t *testing.T) {
	_, url := startStub(t, func(s *stubHTTPServer) { s.token = "sekret" })
	_, err := Start(context.Background(), ClientConfig{URL: url, StartupTimeout: 5 * time.Second})
	if err == nil || !strings.Contains(err.Error(), "401") {
		t.Fatalf("Start() error = %v, want a 401 mention", err)
	}
}

// TestHTTPClientSessionExpired maps a post-handshake 404 onto a clear
// "session lost" error instead of a generic HTTP failure.
func TestHTTPClientSessionExpired(t *testing.T) {
	stub, url := startStub(t, nil)
	client := startHTTPClient(t, url, nil)

	stub.mu.Lock()
	stub.expireSessions = true
	stub.mu.Unlock()

	_, err := client.CallTool(context.Background(), "echo", nil)
	if err == nil || !strings.Contains(err.Error(), "session") {
		t.Fatalf("CallTool() error = %v, want a session-expiry mention", err)
	}
}

// TestStartConfigValidation pins the transport-selection rules on
// ClientConfig: exactly one of command/url, http(s) scheme for url.
func TestStartConfigValidation(t *testing.T) {
	tests := []struct {
		name    string
		cfg     ClientConfig
		wantErr string
	}{
		{"neither command nor url", ClientConfig{}, "exactly one of command or url"},
		{"both command and url", ClientConfig{Command: "/bin/true", URL: "http://x"}, "exactly one of command or url"},
		{"non-http url", ClientConfig{URL: "ftp://example.com/mcp"}, "invalid server url"},
		{"url without host", ClientConfig{URL: "http://"}, "invalid server url"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := Start(context.Background(), tt.cfg)
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("Start() error = %v, want %q", err, tt.wantErr)
			}
		})
	}
}
