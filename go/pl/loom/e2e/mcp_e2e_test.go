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
// Created: 2026/08/01

package e2e

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"gopkg.in/yaml.v3"
)

// mcp_e2e_test.go verifies the MCP integration end to end: a real MCP
// server subprocess (TestMCPHelperProcess), the real config loader, the
// real Bootstrap assembly, and the real tool registry — the same code
// path a user hits when they add mcp_servers to ~/.loom/config.yaml.

// -----------------------------------------------------------------------
// Minimal MCP echo server (runs as a child process via TestMCPHelperProcess)
// -----------------------------------------------------------------------

// TestMCPHelperProcess is the MCP server subprocess. When the test
// binary is re-executed with GO_WANT_HELPER_PROCESS=1, this function
// reads newline-delimited JSON-RPC from stdin and writes responses to
// stdout — the same wire protocol as any external MCP server.
func TestMCPHelperProcess(t *testing.T) {
	if os.Getenv("GO_WANT_HELPER_PROCESS") != "1" {
		return
	}
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var msg struct {
			ID     *int64          `json:"id,omitempty"`
			Method string          `json:"method,omitempty"`
			Params json.RawMessage `json:"params,omitempty"`
		}
		if err := json.Unmarshal(line, &msg); err != nil {
			continue
		}
		switch msg.Method {
		case "initialize":
			writeRPC(msg.ID, map[string]any{
				"protocolVersion": "2025-06-18",
				"serverInfo":      map[string]any{"name": "echo-server", "version": "0.1"},
				"capabilities":    map[string]any{},
			})
		case "notifications/initialized":
			// Notification: no response.
		case "tools/list":
			writeRPC(msg.ID, map[string]any{
				"tools": []map[string]any{{
					"name":        "echo",
					"description": "Echo the input text back",
					"inputSchema": map[string]any{
						"type":       "object",
						"properties": map[string]any{"text": map[string]any{"type": "string"}},
					},
				}},
			})
		case "tools/call":
			var params struct {
				Name      string         `json:"name"`
				Arguments map[string]any `json:"arguments"`
			}
			_ = json.Unmarshal(msg.Params, &params)
			text, _ := params.Arguments["text"].(string)
			writeRPC(msg.ID, map[string]any{
				"content": []map[string]any{
					{"type": "text", "text": "echo: " + text},
				},
			})
		}
	}
	os.Exit(0)
}

func writeRPC(id *int64, result any) {
	resp := map[string]any{"jsonrpc": "2.0", "id": id, "result": result}
	data, _ := json.Marshal(resp)
	fmt.Fprintf(os.Stdout, "%s\n", data)
}

// helperCommand returns the command and args that re-execute this test
// binary as the MCP echo server.
func helperCommand(t *testing.T) (string, []string, map[string]string) {
	t.Helper()
	return os.Args[0],
		[]string{"-test.run=TestMCPHelperProcess"},
		map[string]string{"GO_WANT_HELPER_PROCESS": "1"}
}

