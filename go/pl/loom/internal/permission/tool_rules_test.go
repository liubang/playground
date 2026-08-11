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
// Created: 2026/08/07

package permission

import (
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func toolCall(t *testing.T, name string, args map[string]string) domain.PreparedCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw},
		Risk: domain.R3,
	}
}

func TestToolMemoryEligible(t *testing.T) {
	if canonical, ok := ToolMemoryEligible("generate_image"); !ok || canonical != "generate_image" {
		t.Fatalf("generate_image must be eligible, got %q, %v", canonical, ok)
	}
	// web_search's provider endpoint is pinned by deployment config, so its
	// blast radius is argument-independent and name-level memory is honest.
	if canonical, ok := ToolMemoryEligible("web_search"); !ok || canonical != "web_search" {
		t.Fatalf("web_search must be eligible, got %q, %v", canonical, ok)
	}
	// Eligibility normalizes case and whitespace and returns the canonical name.
	if canonical, ok := ToolMemoryEligible(" Generate_Image "); !ok || canonical != "generate_image" {
		t.Fatalf("normalization must yield generate_image, got %q, %v", canonical, ok)
	}
	for _, name := range []string{"run_cmd", "exec_session", "web_fetch", "edit", "write", "view_image", "", "no-such-tool"} {
		if _, ok := ToolMemoryEligible(name); ok {
			t.Errorf("ToolMemoryEligible(%q) = true, want false", name)
		}
	}
}

func TestSessionToolMemory(t *testing.T) {
	s := NewSessionRules()
	name, ok := s.RememberTool("generate_image")
	if !ok || name != "generate_image" {
		t.Fatalf("RememberTool = %q, %v", name, ok)
	}
	if !s.MatchTool("generate_image") || !s.MatchTool("GENERATE_IMAGE") {
		t.Fatal("MatchTool must hit the remembered tool (case-insensitive)")
	}
	if s.MatchTool("edit") {
		t.Fatal("unremembered tool must not match")
	}
	if len(s.Tools()) != 1 || s.Tools()[0] != "generate_image" {
		t.Fatalf("Tools = %v", s.Tools())
	}
	// Path/host/command-varying tools are never remembered by name.
	for _, bad := range []string{"run_cmd", "web_fetch", "edit", "write"} {
		if _, ok := s.RememberTool(bad); ok {
			t.Errorf("RememberTool(%q) must be refused", bad)
		}
		if s.MatchTool(bad) {
			t.Errorf("MatchTool(%q) must stay false", bad)
		}
	}
	// Forget revokes the session memory immediately.
	if !s.ForgetTool("generate_image") {
		t.Fatal("ForgetTool must report the eviction")
	}
	if s.MatchTool("generate_image") {
		t.Fatal("forgotten tool must no longer match")
	}
	if s.ForgetTool("generate_image") {
		t.Fatal("second ForgetTool must report false")
	}
}

func TestEvaluateToolStrictest(t *testing.T) {
	set := &RuleSet{tools: []ToolRule{
		{Name: "generate_image", Decision: string(domain.DecisionAllow), Justification: "approved once"},
	}}
	if d, r := set.EvaluateTool("GENERATE_IMAGE"); d != domain.DecisionAllow || r.Justification != "approved once" {
		t.Fatalf("EvaluateTool = %v (%v), want allow", d, r)
	}
	if d, _ := set.EvaluateTool("edit"); d != "" {
		t.Fatalf("unmatched tool = %v, want no opinion", d)
	}
	// A layered deny wins over the allow (strictest-wins).
	set.tools = append(set.tools, ToolRule{Name: "generate_image", Decision: string(domain.DecisionDeny)})
	if d, _ := set.EvaluateTool("generate_image"); d != domain.DecisionDeny {
		t.Fatalf("strictest = %v, want deny", d)
	}
	var nilSet *RuleSet
	if d, _ := nilSet.EvaluateTool("generate_image"); d != "" {
		t.Fatalf("nil set = %v, want no opinion", d)
	}
}

func TestToolChainEvaluation(t *testing.T) {
	policy := DefaultPolicy()
	policy.Rules = &RuleSet{}
	policy.Session = NewSessionRules()
	d := policy.Decider(ModeUnlessDangerous)

	call := toolCall(t, "generate_image", map[string]string{"prompt": "a cat"})
	// Unremembered: R3 baseline asks.
	if v := d.Evaluate(call); v.Decision != domain.DecisionAsk {
		t.Fatalf("unremembered generate_image = %s, want ask", v.Decision)
	}
	// Session memory allows.
	if _, ok := policy.Session.RememberTool("generate_image"); !ok {
		t.Fatal("remember failed")
	}
	if v := d.Evaluate(call); v.Decision != domain.DecisionAllow || v.Source != SourceSession {
		t.Fatalf("remembered generate_image = %s (%s), want allow from session", v.Decision, v.Source)
	}
	// A persisted (file/store) rule allows via the rule layer.
	policy2 := DefaultPolicy()
	policy2.Rules = &RuleSet{tools: []ToolRule{{
		Name: "generate_image", Decision: string(domain.DecisionAllow), Source: RememberedSource,
	}}}
	if v := policy2.Decider(ModeUnlessDangerous).Evaluate(call); v.Decision != domain.DecisionAllow || v.Source != SourceRule {
		t.Fatalf("ruled generate_image = %s (%s), want allow from rule", v.Decision, v.Source)
	}
}

// TestToolRuleNeverCoversExec pins the invariant that a tool-name rule can
// never bypass argv/host evaluation: exec calls and web_fetch are routed
// to their dedicated shapes before tool rules are consulted.
func TestToolRuleNeverCoversExec(t *testing.T) {
	set := &RuleSet{tools: []ToolRule{
		{Name: "run_cmd", Decision: string(domain.DecisionAllow)},
		{Name: "web_fetch", Decision: string(domain.DecisionAllow)},
	}}
	policy := DefaultPolicy()
	policy.Rules = set
	d := policy.Decider(ModeUnlessDangerous)

	raw, _ := json.Marshal(map[string]any{"program": "rm", "args": []string{"-rf", "/tmp/x"}})
	exec := domain.PreparedCall{
		Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd", Arguments: raw},
		Risk: domain.R3,
	}
	// The verdict may still be an allow — from the sandbox-backed
	// baseline — but it must never COME from the tool-name rule.
	if v := d.Evaluate(exec); v.Source == SourceRule {
		t.Fatalf("a tool-name rule must never decide an exec call, got %s (%s)", v.Decision, v.Source)
	}
	if v := d.Evaluate(webFetchCall(t, "https://evil.example/")); v.Decision == domain.DecisionAllow {
		t.Fatalf("a tool-name rule must never allow a web_fetch call, got %s (%s)", v.Decision, v.Source)
	}
}
