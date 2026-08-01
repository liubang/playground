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
// Created: 2026/07/22 21:10

package permission

import (
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func runCmdPrepared(t *testing.T, program string, args ...string) domain.PreparedCall {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"program": program, "args": args})
	if err != nil {
		t.Fatal(err)
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: raw},
		Risk: domain.R2,
	}
}

func TestDefaultPolicyUnlessTrustedBaseline(t *testing.T) {
	p := DefaultPolicy()
	d := p.Decider(ModeUnlessTrusted)

	tests := []struct {
		risk domain.RiskLevel
		want domain.Decision
	}{
		{domain.R0, domain.DecisionAllow},
		{domain.R1, domain.DecisionAllow},
		{domain.R2, domain.DecisionAsk},
		{domain.R3, domain.DecisionAsk},
		{domain.R4, domain.DecisionDeny},
	}

	for _, tt := range tests {
		got := d.Evaluate(domain.PreparedCall{Risk: tt.risk}).Decision
		if got != tt.want {
			t.Errorf("Evaluate(R%d) = %s, want %s", tt.risk, got, tt.want)
		}
	}
}

func TestPolicyAutoApproveDisabled(t *testing.T) {
	p := Policy{AutoApproveR1: false, AskR2: true, DenyR4: true}
	got := p.Decider(ModeUnlessTrusted).Evaluate(domain.PreparedCall{Risk: domain.R0}).Decision
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R0) with AutoApproveR1=false = %s, want deny", got)
	}
}

func TestPolicyDenyR4Disabled(t *testing.T) {
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: false}
	got := p.Decider(ModeUnlessTrusted).Evaluate(domain.PreparedCall{Risk: domain.R4}).Decision
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R4) with DenyR4=false = %s, want deny", got)
	}
}

func TestPolicyR3AlwaysAsk(t *testing.T) {
	p := DefaultPolicy()
	got := p.Decider(ModeUnlessTrusted).Evaluate(domain.PreparedCall{Risk: domain.R3}).Decision
	if got != domain.DecisionAsk {
		t.Errorf("Evaluate(R3) = %s, want ask", got)
	}
}

func TestPolicyAskR2Disabled(t *testing.T) {
	p := Policy{AutoApproveR1: true, AskR2: false, DenyR4: true}
	got := p.Decider(ModeUnlessTrusted).Evaluate(domain.PreparedCall{Risk: domain.R2}).Decision
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R2) with AskR2=false = %s, want deny", got)
	}
}

// --- on-request baseline (PERMISSION_DESIGN §4.3) ---

func TestOnRequestAutoAllowsSandboxedRunCmd(t *testing.T) {
	d := DefaultPolicy().Decider(ModeOnRequest)
	v := d.Evaluate(runCmdPrepared(t, "make", "build"))
	if v.Decision != domain.DecisionAllow {
		t.Fatalf("sandboxed run_cmd in on-request = %s, want allow", v.Decision)
	}
	if !v.Grant.IsZero() {
		t.Errorf("grant = %+v, want zero (default sandbox)", v.Grant)
	}
	if v.Source != SourceBaseline {
		t.Errorf("source = %q, want %q", v.Source, SourceBaseline)
	}
}

// TestExecRequestTypedContractPreferred proves the signed ExecRequest is
// authoritative over the raw arguments: a prepared call whose JSON names a
// harmless command but whose signed contract is dangerous must be
// classified by the contract (REVIEW M17).
func TestExecRequestTypedContractPreferred(t *testing.T) {
	d := DefaultPolicy().Decider(ModeNever)
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "exec_session", Arguments: []byte(`{"program":"true"}`)},
		Risk: domain.R2,
		ExecRequest: &domain.ExecRequest{
			Argv: []string{"rm", "-rf", "/"},
		},
	}
	v := d.Evaluate(call)
	if v.Decision != domain.DecisionDeny || v.Source != SourceDanger {
		t.Fatalf("dangerous ExecRequest in never = %s (%s), want deny from danger", v.Decision, v.Source)
	}
}

// TestExecSessionMatchesArgvRules proves exec_session calls flow through
// the same argv-prefix rules as run_cmd via the typed contract.
func TestExecSessionMatchesArgvRules(t *testing.T) {
	p := DefaultPolicy()
	p.Rules = &RuleSet{rules: []Rule{{ArgvPrefix: []string{"git", "status"}, Decision: "allow"}}}
	d := p.Decider(ModeUnlessTrusted)
	call := domain.PreparedCall{
		Call:        domain.ToolCall{Name: "exec_session", Arguments: []byte(`{"program":"git","args":["status"]}`)},
		Risk:        domain.R2,
		ExecRequest: &domain.ExecRequest{Argv: []string{"git", "status"}},
	}
	v := d.Evaluate(call)
	if v.Decision != domain.DecisionAllow || v.Source != SourceRule {
		t.Fatalf("exec_session git status = %s (%s), want allow from rule", v.Decision, v.Source)
	}
}

func TestOnRequestNeedsNetworkAsksWithNetworkGrant(t *testing.T) {
	d := DefaultPolicy().Decider(ModeOnRequest)
	call := runCmdPrepared(t, "mycli", "query", "submit")
	raw := []byte(`{"program":"mycli","args":["query","submit"],"needs_network":true}`)
	call.Call.Arguments = raw
	v := d.Evaluate(call)
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("needs_network in on-request = %s, want ask", v.Decision)
	}
	if !v.Grant.NetworkFull || v.Grant.Unsandboxed {
		t.Errorf("grant = %+v, want sandboxed network grant", v.Grant)
	}
}

