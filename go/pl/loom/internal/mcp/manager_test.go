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
// Created: 2026/08/08

package mcp

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"testing"
	"time"
)

// TestManagerHelperProcess is the MCP server subprocess: re-executed with
// LOOM_MCP_TEST_HELPER=1 it speaks newline-delimited JSON-RPC on
// stdin/stdout, answering initialize and tools/list with one echo tool.
func TestManagerHelperProcess(t *testing.T) {
	if os.Getenv("LOOM_MCP_TEST_HELPER") != "1" {
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
		var result any
		switch msg.Method {
		case "initialize":
			result = map[string]any{
				"protocolVersion": "2025-06-18",
				"serverInfo":      map[string]any{"name": "test-server", "version": "0.1"},
				"capabilities":    map[string]any{},
			}
		case "notifications/initialized":
			continue
		case "tools/list":
			result = map[string]any{"tools": []map[string]any{{
				"name":        "echo",
				"description": "echo tool",
				"inputSchema": map[string]any{"type": "object"},
			}}}
		default:
			continue
		}
		data, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": msg.ID, "result": result})
		fmt.Fprintf(os.Stdout, "%s\n", data)
	}
	os.Exit(0)
}

// helperServerConfig returns the stdio config that re-executes this test
// binary as the echo MCP server.
func helperServerConfig(t *testing.T) ServerConfig {
	t.Helper()
	return ServerConfig{
		Command: os.Args[0],
		Args:    []string{"-test.run=TestManagerHelperProcess"},
		Env:     map[string]string{"LOOM_MCP_TEST_HELPER": "1"},
	}
}

func testCtx(t *testing.T) context.Context {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	t.Cleanup(cancel)
	return ctx
}

func TestManagerAddAndRemove(t *testing.T) {
	m := NewManager(nil)
	t.Cleanup(func() { _ = m.Close() })

	st := m.Add(testCtx(t), "echo", helperServerConfig(t))
	if !st.Connected {
		t.Fatalf("not connected: %s", st.Error)
	}
	if len(st.Tools) != 1 || st.Tools[0] != "mcp__echo__echo" {
		t.Fatalf("tools = %v, want [mcp__echo__echo]", st.Tools)
	}
	if got := len(m.Tools()); got != 1 {
		t.Fatalf("Tools() = %d, want 1", got)
	}
	if got, ok := m.Status("echo"); !ok || !got.Connected {
		t.Fatalf("Status(echo) = %+v, %v", got, ok)
	}

	m.Remove("echo")
	if got := len(m.Servers()); got != 0 {
		t.Fatalf("Servers() after remove = %d, want 0", got)
	}
	if got := len(m.Tools()); got != 0 {
		t.Fatalf("Tools() after remove = %d, want 0", got)
	}
	// Unknown names are no-ops.
	m.Remove("ghost")
}

func TestManagerAddFailureIsReported(t *testing.T) {
	m := NewManager(nil)
	t.Cleanup(func() { _ = m.Close() })

	st := m.Add(testCtx(t), "bad", ServerConfig{Command: "/nonexistent/loom-mcp-test-binary"})
	if st.Connected || st.Error == "" {
		t.Fatalf("status = %+v, want a reported failure", st)
	}
	// The failed server stays listed so frontends can show the reason.
	servers := m.Servers()
	if len(servers) != 1 || servers[0].Connected || servers[0].Error == "" {
		t.Fatalf("Servers() = %+v, want the failed entry listed", servers)
	}
	if got := len(m.Tools()); got != 0 {
		t.Fatalf("Tools() = %d, want 0 for a failed server", got)
	}
}

func TestManagerReconcile(t *testing.T) {
	m := NewManager(nil)
	t.Cleanup(func() { _ = m.Close() })
	ctx := testCtx(t)

	m.Add(ctx, "stay", helperServerConfig(t))
	m.Add(ctx, "remove-me", helperServerConfig(t))

	changed := helperServerConfig(t)
	changed.StartupTimeoutSec = 60 // force a config diff → reconnect
	m.Add(ctx, "change-me", helperServerConfig(t))

	m.Reconcile(ctx, map[string]ServerConfig{
		"stay":      helperServerConfig(t),
		"change-me": changed,
		"new":       helperServerConfig(t),
	})

	names := map[string]ServerStatus{}
	for _, s := range m.Servers() {
		names[s.Name] = s
	}
	if _, ok := names["remove-me"]; ok {
		t.Fatal("remove-me survived reconcile")
	}
	for _, want := range []string{"stay", "change-me", "new"} {
		s, ok := names[want]
		if !ok || !s.Connected {
			t.Fatalf("server %q = %+v (present=%v), want connected", want, s, ok)
		}
	}
	// stay/change-me/new each expose the echo tool.
	if got := len(m.Tools()); got != 3 {
		t.Fatalf("Tools() = %d, want 3", got)
	}
}

func TestStartServersAllFailKeepsManager(t *testing.T) {
	m, err := StartServers(testCtx(t), map[string]ServerConfig{
		"bad": {Command: "/nonexistent/loom-mcp-test-binary"},
	}, nil)
	if err == nil {
		t.Fatal("expected an error when every server fails")
	}
	if m == nil || len(m.Servers()) != 1 {
		t.Fatalf("manager = %+v, want the failure recorded for inspection", m)
	}
	_ = m.Close()
}
