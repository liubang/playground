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
// Created: 2026/08/17

package permission

import (
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func rawRunCmd(t *testing.T, program string, args ...string) json.RawMessage {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"program": program, "args": args})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func rawWebFetch(t *testing.T, url string) json.RawMessage {
	t.Helper()
	raw, err := json.Marshal(map[string]string{"url": url})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

// runCmdInfo builds a RunCmdCall via classifyShell so the shell analysis
// fields are populated exactly like the production Prepare path.
func runCmdInfo(t *testing.T, program string, args ...string) RunCmdCall {
	t.Helper()
	info, ok := ParseRunCmdCall(rawRunCmd(t, program, args...))
	if !ok {
		t.Fatal("ParseRunCmdCall failed")
	}
	return info
}

// TestDeriveMemoryShapeArgv covers the ExecRequest → MemoryArgv path:
// both the typed ExecRequest (Prepare path) and the raw-argument fallback
// (approval UI boundary) resolve to MemoryArgv with the correct info.
func TestDeriveMemoryShapeArgv(t *testing.T) {
	// Typed ExecRequest (Prepare path).
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: rawRunCmd(t, "go", "test", "./...")},
		ExecRequest: &domain.ExecRequest{
			Argv: []string{"go", "test", "./..."},
		},
	}
	shape := DeriveMemoryShape(call)
	if shape.Kind != MemoryArgv {
		t.Fatalf("kind = %d, want MemoryArgv", shape.Kind)
	}
	if len(shape.Info.Argv) != 3 || shape.Info.Argv[0] != "go" {
		t.Fatalf("argv = %v", shape.Info.Argv)
	}

	// Raw-argument fallback (approval UI boundary).
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: rawRunCmd(t, "go", "vet", "./...")},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryArgv {
		t.Fatalf("kind = %d, want MemoryArgv (fallback)", shape.Kind)
	}
	if len(shape.Info.Argv) != 3 || shape.Info.Argv[0] != "go" {
		t.Fatalf("argv = %v (fallback)", shape.Info.Argv)
	}

	// exec_session also flows through MemoryArgv — the tool name is
	// irrelevant, the typed ExecRequest is authoritative.
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "exec_session", Arguments: json.RawMessage(`{}`)},
		ExecRequest: &domain.ExecRequest{
			Argv: []string{"make", "build"},
		},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryArgv {
		t.Fatalf("exec_session kind = %d, want MemoryArgv", shape.Kind)
	}
	if shape.Info.Argv[0] != "make" {
		t.Fatalf("exec_session argv = %v", shape.Info.Argv)
	}
}

// TestDeriveMemoryShapeHost covers the URLRequest → MemoryHost path:
// both the typed URLRequest and the raw-argument fallback resolve to
// MemoryHost with the canonical host.
func TestDeriveMemoryShapeHost(t *testing.T) {
	// Typed URLRequest (Prepare path).
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_fetch", Arguments: rawWebFetch(t, "https://www.example.com/a")},
		URLRequest: &domain.URLRequest{
			Host: "www.example.com",
		},
	}
	shape := DeriveMemoryShape(call)
	if shape.Kind != MemoryHost {
		t.Fatalf("kind = %d, want MemoryHost", shape.Kind)
	}
	if shape.Host != "www.example.com" {
		t.Fatalf("host = %q, want www.example.com", shape.Host)
	}

	// Raw-argument fallback (approval UI boundary).
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_fetch", Arguments: rawWebFetch(t, "https://WWW.Example.COM/b")},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryHost {
		t.Fatalf("kind = %d, want MemoryHost (fallback)", shape.Kind)
	}
	if shape.Host != "www.example.com" {
		t.Fatalf("host = %q, want www.example.com (fallback)", shape.Host)
	}

	// browser navigate also flows through MemoryHost — the typed
	// URLRequest is authoritative, not the tool name.
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "browser_navigate", Arguments: json.RawMessage(`{}`)},
		URLRequest: &domain.URLRequest{
			Host: "docs.example.org",
		},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryHost {
		t.Fatalf("browser_navigate kind = %d, want MemoryHost", shape.Kind)
	}
	if shape.Host != "docs.example.org" {
		t.Fatalf("browser_navigate host = %q, want docs.example.org", shape.Host)
	}

	// Non-http URL → MemoryNone.
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_fetch", Arguments: rawWebFetch(t, "ftp://x/y")},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryNone {
		t.Fatalf("ftp URL kind = %d, want MemoryNone", shape.Kind)
	}
}

