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
// Created: 2026/08/15

package toolkit

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func testDef(name string) domain.ToolDefinition {
	return domain.ToolDefinition{
		Name:         name,
		Description:  "test tool",
		InputSchema:  json.RawMessage(`{"type":"object"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}
}

func testCall(t *testing.T, name string, args any) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("marshal args: %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw}
}

func TestBaseToolPrepareCallSignsContracts(t *testing.T) {
	bt, err := NewBaseTool(testDef("t1"))
	if err != nil {
		t.Fatalf("NewBaseTool() error = %v", err)
	}
	risk := domain.R3
	call := testCall(t, "t1", map[string]any{"path": "a.txt"})
	prepared, err := bt.PrepareCall(context.Background(), call, call.Arguments, PrepareOptions{
		ReadPaths:    []string{"/ws/a.txt", "/ws/b.txt"},
		ApprovalDesc: "read a and b",
		Risk:         &risk,
		URLRequest:   &domain.URLRequest{Host: "example.com"},
	})
	if err != nil {
		t.Fatalf("PrepareCall() error = %v", err)
	}
	if prepared.Call.Name != "t1" {
		t.Fatalf("prepared.Call.Name = %q, want t1", prepared.Call.Name)
	}
	if prepared.Risk != domain.R3 {
		t.Fatalf("prepared.Risk = %d, want R3 override", prepared.Risk)
	}
	if len(prepared.ReadPaths) != 2 || prepared.ReadPaths[0] != "/ws/a.txt" {
		t.Fatalf("prepared.ReadPaths = %v, want sorted [a b]", prepared.ReadPaths)
	}
	if prepared.URLRequest == nil || prepared.URLRequest.Host != "example.com" {
		t.Fatalf("prepared.URLRequest = %+v, want example.com", prepared.URLRequest)
	}
	if prepared.ArgsHash == "" {
		t.Fatal("prepared.ArgsHash is empty")
	}
}

func TestBaseToolVerifyPreparedCallRejectsTampering(t *testing.T) {
	bt, err := NewBaseTool(testDef("t2"))
	if err != nil {
		t.Fatalf("NewBaseTool() error = %v", err)
	}
	call := testCall(t, "t2", map[string]any{"path": "a.txt"})
	prepared, err := bt.PrepareCall(context.Background(), call, call.Arguments, PrepareOptions{
		ReadPaths:    []string{"/ws/a.txt"},
		ApprovalDesc: "read a",
	})
	if err != nil {
		t.Fatalf("PrepareCall() error = %v", err)
	}
	if err := bt.VerifyPreparedCall(prepared); err != nil {
		t.Fatalf("VerifyPreparedCall(valid) error = %v", err)
	}

	tampered := prepared
	tampered.ReadPaths = []string{"/ws/other.txt"}
	if err := bt.VerifyPreparedCall(tampered); err == nil {
		t.Fatal("VerifyPreparedCall(tampered ReadPaths) succeeded, want error")
	}

	// A different tool instance must not accept the signature.
	other, err := NewBaseTool(testDef("t2"))
	if err != nil {
		t.Fatalf("NewBaseTool() error = %v", err)
	}
	if err := other.VerifyPreparedCall(prepared); err == nil {
		t.Fatal("VerifyPreparedCall(cross-instance) succeeded, want error")
	}
}

func TestBaseToolVerifyPreparedCallStructuralSkipsRisk(t *testing.T) {
	bt, err := NewBaseTool(testDef("t3"))
	if err != nil {
		t.Fatalf("NewBaseTool() error = %v", err)
	}
	risk := domain.R3
	call := testCall(t, "t3", map[string]any{"action": "navigate"})
	prepared, err := bt.PrepareCall(context.Background(), call, call.Arguments, PrepareOptions{
		ApprovalDesc: "graded action",
		Risk:         &risk,
	})
	if err != nil {
		t.Fatalf("PrepareCall() error = %v", err)
	}
	// Structural verification passes with the graded risk intact.
	if err := bt.VerifyPreparedCallStructural(prepared); err != nil {
		t.Fatalf("VerifyPreparedCallStructural() error = %v", err)
	}
	// Full verification REJECTS the graded risk: VerifyPreparedCall
	// compares against the definition default (R1 for CapFSRead), which
	// is why tools that override risk (browser) verify structurally and
	// re-derive the risk themselves.
	if err := bt.VerifyPreparedCall(prepared); err == nil {
		t.Fatal("VerifyPreparedCall(risk override) succeeded, want risk mismatch")
	}
}
