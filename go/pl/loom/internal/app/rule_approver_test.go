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
// Created: 2026/07/25

package app

import (
	"context"
	"encoding/json"
	"reflect"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func argsJSON(t *testing.T, program string, args ...string) json.RawMessage {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"program": program, "args": args})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func preparedRunCmd(t *testing.T, program string, args ...string) domain.PreparedCall {
	t.Helper()
	return domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "run_cmd",
			Arguments: argsJSON(t, program, args...),
		},
	}
}

func TestDeriveRunCmdPrefix(t *testing.T) {
	tests := []struct {
		name string
		argv []string
		want []string
		ok   bool
	}{
		{"subcommand widens the rule", []string{"go", "test", "./..."}, []string{"go", "test"}, true},
		{"golangci-lint run", []string{"golangci-lint", "run", "--out-format", "json", "./..."}, []string{"golangci-lint", "run"}, true},
		{"flag first arg keeps program only", []string{"bazel", "test"}, []string{"bazel", "test"}, true},
		{"program only when arg is a path", []string{"ls", "-la"}, []string{"ls"}, true},
		{"non-token arg keeps program only", []string{"rg", "pattern"}, []string{"rg"}, true},
		{"shell is banned", []string{"sh", "-c", "echo hi"}, nil, false},
		{"bash path is banned", []string{"/bin/bash", "-c", "echo hi"}, nil, false},
		{"python without subcommand is banned", []string{"python3", "script.py"}, nil, false},
		{"node is banned", []string{"node", "server.js"}, nil, false},
		{"rm is banned", []string{"rm", "-rf", "x"}, nil, false},
		{"sudo is banned", []string{"sudo", "make", "install"}, nil, false},
		{"heredoc is banned", []string{"sh-cmd-wrapper", "cat<<EOF"}, nil, false},
		{"empty argv", nil, nil, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, ok := DeriveRunCmdPrefix(tt.argv)
			if ok != tt.ok {
				t.Fatalf("DeriveRunCmdPrefix(%v) ok = %v, want %v", tt.argv, ok, tt.ok)
			}
			if tt.ok && !reflect.DeepEqual(got, tt.want) {
				t.Fatalf("DeriveRunCmdPrefix(%v) = %v, want %v", tt.argv, got, tt.want)
			}
		})
	}
}

type recordingApprover struct {
	calls int
	last  domain.ApprovalRequest
}

func (r *recordingApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	r.calls++
	r.last = req
	return domain.DecisionAsk, nil
}

func TestRuleApproverAutoAllowsRememberedPrefix(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner)

	prefix, ok := rules.RememberRunCmd("run_cmd", argsJSON(t, "go", "test", "./pl/loom/..."))
	if !ok {
		t.Fatal("expected go test to be persistable")
	}
	if !reflect.DeepEqual(prefix, []string{"go", "test"}) {
		t.Fatalf("prefix = %v, want [go test]", prefix)
	}

	// A matching later call is auto-approved without touching the user.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "go", "test", "./go/pl/other/...", "-count=1"),
	})
	if err != nil || decision != domain.DecisionAllow {
		t.Fatalf("decision = %v err = %v, want allow", decision, err)
	}
	if inner.calls != 0 {
		t.Fatalf("inner approver was consulted for a rule-matching call")
	}

	// A non-matching call still reaches the user.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "go", "build", "./..."),
	})
	if err != nil || decision != domain.DecisionAsk {
		t.Fatalf("decision = %v err = %v, want delegated ask", decision, err)
	}
	if inner.calls != 1 {
		t.Fatalf("inner approver calls = %d, want 1", inner.calls)
	}

	// Non-run_cmd tools never match.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "edit", Arguments: json.RawMessage(`{}`)}},
	})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 2 {
		t.Fatalf("edit call must delegate: decision=%v calls=%d", decision, inner.calls)
	}
}

func TestRememberRunCmdRejectsNonPersistable(t *testing.T) {
	rules := NewRuleApprover(nil)
	for name, raw := range map[string]json.RawMessage{
		"shell":      argsJSON(t, "sh", "-c", "echo hi"),
		"python":     argsJSON(t, "python3", "script.py"),
		"escalated":  json.RawMessage(`{"program":"go","args":["mod","download"],"sandbox_permissions":"require_escalated"}`),
		"other tool": json.RawMessage(`{"path":"x.go"}`),
	} {
		toolName := "run_cmd"
		if name == "other tool" {
			toolName = "edit"
		}
		if _, ok := rules.RememberRunCmd(toolName, raw); ok {
			t.Fatalf("%s must not be persistable", name)
		}
	}
	if rules.RunCmdRuleCount() != 0 {
		t.Fatalf("rules = %d, want 0", rules.RunCmdRuleCount())
	}
}

func TestRunCmdRulePreview(t *testing.T) {
	preview, ok := RunCmdRulePreview("run_cmd", argsJSON(t, "go", "vet", "./..."))
	if !ok || preview != "go vet" {
		t.Fatalf("preview = %q ok = %v, want 'go vet'", preview, ok)
	}
	if _, ok := RunCmdRulePreview("run_cmd", argsJSON(t, "sh", "-c", "x")); ok {
		t.Fatal("shell must not have a rule preview")
	}
	if _, ok := RunCmdRulePreview("edit", json.RawMessage(`{}`)); ok {
		t.Fatal("non-run_cmd must not have a rule preview")
	}
}