// writeMCPConfig serializes a config.File with a provider and the given
// MCP servers to a YAML file, returning the path.
func writeMCPConfig(t *testing.T, dir string, mcpServers map[string]config.MCPServer) string {
	t.Helper()
	f := config.File{
		Default: "test/test-model",
		Providers: []config.Provider{{
			Name:     "test",
			Type:     "openai",
			BaseURL:  "http://localhost:1",
			APIKey:   "test-key",
			Models:   []config.Model{{Name: "test-model", ContextWindow: 128000}},
		}},
		MCPServers: mcpServers,
	}
	data, err := yaml.Marshal(f)
	if err != nil {
		t.Fatalf("yaml.Marshal() error = %v", err)
	}
	path := filepath.Join(dir, "config.yaml")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

// -----------------------------------------------------------------------
// E2E: config resolution
// -----------------------------------------------------------------------

// TestE2EMCPConfigResolution verifies that mcp_servers in the YAML file
// flows through config.Load → resolve into ResolvedConfig.MCP.
func TestE2EMCPConfigResolution(t *testing.T) {
	cmd, args, env := helperCommand(t)

	configPath := writeMCPConfig(t, t.TempDir(), map[string]config.MCPServer{
		"echo": {
			Command:          cmd,
			Args:             args,
			Env:              env,
			StartupTimeoutSec: 15,
			ToolTimeoutSec:    60,
			EnabledTools:     []string{"echo"},
		},
	})
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, os.LookupEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}

	// MCP section resolved.
	if len(resolved.MCP.Servers) != 1 {
		t.Fatalf("MCP.Servers count = %d, want 1", len(resolved.MCP.Servers))
	}
	srv, ok := resolved.MCP.Servers["echo"]
	if !ok {
		t.Fatal("MCP.Servers missing \"echo\" server")
	}
	if srv.Command != cmd {
		t.Fatalf("command = %q, want %q", srv.Command, cmd)
	}
	if srv.StartupTimeoutSec != 15 {
		t.Fatalf("startup_timeout_sec = %v, want 15", srv.StartupTimeoutSec)
	}
	if srv.ToolTimeoutSec != 60 {
		t.Fatalf("tool_timeout_sec = %v, want 60", srv.ToolTimeoutSec)
	}
	if len(srv.EnabledTools) != 1 || srv.EnabledTools[0] != "echo" {
		t.Fatalf("enabled_tools = %v, want [echo]", srv.EnabledTools)
	}
}

// TestE2EMCPConfigValidationErrors verifies that invalid MCP entries
// are rejected at load time.
func TestE2EMCPConfigValidationErrors(t *testing.T) {
	tests := []struct {
		name    string
		servers map[string]config.MCPServer
		wantErr string
	}{
		{
			name: "missing command",
			servers: map[string]config.MCPServer{
				"bad": {Args: []string{"-test.run=TestMCPHelperProcess"}},
			},
			wantErr: "command is required",
		},
		{
			name: "negative startup timeout",
			servers: map[string]config.MCPServer{
				"bad": {Command: "/bin/true", StartupTimeoutSec: -1},
			},
			wantErr: "startup_timeout_sec must be >= 0",
		},
		{
			name: "negative tool timeout",
			servers: map[string]config.MCPServer{
				"bad": {Command: "/bin/true", ToolTimeoutSec: -0.5},
			},
			wantErr: "tool_timeout_sec must be >= 0",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			configPath := writeMCPConfig(t, t.TempDir(), tt.servers)
			_, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, os.LookupEnv)
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("Load() error = %v, want %q", err, tt.wantErr)
			}
		})
	}
}

// -----------------------------------------------------------------------
// E2E: Bootstrap integration — MCP server startup + tool registration
// -----------------------------------------------------------------------

