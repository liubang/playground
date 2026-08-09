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

package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// mcp_http_e2e_test.go verifies the streamable HTTP MCP transport end to
// end: a real HTTP MCP server (httptest), the real config loader, the
// real Bootstrap assembly, and the real tool registry — the same code
// path a user hits with a url-based mcp_servers entry.

// -----------------------------------------------------------------------
// Streamable HTTP MCP echo server (httptest, in-process)
// -----------------------------------------------------------------------

// httpEchoServer speaks the streamable HTTP transport (spec 2025-06-18):
// bearer auth, a session issued at initialize and enforced with 404,
// notifications answered with 202, and requests answered with either a
// bare JSON body or an SSE frame. It records the headers every POST
// carried so tests can assert what the client sent.
type httpEchoServer struct {
	mu      sync.Mutex
	token   string
	sse     bool
	session string

	authHeaders    []string
	sessionHeaders []string
}

func (s *httpEchoServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if s.token != "" && r.Header.Get("Authorization") != "Bearer "+s.token {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	s.mu.Lock()
	s.authHeaders = append(s.authHeaders, r.Header.Get("Authorization"))
	s.sessionHeaders = append(s.sessionHeaders, r.Header.Get("Mcp-Session-Id"))
	session := s.session
	s.mu.Unlock()
	if session != "" && r.Header.Get("Mcp-Session-Id") != session {
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
		s.session = "e2e-session"
		s.mu.Unlock()
		w.Header().Set("Mcp-Session-Id", "e2e-session")
		result = map[string]any{
			"protocolVersion": "2025-06-18",
			"serverInfo":      map[string]any{"name": "http-echo", "version": "0.1"},
			"capabilities":    map[string]any{},
		}
	case "tools/list":
		result = map[string]any{"tools": []map[string]any{{
			"name":        "echo",
			"description": "Echo the input text back",
			"inputSchema": map[string]any{
				"type":       "object",
				"properties": map[string]any{"text": map[string]any{"type": "string"}},
			},
		}}}
	case "tools/call":
		var params struct {
			Arguments map[string]any `json:"arguments"`
		}
		_ = json.Unmarshal(req.Params, &params)
		text, _ := params.Arguments["text"].(string)
		result = map[string]any{"content": []map[string]any{{"type": "text", "text": "echo: " + text}}}
	default:
		result = map[string]any{}
	}
	data, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": *req.ID, "result": result})

	if s.sse {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprintf(w, "event: message\ndata: %s\n\n", data)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(data)
}

func startHTTPEcho(t *testing.T, token string, sse bool) (*httpEchoServer, string) {
	t.Helper()
	stub := &httpEchoServer{token: token, sse: sse}
	srv := httptest.NewServer(stub)
	t.Cleanup(srv.Close)
	return stub, srv.URL
}

// bootstrappingWithMCP wires a config file with the given MCP servers
// through config.Load → NewBootstrap, mirroring TestE2EMCPBootstrapIntegration.
func bootstrapWithMCP(t *testing.T, servers map[string]config.MCPServer, lookup config.EnvLookup) *app.Bootstrap {
	t.Helper()
	ws := t.TempDir()
	if lookup == nil {
		lookup = envLookupOrEmpty
	}
	configPath := writeMCPConfig(t, ws, servers)
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, lookup)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	prepareSessionsDir(t, resolved)

	proc, err := app.NewProcessRuntime(context.Background(), resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(ws, "artifacts"),
		Version:     "e2e-test",
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime() error = %v", err)
	}
	t.Cleanup(proc.Close)
	bootstrap, err := app.NewWorkspaceBootstrap(context.Background(), proc, app.BootstrapConfig{
		WorkspaceRoot: ws,
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap() error = %v", err)
	}
	t.Cleanup(bootstrap.Close)
	return bootstrap
}

// envLookupOrEmpty resolves no variables, so ${VAR} references fail
// unless the test injects its own lookup.
func envLookupOrEmpty(string) (string, bool) { return "", false }

// callEchoTool runs a Prepare/Execute round-trip against mcp__http__echo.
func callEchoTool(t *testing.T, bootstrap *app.Bootstrap, toolName, text string) string {
	t.Helper()
	tool, ok := bootstrap.Registry.Lookup(toolName)
	if !ok {
		t.Fatalf("%s not found in the tool registry", toolName)
	}
	argsJSON, _ := json.Marshal(map[string]any{"text": text})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      toolName,
		Arguments: argsJSON,
	})
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if len(result.Content) == 0 {
		t.Fatal("Execute() returned no content parts")
	}
	return result.Content[0].Text
}

