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
	"context"
	"encoding/json"
	"io"
	"log/slog"
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

// TestAttachRulesLoadsDomainOnlyRememberedStore is the regression test for
// a remembered store holding only host rules (no argv rules): AttachRules
// must still merge it, or every web_fetch to a remembered host would prompt
// again after a restart (Size() only counted argv rules, so a domain-only
// store was dropped as "empty").
func TestAttachRulesLoadsDomainOnlyRememberedStore(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	if err := store.RememberDomain(ctx, "www.weather.com.cn"); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	p := AttachRules(ctx, DefaultPolicy(), t.TempDir(), dir, RuleLoadOptions{
		Enabled: true,
		Builtin: true,
	}, logger)
	d, rule := p.Rules.EvaluateDomain("www.weather.com.cn")
	if d != domain.DecisionAllow {
		t.Fatalf("remembered domain not loaded after AttachRules: decision=%s rule=%+v", d, rule)
	}
}

// TestDefaultPolicyBaseline covers the non-exec baseline: workspace-
// confined built-in tools (R0–R2) never prompt in any mode; R3+ prompts
// interactively and is denied outright in never mode.
func TestDefaultPolicyBaseline(t *testing.T) {
	tests := []struct {
		risk domain.RiskLevel
		want domain.Decision
	}{
		{domain.R0, domain.DecisionAllow},
		{domain.R1, domain.DecisionAllow},
		{domain.R2, domain.DecisionAllow},
		{domain.R3, domain.DecisionAsk},
		{domain.R4, domain.DecisionAsk},
	}
	for _, mode := range []ApprovalMode{ModeOnRequest, ModeUnlessDangerous} {
		d := DefaultPolicy().Decider(mode)
		for _, tt := range tests {
			got := d.Evaluate(domain.PreparedCall{Risk: tt.risk}).Decision
			if got != tt.want {
				t.Errorf("%s Evaluate(R%d) = %s, want %s", mode, tt.risk, got, tt.want)
			}
		}
	}
}

// TestNeverModeBaselineDeniesR3: an unattended run must never produce an
// ask (it would hang forever); R3+ built-in tools are denied instead.
func TestNeverModeBaselineDeniesR3(t *testing.T) {
	d := DefaultPolicy().Decider(ModeNever)
	if got := d.Evaluate(domain.PreparedCall{Risk: domain.R2}).Decision; got != domain.DecisionAllow {
		t.Errorf("never Evaluate(R2) = %s, want allow", got)
	}
	for _, risk := range []domain.RiskLevel{domain.R3, domain.R4} {
		if got := d.Evaluate(domain.PreparedCall{Risk: risk}).Decision; got != domain.DecisionDeny {
			t.Errorf("never Evaluate(R%d) = %s, want deny", risk, got)
		}
	}
}