// TestE2EMCPBootstrapIntegration verifies the full chain: config file
// with mcp_servers → config.Load → NewBootstrap → MCP server started →
// tools discovered and registered → Prepare/Execute round-trip.
func TestE2EMCPBootstrapIntegration(t *testing.T) {
	ws := t.TempDir()
	cmd, args, env := helperCommand(t)

	configPath := writeMCPConfig(t, ws, map[string]config.MCPServer{
		"echo": {
			Command:           cmd,
			Args:              args,
			Env:               env,
			StartupTimeoutSec: 15,
			ToolTimeoutSec:    60,
		},
	})
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, os.LookupEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	resolved.Storage.SessionDB = filepath.Join(ws, "sessions.db")

	bootstrap, err := app.NewBootstrap(context.Background(), resolved, app.BootstrapConfig{
		WorkspaceRoot: ws,
		ArtifactDir:   filepath.Join(ws, "artifacts"),
		Version:       "e2e-test",
	})
	if err != nil {
		t.Fatalf("NewBootstrap() error = %v", err)
	}
	defer bootstrap.Close()

	// MCP manager is non-nil (server started successfully).
	if bootstrap.MCPManager == nil {
		t.Fatal("MCPManager is nil, want a running MCP server")
	}

	// The echo tool was discovered and registered with the qualified name.
	tool, ok := bootstrap.Registry.Lookup("mcp__echo__echo")
	if !ok {
		t.Fatal("mcp__echo__echo not found in the tool registry")
	}
	def := tool.Definition()
	if def.Name != "mcp__echo__echo" {
		t.Fatalf("tool name = %q, want mcp__echo__echo", def.Name)
	}
	if def.Source != domain.ToolSourceMCP {
		t.Fatalf("tool source = %q, want %q", def.Source, domain.ToolSourceMCP)
	}
	if !strings.Contains(def.Description, `[MCP server "echo"]`) || !strings.Contains(def.Description, "Echo") {
		t.Fatalf("tool description = %q, want MCP server prefix and Echo mention", def.Description)
	}

	// Prepare + Execute round-trip: the tool call travels over stdio
	// JSON-RPC to the child process and the result comes back.
	callID := domain.NewToolCallID()
	argsJSON, _ := json.Marshal(map[string]any{"text": "hello mcp"})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        callID,
		Name:      "mcp__echo__echo",
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
	text := result.Content[0].Text
	if text != "echo: hello mcp" {
		t.Fatalf("Execute() text = %q, want %q", text, "echo: hello mcp")
	}
}

// TestE2EMCPToolFiltering verifies that enabled_tools/disabled_tools
// filter the discovered catalog before registration.
func TestE2EMCPToolFiltering(t *testing.T) {
	ws := t.TempDir()
	cmd, args, env := helperCommand(t)

	configPath := writeMCPConfig(t, ws, map[string]config.MCPServer{
		"echo": {
			Command:           cmd,
			Args:              args,
			Env:               env,
			StartupTimeoutSec: 15,
			EnabledTools:      []string{"nonexistent_tool"},
		},
	})
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, os.LookupEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	resolved.Storage.SessionDB = filepath.Join(ws, "sessions.db")

	bootstrap, err := app.NewBootstrap(context.Background(), resolved, app.BootstrapConfig{
		WorkspaceRoot: ws,
		ArtifactDir:   filepath.Join(ws, "artifacts"),
		Version:       "e2e-test",
	})
	if err != nil {
		t.Fatalf("NewBootstrap() error = %v", err)
	}
	defer bootstrap.Close()

	// The MCP server started but no tools matched the filter, so the
	// qualified name must NOT be in the registry.
	if _, ok := bootstrap.Registry.Lookup("mcp__echo__echo"); ok {
		t.Fatal("mcp__echo__echo should NOT be registered (filtered out by enabled_tools)")
	}
}

// TestE2EMCPGracefulDegradation verifies that a nonexistent MCP server
// command does not crash Bootstrap — the agent runs with built-in tools only.
func TestE2EMCPGracefulDegradation(t *testing.T) {
	ws := t.TempDir()

	configPath := writeMCPConfig(t, ws, map[string]config.MCPServer{
		"broken": {
			Command: "/nonexistent/mcp-server-that-does-not-exist",
		},
	})
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true}, os.LookupEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	resolved.Storage.SessionDB = filepath.Join(ws, "sessions.db")

	bootstrap, err := app.NewBootstrap(context.Background(), resolved, app.BootstrapConfig{
		WorkspaceRoot: ws,
		ArtifactDir:   filepath.Join(ws, "artifacts"),
		Version:       "e2e-test",
	})
	if err != nil {
		t.Fatalf("NewBootstrap() error = %v, want graceful degradation", err)
	}
	defer bootstrap.Close()

	// MCPManager should be nil (no server could start).
	if bootstrap.MCPManager != nil {
		t.Fatal("MCPManager should be nil when all servers fail to start")
	}
	// Built-in tools still registered.
	if _, ok := bootstrap.Registry.Lookup("read_file"); !ok {
		t.Fatal("read_file should still be registered (built-in)")
	}
}