// -----------------------------------------------------------------------
// E2E: streamable HTTP transport through Bootstrap
// -----------------------------------------------------------------------

// TestE2EMCPHTTPBootstrapIntegration verifies the full chain over the
// streamable HTTP transport: config file with url+headers → config.Load
// → NewBootstrap → tool discovered and registered → Prepare/Execute
// round-trip, with the server observing the auth and session headers.
func TestE2EMCPHTTPBootstrapIntegration(t *testing.T) {
	stub, url := startHTTPEcho(t, "e2e-token", false)

	bootstrap := bootstrapWithMCP(t, map[string]config.MCPServer{
		"http": {
			URL:     url,
			Headers: map[string]string{"Authorization": "Bearer e2e-token"},
		},
	}, nil)

	if bootstrap.MCPManager == nil {
		t.Fatal("MCPManager is nil, want a running MCP server")
	}
	text := callEchoTool(t, bootstrap, "mcp__http__echo", "hello http")
	if text != "echo: hello http" {
		t.Fatalf("echo text = %q, want %q", text, "echo: hello http")
	}

	stub.mu.Lock()
	defer stub.mu.Unlock()
	if len(stub.authHeaders) < 3 {
		t.Fatalf("POST count = %d, want at least 3 (initialize, tools/list, tools/call)", len(stub.authHeaders))
	}
	for i, got := range stub.authHeaders {
		if got != "Bearer e2e-token" {
			t.Fatalf("authHeaders[%d] = %q, want Bearer e2e-token", i, got)
		}
	}
	if stub.sessionHeaders[0] != "" {
		t.Fatalf("initialize carried session %q, want none", stub.sessionHeaders[0])
	}
	for i := 1; i < len(stub.sessionHeaders); i++ {
		if stub.sessionHeaders[i] != "e2e-session" {
			t.Fatalf("sessionHeaders[%d] = %q, want e2e-session", i, stub.sessionHeaders[i])
		}
	}
}

// TestE2EMCPHTTPSSEResponses runs the same chain with the server forced
// to answer requests over text/event-stream.
func TestE2EMCPHTTPSSEResponses(t *testing.T) {
	_, url := startHTTPEcho(t, "", true)

	bootstrap := bootstrapWithMCP(t, map[string]config.MCPServer{
		"http": {URL: url},
	}, nil)

	text := callEchoTool(t, bootstrap, "mcp__http__echo", "hello sse")
	if text != "echo: hello sse" {
		t.Fatalf("echo text = %q, want %q", text, "echo: hello sse")
	}
}

// TestE2EMCPHTTPHeaderEnvExpansion verifies that a ${VAR} reference in a
// header value resolves through the config env lookup before the client
// ever sees it.
func TestE2EMCPHTTPHeaderEnvExpansion(t *testing.T) {
	stub, url := startHTTPEcho(t, "expanded-token", false)
	lookup := func(name string) (string, bool) {
		if name == "MCP_E2E_TOKEN" {
			return "expanded-token", true
		}
		return "", false
	}

	bootstrap := bootstrapWithMCP(t, map[string]config.MCPServer{
		"http": {
			URL:     url,
			Headers: map[string]string{"Authorization": "Bearer ${MCP_E2E_TOKEN}"},
		},
	}, lookup)

	text := callEchoTool(t, bootstrap, "mcp__http__echo", "env expansion")
	if text != "echo: env expansion" {
		t.Fatalf("echo text = %q, want %q", text, "echo: env expansion")
	}
	stub.mu.Lock()
	defer stub.mu.Unlock()
	if len(stub.authHeaders) == 0 || stub.authHeaders[0] != "Bearer expanded-token" {
		t.Fatalf("authHeaders = %v, want Bearer expanded-token", stub.authHeaders)
	}
}

