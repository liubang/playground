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
	"github.com/liubang/playground/go/pl/loom/internal/permission"
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
		{"script file is persistable", []string{"python3", "script.py"}, []string{"python3", "script.py"}, true},
		{"node script path is persistable", []string{"node", "/home/u/.loom/skills/x/scripts/lx.js", "skill", "start"}, []string{"node", "/home/u/.loom/skills/x/scripts/lx.js"}, true},
		{"node eval is banned", []string{"node", "-e", "require('fs').rmSync('/')"}, nil, false},
		{"python bare repl is banned", []string{"python3"}, nil, false},
		{"rm is banned", []string{"rm", "-rf", "x"}, nil, false},
		{"sudo is banned", []string{"sudo", "make", "install"}, nil, false},
		{"heredoc is banned", []string{"sh-cmd-wrapper", "cat<<EOF"}, nil, false},
		{"empty argv", nil, nil, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, ok := permission.DeriveRunCmdPrefix(tt.argv)
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
	rules := NewRuleApprover(inner, permission.NewSessionRules())

	rule, ok := rules.RememberCall("run_cmd", argsJSON(t, "go", "test", "./pl/loom/..."), "")
	if !ok {
		t.Fatal("expected go test to be persistable")
	}
	if !reflect.DeepEqual(rule.Prefix, []string{"go", "test"}) {
		t.Fatalf("prefix = %v, want [go test]", rule.Prefix)
	}
	if !rule.Grant.IsZero() {
		t.Fatalf("grant = %+v, want zero for a plain sandboxed call", rule.Grant)
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
	rules := NewRuleApprover(nil, permission.NewSessionRules())
	for name, raw := range map[string]json.RawMessage{
		// A compound shell script cannot be unwrapped and stays
		// unpersistable; a PROVABLY SIMPLE sh -c command is persistable
		// under its inner program (covered in TestRememberRunCmd).
		"compound shell": argsJSON(t, "sh", "-c", "echo hi | cat"),
		"eval":           argsJSON(t, "python3", "-c", "print(1)"),
		"other tool":     json.RawMessage(`{"path":"x.go"}`),
	} {
		toolName := "run_cmd"
		if name == "other tool" {
			toolName = "edit"
		}
		if _, ok := rules.RememberCall(toolName, raw, ""); ok {
			t.Fatalf("%s must not be persistable", name)
		}
	}
	if rules.RunCmdRuleCount() != 0 {
		t.Fatalf("rules = %d, want 0", rules.RunCmdRuleCount())
	}
}

// TestRememberRunCmdGrantDerivation covers the grant flavors a remembered
// rule carries: needs_network calls remember a sandboxed network grant;
// escalated calls can ONLY be remembered as explicit L2 full trust — a
// lesser grant would never cover the next escalation.
func TestRememberRunCmdGrantDerivation(t *testing.T) {
	rules := NewRuleApprover(nil, permission.NewSessionRules())

	needsNet := json.RawMessage(`{"program":"mycli","args":["query","submit"],"needs_network":true}`)
	rule, ok := rules.RememberCall("run_cmd", needsNet, "")
	if !ok || !rule.Grant.NetworkFull || rule.Grant.Unsandboxed {
		t.Fatalf("needs_network remember = ok=%v grant=%+v, want sandboxed network grant", ok, rule.Grant)
	}

	escalated := json.RawMessage(`{"program":"make","args":["deploy"],"sandbox_permissions":"require_escalated"}`)
	if _, ok = rules.RememberCall("run_cmd", escalated, ""); ok {
		t.Fatal("escalated remember without trust flavor must be refused (minimal grants cannot cover escalations)")
	}

	rules2 := NewRuleApprover(nil, permission.NewSessionRules())
	rule, ok = rules2.RememberCall("run_cmd", escalated, TrustUnsandboxed)
	if !ok || !rule.Grant.Unsandboxed {
		t.Fatalf("escalated remember (trust flavor) = ok=%v grant=%+v, want unsandboxed", ok, rule.Grant)
	}
	// The trust flavor is honored only for escalated calls.
	rule, ok = rules2.RememberCall("run_cmd", argsJSON(t, "go", "build", "./..."), TrustUnsandboxed)
	if !ok || !rule.Grant.IsZero() {
		t.Fatalf("non-escalated trust flavor = ok=%v grant=%+v, want zero grant", ok, rule.Grant)
	}
}

func TestApprovalRulePreview(t *testing.T) {
	preview, grant, ok := ApprovalRulePreview("run_cmd", argsJSON(t, "go", "vet", "./..."))
	if !ok || preview != "go vet" {
		t.Fatalf("preview = %q ok = %v, want 'go vet'", preview, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("grant = %+v, want zero", grant)
	}
	// A compound shell script has no preview; a provably simple sh -c
	// command previews its inner program.
	if _, _, ok := ApprovalRulePreview("run_cmd", argsJSON(t, "sh", "-c", "x | y")); ok {
		t.Fatal("compound shell must not have a rule preview")
	}
	preview, _, ok = ApprovalRulePreview("run_cmd", argsJSON(t, "sh", "-c", "ls -la"))
	if !ok || preview != "ls" {
		t.Fatalf("simple shell preview = %q ok=%v, want 'ls'", preview, ok)
	}
	// Escalated calls have no minimal-capability preview (only the
	// unsandboxed trust option, surfaced by RunCmdTrustPreview).
	if _, _, ok := ApprovalRulePreview("run_cmd", json.RawMessage(`{"program":"make","args":["deploy"],"sandbox_permissions":"require_escalated"}`)); ok {
		t.Fatal("escalated calls must not have a minimal rule preview")
	}
	if _, _, ok := ApprovalRulePreview("edit", json.RawMessage(`{}`)); ok {
		t.Fatal("non-run_cmd must not have a rule preview")
	}

	// needs_network previews carry the network grant.
	_, grant, ok = ApprovalRulePreview("run_cmd", json.RawMessage(`{"program":"mycli","needs_network":true}`))
	if !ok || !grant.NetworkFull {
		t.Fatalf("needs_network preview grant = %+v ok=%v", grant, ok)
	}

	// web_fetch previews show the exact host.
	preview, _, ok = ApprovalRulePreview("web_fetch", json.RawMessage(`{"url":"https://WWW.weather.com.cn/weather/1.shtml"}`))
	if !ok || preview != "www.weather.com.cn" {
		t.Fatalf("web_fetch preview = %q ok=%v, want www.weather.com.cn", preview, ok)
	}
	if _, _, ok := ApprovalRulePreview("web_fetch", json.RawMessage(`{"url":"ftp://x/y"}`)); ok {
		t.Fatal("non-http URLs must not have a rule preview")
	}
}

// TestRememberWebFetchDomain covers the domain memory for web_fetch
// approvals: exact host, session match, and auto-approval on later calls.
func TestRememberWebFetchDomain(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, permission.NewSessionRules())

	rule, ok := rules.RememberCall("web_fetch", json.RawMessage(`{"url":"https://www.weather.com.cn/a"}`), "")
	if !ok || rule.Host != "www.weather.com.cn" {
		t.Fatalf("remember = %+v ok=%v", rule, ok)
	}

	fetch := func(url string) domain.PreparedCall {
		raw, _ := json.Marshal(map[string]string{"url": url})
		return domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_fetch", Arguments: raw}}
	}
	// Same host (different path, case) auto-approves.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: fetch("https://WWW.weather.com.cn/other")})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("remembered host: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
	// A different host still prompts.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: fetch("https://api.weather.com.cn/")})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 1 {
		t.Fatalf("unremembered host: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
}

func TestRunCmdTrustPreview(t *testing.T) {
	escalated := json.RawMessage(`{"program":"make","args":["deploy"],"sandbox_permissions":"require_escalated"}`)
	if !RunCmdTrustPreview("run_cmd", escalated) {
		t.Fatal("escalated run_cmd must offer the trust option")
	}
	if RunCmdTrustPreview("run_cmd", argsJSON(t, "make", "build")) {
		t.Fatal("non-escalated calls must not offer the trust option")
	}
	if RunCmdTrustPreview("run_cmd", json.RawMessage(`{"program":"sh","args":["-c","make deploy && echo done"],"sandbox_permissions":"require_escalated"}`)) {
		t.Fatal("compound shells must not offer the trust option")
	}
	// A simple sh -c script unwraps to its inner program and may offer it.
	if !RunCmdTrustPreview("run_cmd", json.RawMessage(`{"program":"sh","args":["-c","make deploy"],"sandbox_permissions":"require_escalated"}`)) {
		t.Fatal("simple-shell escalated calls must offer the trust option for the inner program")
	}
	if RunCmdTrustPreview("edit", json.RawMessage(`{}`)) {
		t.Fatal("non-run_cmd must not offer the trust option")
	}
}