// TestDeriveMemoryShapeTool covers the fallback → MemoryTool path:
// eligible tools (generate_image, web_search, MCP tools) resolve to
// MemoryTool; ineligible tools resolve to MemoryNone.
func TestDeriveMemoryShapeTool(t *testing.T) {
	// generate_image is eligible.
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "generate_image", Arguments: json.RawMessage(`{"prompt":"x"}`)},
	}
	shape := DeriveMemoryShape(call)
	if shape.Kind != MemoryTool {
		t.Fatalf("generate_image kind = %d, want MemoryTool", shape.Kind)
	}
	if shape.ToolName != "generate_image" {
		t.Fatalf("tool = %q, want generate_image", shape.ToolName)
	}

	// web_search is eligible.
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_search", Arguments: json.RawMessage(`{"query":"x"}`)},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryTool {
		t.Fatalf("web_search kind = %d, want MemoryTool", shape.Kind)
	}
	if shape.ToolName != "web_search" {
		t.Fatalf("tool = %q, want web_search", shape.ToolName)
	}

	// MCP tool is eligible.
	call = domain.PreparedCall{
		Call: domain.ToolCall{Name: "mcp__github__create_pr", Arguments: json.RawMessage(`{}`)},
	}
	shape = DeriveMemoryShape(call)
	if shape.Kind != MemoryTool {
		t.Fatalf("mcp tool kind = %d, want MemoryTool", shape.Kind)
	}
	if shape.ToolName != "mcp__github__create_pr" {
		t.Fatalf("tool = %q, want mcp__github__create_pr", shape.ToolName)
	}

	// Ineligible tools → MemoryNone.
	for _, name := range []string{"edit", "write", "view_image"} {
		call = domain.PreparedCall{
			Call: domain.ToolCall{Name: name, Arguments: json.RawMessage(`{}`)},
		}
		shape = DeriveMemoryShape(call)
		if shape.Kind != MemoryNone {
			t.Fatalf("%s kind = %d, want MemoryNone", name, shape.Kind)
		}
	}
}

// TestMemoryShapePreviewLabel covers the display rendering for each kind.
func TestMemoryShapePreviewLabel(t *testing.T) {
	// MemoryArgv: prefix label + zero grant for a plain call.
	shape := MemoryShape{
		Kind: MemoryArgv,
		Info: runCmdInfo(t, "go", "test", "./..."),
	}
	label, grant, ok := shape.PreviewLabel()
	if !ok || label != "go test" {
		t.Fatalf("argv preview = %q ok=%v, want 'go test'", label, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("argv grant = %+v, want zero", grant)
	}

	// MemoryArgv: compound shell renders one label per subcommand.
	shape = MemoryShape{
		Kind: MemoryArgv,
		Info: runCmdInfo(t, "sh", "-c", "go test ./... && git status"),
	}
	label, _, ok = shape.PreviewLabel()
	if !ok || label != "go test && git status" {
		t.Fatalf("compound preview = %q ok=%v, want 'go test && git status'", label, ok)
	}

	// MemoryArgv: dynamic shell has no preview.
	shape = MemoryShape{
		Kind: MemoryArgv,
		Info: runCmdInfo(t, "sh", "-c", "echo hi > $out"),
	}
	if _, _, ok := shape.PreviewLabel(); ok {
		t.Fatal("dynamic shell must not have a preview")
	}

	// MemoryHost: host label.
	shape = MemoryShape{Kind: MemoryHost, Host: "www.example.com"}
	label, grant, ok = shape.PreviewLabel()
	if !ok || label != "www.example.com" {
		t.Fatalf("host preview = %q ok=%v, want www.example.com", label, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("host grant = %+v, want zero", grant)
	}

	// MemoryHost: empty host has no preview.
	shape = MemoryShape{Kind: MemoryHost, Host: ""}
	if _, _, ok := shape.PreviewLabel(); ok {
		t.Fatal("empty host must not have a preview")
	}

	// MemoryTool: tool name label.
	shape = MemoryShape{Kind: MemoryTool, ToolName: "generate_image"}
	label, grant, ok = shape.PreviewLabel()
	if !ok || label != "generate_image" {
		t.Fatalf("tool preview = %q ok=%v, want generate_image", label, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("tool grant = %+v, want zero", grant)
	}

	// MemoryNone: no preview.
	shape = MemoryShape{Kind: MemoryNone}
	if _, _, ok := shape.PreviewLabel(); ok {
		t.Fatal("MemoryNone must not have a preview")
	}
}

// TestDeriveRememberGrant covers the grant derivation for the trust-flavor
// logic: unsandboxed trust on an escalated call → L2 full trust; otherwise
// → exactly the declared capabilities.
func TestDeriveRememberGrant(t *testing.T) {
	// Escalated + unsandboxed trust → L2.
	info := RunCmdCall{Argv: []string{"make", "deploy"}, Escalated: true}
	grant := DeriveRememberGrant(info, TrustUnsandboxed)
	if !grant.Unsandboxed {
		t.Fatalf("escalated + unsandboxed trust = %+v, want unsandboxed", grant)
	}

	// Escalated + no trust → declared grant (unsandboxed via DeclaredGrant).
	grant = DeriveRememberGrant(info, "")
	if !grant.Unsandboxed {
		t.Fatalf("escalated + no trust = %+v, want unsandboxed via DeclaredGrant", grant)
	}

	// Non-escalated + needs_network → network grant.
	info = RunCmdCall{Argv: []string{"talos", "query"}, NeedsNetwork: true}
	grant = DeriveRememberGrant(info, "")
	if !grant.NetworkFull || grant.Unsandboxed {
		t.Fatalf("needs_network = %+v, want network grant", grant)
	}

	// Non-escalated + no declarations → zero grant.
	info = RunCmdCall{Argv: []string{"go", "test"}}
	grant = DeriveRememberGrant(info, "")
	if !grant.IsZero() {
		t.Fatalf("plain call = %+v, want zero", grant)
	}

	// Non-escalated + unsandboxed trust → still zero (trust flavor only
	// applies to escalated calls).
	grant = DeriveRememberGrant(info, TrustUnsandboxed)
	if !grant.IsZero() {
		t.Fatalf("non-escalated trust = %+v, want zero", grant)
	}
}