// TestE2EMCPHTTPAuthFailureGraceful verifies that a server rejecting the
// configured token degrades gracefully: no tools, but the manager keeps
// the failure reason for /mcp and built-ins keep working.
func TestE2EMCPHTTPAuthFailureGraceful(t *testing.T) {
	_, url := startHTTPEcho(t, "right-token", false)

	bootstrap := bootstrapWithMCP(t, map[string]config.MCPServer{
		"http": {
			URL:     url,
			Headers: map[string]string{"Authorization": "Bearer wrong-token"},
		},
	}, nil)

	if bootstrap.MCPManager == nil {
		t.Fatal("MCPManager should be kept to report per-server failures")
	}
	servers := bootstrap.MCPManager.Servers()
	if len(servers) != 1 || servers[0].Connected {
		t.Fatalf("Servers() = %+v, want one disconnected server", servers)
	}
	if !strings.Contains(servers[0].Error, "401") {
		t.Fatalf("Servers()[0].Error = %q, want a 401 mention", servers[0].Error)
	}
	if _, ok := bootstrap.Registry.Lookup("mcp__http__echo"); ok {
		t.Fatal("mcp__http__echo should NOT be registered (auth failed)")
	}
	if _, ok := bootstrap.Registry.Lookup("read_file"); !ok {
		t.Fatal("read_file should still be registered (built-in)")
	}
}

// TestE2EMCPHTTPConfigValidation covers the url-side config constraints;
// the command-side cases live in TestE2EMCPConfigValidationErrors.
func TestE2EMCPHTTPConfigValidation(t *testing.T) {
	tests := []struct {
		name    string
		servers map[string]config.MCPServer
		lookup  config.EnvLookup
		wantErr string
	}{
		{
			name:    "command and url both set",
			servers: map[string]config.MCPServer{"bad": {Command: "/bin/true", URL: "http://localhost/mcp"}},
			wantErr: "mutually exclusive",
		},
		{
			name:    "non-http url",
			servers: map[string]config.MCPServer{"bad": {URL: "ftp://example.com/mcp"}},
			wantErr: "valid http(s) URL",
		},
		{
			name:    "url without host",
			servers: map[string]config.MCPServer{"bad": {URL: "http://"}},
			wantErr: "valid http(s) URL",
		},
		{
			name:    "args with url",
			servers: map[string]config.MCPServer{"bad": {URL: "http://localhost/mcp", Args: []string{"x"}}},
			wantErr: "only apply to command",
		},
		{
			name:    "headers with command",
			servers: map[string]config.MCPServer{"bad": {Command: "/bin/true", Headers: map[string]string{"Authorization": "Bearer x"}}},
			wantErr: "only apply to url",
		},
		{
			name: "unset env reference in header",
			servers: map[string]config.MCPServer{"bad": {
				URL:     "http://localhost/mcp",
				Headers: map[string]string{"Authorization": "Bearer ${DEFINITELY_UNSET_VAR}"},
			}},
			wantErr: "not set",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			lookup := tt.lookup
			if lookup == nil {
				lookup = envLookupOrEmpty
			}
			configPath := writeMCPConfig(t, t.TempDir(), tt.servers)
			_, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, lookup)
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("Load() error = %v, want %q", err, tt.wantErr)
			}
		})
	}
}