// TestMCPToolsKeepApproval: third-party MCP tools are not workspace-
// confined, so they keep per-call approvals in interactive modes and are
// denied in never mode.
func TestMCPToolsKeepApproval(t *testing.T) {
	mcpCall := domain.PreparedCall{
		Call:       domain.ToolCall{Name: "mcp__srv__do"},
		Risk:       domain.R2,
		Definition: domain.ToolDefinition{Source: domain.ToolSourceMCP},
	}
	if got := DefaultPolicy().Decider(ModeOnRequest).Evaluate(mcpCall).Decision; got != domain.DecisionAsk {
		t.Errorf("on-request MCP R2 = %s, want ask", got)
	}
	if got := DefaultPolicy().Decider(ModeNever).Evaluate(mcpCall).Decision; got != domain.DecisionDeny {
		t.Errorf("never MCP R2 = %s, want deny", got)
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
	d := p.Decider(ModeOnRequest)
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
	call := runCmdPrepared(t, "talos", "query", "submit")
	raw := []byte(`{"program":"talos","args":["query","submit"],"needs_network":true}`)
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

// --- writable_paths declarations (scoped write widening) ---

// runCmdWritablePrepared builds a run_cmd prepared call declaring
// writable_paths, mirroring the canonical arguments the tool produces.
func runCmdWritablePrepared(t *testing.T, program string, writable []string, args ...string) domain.PreparedCall {
	t.Helper()
	raw, err := json.Marshal(map[string]any{
		"program":        program,
		"args":           args,
		"writable_paths": writable,
	})
	if err != nil {
		t.Fatal(err)
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: raw},
		Risk: domain.R3,
	}
}

// TestWritablePathsBaselinePerMode: a declared writable-path widening asks
// in every interactive mode (the sandbox can no longer bound the blast
// radius) with a SCOPED grant stamped — never the unsandboxed grant — and
// is denied outright in never mode.
func TestWritablePathsBaselinePerMode(t *testing.T) {
	writable := []string{"/Users/test/Library/Logs/dsx"}

	d := DefaultPolicy().Decider(ModeUnlessDangerous)
	v := d.Evaluate(runCmdWritablePrepared(t, "dsx", writable, "env", "production"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("writable_paths in unless-dangerous = %s, want ask", v.Decision)
	}
	if v.Grant.Unsandboxed {
		t.Fatalf("grant = %+v, want sandboxed writable-path grant (never unsandboxed)", v.Grant)
	}
	if len(v.Grant.WritablePaths) != 1 || v.Grant.WritablePaths[0] != writable[0] {
		t.Fatalf("grant.WritablePaths = %v, want %v", v.Grant.WritablePaths, writable)
	}

	d = DefaultPolicy().Decider(ModeOnRequest)
	v = d.Evaluate(runCmdWritablePrepared(t, "dsx", writable, "env", "production"))
	if v.Decision != domain.DecisionAsk || len(v.Grant.WritablePaths) != 1 {
		t.Fatalf("writable_paths in on-request = %s %+v, want ask with the scoped grant", v.Decision, v.Grant)
	}

	d = DefaultPolicy().Decider(ModeNever)
	if v = d.Evaluate(runCmdWritablePrepared(t, "dsx", writable, "env", "production")); v.Decision != domain.DecisionDeny {
		t.Fatalf("writable_paths in never = %s, want deny", v.Decision)
	}
}

// TestAllowGrantCoversWritablePaths: coverage is exact per declared path —
// a granted directory covers requests beneath it, a sibling does not, and
// only an unsandboxed grant answers everything.
func TestAllowGrantCoversWritablePaths(t *testing.T) {
	info := RunCmdCall{Argv: []string{"dsx"}, WritablePaths: []string{"/Users/test/Library/Logs/dsx"}}
	cases := []struct {
		name  string
		grant domain.ExecGrant
		want  bool
	}{
		{"zero grant", domain.ExecGrant{}, false},
		{"exact path", domain.ExecGrant{WritablePaths: []string{"/Users/test/Library/Logs/dsx"}}, true},
		{"granted parent", domain.ExecGrant{WritablePaths: []string{"/Users/test/Library/Logs"}}, true},
		{"granted sibling", domain.ExecGrant{WritablePaths: []string{"/Users/test/Library/Caches"}}, false},
		{"prefix lookalike", domain.ExecGrant{WritablePaths: []string{"/Users/test/Library/Logs/dsx-backup"}}, false},
		{"unsandboxed covers all", domain.ExecGrant{Unsandboxed: true}, true},
	}
	for _, tc := range cases {
		if got := AllowGrantCovers(tc.grant, info); got != tc.want {
			t.Errorf("%s: AllowGrantCovers = %v, want %v", tc.name, got, tc.want)
		}
	}

	// Every declared path must be covered independently.
	two := RunCmdCall{Argv: []string{"dsx"}, WritablePaths: []string{"/a/logs", "/b/config"}}
	if AllowGrantCovers(domain.ExecGrant{WritablePaths: []string{"/a"}}, two) {
		t.Error("partial coverage must not satisfy a two-path declaration")
	}
	if !AllowGrantCovers(domain.ExecGrant{WritablePaths: []string{"/a", "/b"}}, two) {
		t.Error("full coverage must satisfy a two-path declaration")
	}
}

// TestWritablePathsSessionMemory: an "allow always" on a writable-path ask
// remembers the argv prefix WITH the scoped grant, so later calls under
// the same prefix and equal-or-narrower paths resolve silently — while a
// WIDER path declaration falls through to the baseline ask.
func TestWritablePathsSessionMemory(t *testing.T) {
	session := NewSessionRules()
	p := DefaultPolicy()
	p.Session = session
	d := p.Decider(ModeUnlessDangerous)

	first := runCmdWritablePrepared(t, "dsx", []string{"/Users/test/Library/Logs/dsx"}, "login")
	if v := d.Evaluate(first); v.Decision != domain.DecisionAsk {
		t.Fatalf("first call = %s, want ask before any memory", v.Decision)
	}

	info, ok := ExecInfoOf(first)
	if !ok {
		t.Fatal("ExecInfoOf failed")
	}
	if _, ok := session.RememberRunCmd(info, DeclaredGrant(info)); !ok {
		t.Fatal("RememberRunCmd failed")
	}

	// Same prefix + same paths: silent allow carrying the scoped grant.
	v := d.Evaluate(runCmdWritablePrepared(t, "dsx", []string{"/Users/test/Library/Logs/dsx"}, "table", "inspect"))
	if v.Decision != domain.DecisionAllow || v.Source != SourceSession {
		t.Fatalf("remembered call = %s (%s), want allow from session", v.Decision, v.Source)
	}
	if v.Grant.Unsandboxed || len(v.Grant.WritablePaths) != 1 {
		t.Fatalf("remembered grant = %+v, want scoped writable grant", v.Grant)
	}

	// Same prefix + a WIDER path: the scoped memory must not answer it.
	v = d.Evaluate(runCmdWritablePrepared(t, "dsx", []string{"/Users/test/Library/Logs"}, "login"))
	if v.Decision != domain.DecisionAsk || v.Source != SourceBaseline {
		t.Fatalf("widened call = %s (%s), want baseline ask", v.Decision, v.Source)
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
	if _, ok := session.RememberRunCmd(RunCmdCall{Argv: []string{"go", "test", "./..."}}, domain.ExecGrant{}); !ok {
		t.Fatal("remember failed")
	}
	p := Policy{Rules: rules, Session: session}
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
	if _, ok := session.RememberRunCmd(RunCmdCall{Argv: []string{"talos", "query"}}, grant); !ok {
		t.Fatal("remember failed")
	}
	p := Policy{Session: session}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "talos", "query", "submit"))
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
		ArgvPrefix: []string{"talos"},
		Decision:   string(domain.DecisionAllow),
		Grant:      &RuleGrant{Network: "full"},
	}}}
	p := Policy{Rules: rules}
	v := p.Decider(ModeOnRequest).Evaluate(runCmdPrepared(t, "talos", "diag", "check"))
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
	p := Policy{Session: session}
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
	p := Policy{Rules: rules}
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