func TestOnRequestEscalatedAsksWithUnsandboxedGrant(t *testing.T) {
	d := DefaultPolicy().Decider(ModeOnRequest)
	call := runCmdPrepared(t, "make", "deploy")
	call.Call.Arguments = []byte(`{"program":"make","args":["deploy"],"sandbox_permissions":"require_escalated","justification":"needs ssh"}`)
	call.Risk = domain.R3
	v := d.Evaluate(call)
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("escalated in on-request = %s, want ask", v.Decision)
	}
	if !v.Grant.Unsandboxed {
		t.Errorf("grant = %+v, want unsandboxed (approval executes outside the sandbox once)", v.Grant)
	}
}

func TestNeverModeDeniesEscalatedAndGrantsNetwork(t *testing.T) {
	d := DefaultPolicy().Decider(ModeNever)

	escalated := runCmdPrepared(t, "make", "deploy")
	escalated.Call.Arguments = []byte(`{"program":"make","args":["deploy"],"sandbox_permissions":"require_escalated","justification":"x"}`)
	escalated.Risk = domain.R3
	if v := d.Evaluate(escalated); v.Decision != domain.DecisionDeny {
		t.Errorf("escalated in never = %s, want deny", v.Decision)
	}

	needsNet := runCmdPrepared(t, "go", "mod", "download")
	needsNet.Call.Arguments = []byte(`{"program":"go","args":["mod","download"],"needs_network":true}`)
	v := d.Evaluate(needsNet)
	if v.Decision != domain.DecisionAllow || !v.Grant.NetworkFull {
		t.Errorf("needs_network in never = %s %+v, want allow with network grant", v.Decision, v.Grant)
	}

	if v := d.Evaluate(runCmdPrepared(t, "go", "build", "./...")); v.Decision != domain.DecisionAllow {
		t.Errorf("sandboxed in never = %s, want allow", v.Decision)
	}
}

// --- chain ordering ---

func TestChainRuleDenyBeatsSessionAllow(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{rules: []Rule{{
		ArgvPrefix: []string{"go", "test"},
		Decision:   string(domain.DecisionDeny),
	}}})
	session := NewSessionRules()
	if _, ok := session.RememberRunCmd([]string{"go", "test", "./..."}, domain.ExecGrant{}); !ok {
		t.Fatal("remember failed")
	}
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true, Rules: rules, Session: session}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "go", "test", "./..."))
	if v.Decision != domain.DecisionDeny {
		t.Fatalf("file-layer deny must beat session memory, got %s", v.Decision)
	}
	if v.Source != SourceRule {
		t.Errorf("source = %q, want %q", v.Source, SourceRule)
	}
}

func TestChainSessionAllowCarriesGrant(t *testing.T) {
	session := NewSessionRules()
	grant := domain.ExecGrant{NetworkFull: true}
	if _, ok := session.RememberRunCmd([]string{"mycli", "query"}, grant); !ok {
		t.Fatal("remember failed")
	}
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true, Session: session}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "mycli", "query", "submit"))
	if v.Decision != domain.DecisionAllow {
		t.Fatalf("session match = %s, want allow", v.Decision)
	}
	if !v.Grant.NetworkFull {
		t.Errorf("grant = %+v, want network grant from session memory", v.Grant)
	}
	if v.Source != SourceSession {
		t.Errorf("source = %q, want %q", v.Source, SourceSession)
	}
}

func TestChainRuleAllowCarriesGrant(t *testing.T) {
	rules := &RuleSet{rules: []Rule{{
		ArgvPrefix: []string{"mycli"},
		Decision:   string(domain.DecisionAllow),
		Grant:      &RuleGrant{Network: "full"},
	}}}
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true, Rules: rules}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "mycli", "diag", "check"))
	if v.Decision != domain.DecisionAllow || !v.Grant.NetworkFull {
		t.Fatalf("rule grant = %s %+v, want allow with network grant", v.Decision, v.Grant)
	}
	if v.Source != SourceRule {
		t.Errorf("source = %q, want %q", v.Source, SourceRule)
	}
}

func TestChainDangerBeatsSessionAndBaseline(t *testing.T) {
	session := NewSessionRules()
	// rm can never be remembered via DeriveRunCmdPrefix, so simulate a
	// remembered dangerous-looking prefix with a file rule absent.
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true, Session: session}
	d := p.Decider(ModeOnRequest)
	v := d.Evaluate(runCmdPrepared(t, "rm", "-rf", "/"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("rm -rf / in on-request = %s, want ask (danger screen)", v.Decision)
	}
	if v.Source != SourceDanger {
		t.Errorf("source = %q, want %q", v.Source, SourceDanger)
	}
}

func TestChainRuleAllowBeatsDanger(t *testing.T) {
	rules := &RuleSet{rules: []Rule{{
		ArgvPrefix: []string{"git", "push"},
		Decision:   string(domain.DecisionAllow),
	}}}
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true, Rules: rules}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "git", "push", "--force", "origin", "main"))
	if v.Decision != domain.DecisionAllow {
		t.Fatalf("explicit allow rule must beat the danger screen, got %s", v.Decision)
	}
}

func TestEmptyChainFailsClosed(t *testing.T) {
	var c Chain
	v := c.Evaluate(runCmdPrepared(t, "ls"))
	if v.Decision != domain.DecisionDeny {
		t.Fatalf("empty chain = %s, want deny (fail closed)", v.Decision)
	}
}
