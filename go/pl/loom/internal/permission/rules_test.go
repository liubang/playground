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
// Created: 2026/07/26

package permission

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func writeRulesFile(t *testing.T, dir, name, content string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
}

func runCmdCall(t *testing.T, program string, args ...string) domain.PreparedCall {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"program": program, "args": args})
	if err != nil {
		t.Fatal(err)
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd", Arguments: raw},
		Risk: domain.R2,
	}
}

func TestRuleSetEvaluateStrictestWins(t *testing.T) {
	set := &RuleSet{rules: []Rule{
		{ArgvPrefix: []string{"go"}, Decision: "allow"},
		{ArgvPrefix: []string{"go", "test"}, Decision: "deny"},
		{ArgvPrefix: []string{"git"}, Decision: "allow"},
	}}
	if d, _ := set.Evaluate([]string{"go", "build", "./..."}); d != domain.DecisionAllow {
		t.Fatalf("go build = %v, want allow", d)
	}
	if d, _ := set.Evaluate([]string{"go", "test", "./..."}); d != domain.DecisionDeny {
		t.Fatalf("go test = %v, want deny (strictest wins over [go] allow)", d)
	}
	if d, _ := set.Evaluate([]string{"npm", "run"}); d != "" {
		t.Fatalf("npm = %v, want no match", d)
	}
}

func TestLoadRuleSetsLayersAndSelfTest(t *testing.T) {
	user := t.TempDir()
	project := t.TempDir()
	writeRulesFile(t, user, "a.json", `{"rules":[
		{"argv_prefix":["go","test"],"decision":"allow","justification":"tests are safe",
		 "match":[["go","test","./..."], "go test -run X ./pkg"],
		 "not_match":[["gofmt","-w","."], "go run ."]}
	]}`)
	writeRulesFile(t, project, "b.json", `{"rules":[
		{"argv_prefix":["curl"],"decision":"deny"},
		{"argv_prefix":["make"],"decision":"allow"}
	]}`)

	// Project allows disabled: the project "allow" rule must be dropped,
	// the project "deny" rule must survive (tighten-only untrusted layer).
	set, errs := LoadRuleSets(user, project, LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	if d, _ := set.Evaluate([]string{"go", "test", "./..."}); d != domain.DecisionAllow {
		t.Fatalf("user allow rule lost: %v", d)
	}
	if d, _ := set.Evaluate([]string{"curl", "x"}); d != domain.DecisionDeny {
		t.Fatalf("project deny rule lost: %v", d)
	}
	if d, _ := set.Evaluate([]string{"make"}); d != "" {
		t.Fatalf("project allow rule must be dropped by default, got %v", d)
	}

	// Project allows enabled: the make rule now applies.
	set, errs = LoadRuleSets(user, project, LoadOptions{ProjectAllows: true})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	if d, _ := set.Evaluate([]string{"make"}); d != domain.DecisionAllow {
		t.Fatalf("ProjectAllows should honor project allow rules: %v", d)
	}
}

func TestLoadRuleFileRejectsBrokenSelfTest(t *testing.T) {
	dir := t.TempDir()
	writeRulesFile(t, dir, "bad.json", `{"rules":[
		{"argv_prefix":["go","test"],"decision":"allow","match":[["go","build"]]}
	]}`)
	if _, err := loadRuleFile(filepath.Join(dir, "bad.json")); err == nil {
		t.Fatal("self-test failure must reject the file")
	}
	writeRulesFile(t, dir, "bad2.json", `{"rules":[
		{"argv_prefix":["go","test"],"decision":"allow","not_match":[["go","test","./..."]]}
	]}`)
	if _, err := loadRuleFile(filepath.Join(dir, "bad2.json")); err == nil {
		t.Fatal("not_match hitting the rule must reject the file")
	}
	writeRulesFile(t, dir, "bad3.json", `{"rules":[{"argv_prefix":["go"],"decision":"yolo"}]}`)
	if _, err := loadRuleFile(filepath.Join(dir, "bad3.json")); err == nil {
		t.Fatal("invalid decision must reject the file")
	}
}

func TestPolicyEvaluateWithRulesAndSession(t *testing.T) {
	dir := t.TempDir()
	writeRulesFile(t, dir, "rules.json", `{"rules":[
		{"argv_prefix":["/usr/bin/talos","query","result"],"decision":"allow"},
		{"argv_prefix":["rm"],"decision":"deny","justification":"destructive"}
	]}`)
	set, errs := LoadRuleSets(dir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	policy := DefaultPolicy()
	policy.Rules = set
	policy.Session = NewSessionRules()

	// Rule allow beats the R2-ask baseline.
	if d := policy.Evaluate(runCmdCall(t, "/usr/bin/talos", "query", "result", "--qid", "1")); d != domain.DecisionAllow {
		t.Fatalf("talos query result = %v, want allow", d)
	}
	// Rule deny short-circuits even an R1-rated call.
	deny := runCmdCall(t, "rm", "-rf", "/tmp/x")
	deny.Risk = domain.R1
	if d := policy.Evaluate(deny); d != domain.DecisionDeny {
		t.Fatalf("rm = %v, want deny", d)
	}
	// Session memory allows the remembered prefix.
	if _, ok := policy.Session.RememberRunCmd([]string{"go", "test", "./..."}); !ok {
		t.Fatal("go test must be rememberable")
	}
	if d := policy.Evaluate(runCmdCall(t, "go", "test", "./pl/...")); d != domain.DecisionAllow {
		t.Fatalf("remembered go test = %v, want allow", d)
	}
	// Unmatched calls fall back to the risk baseline (R2 -> ask).
	if d := policy.Evaluate(runCmdCall(t, "npm", "install")); d != domain.DecisionAsk {
		t.Fatalf("npm install = %v, want ask (baseline)", d)
	}
	// Escalated runs are never rule-eligible.
	esc, _ := json.Marshal(map[string]any{"program": "/usr/bin/talos", "args": []string{"query", "result"}, "sandbox_permissions": "require_escalated"})
	call := domain.PreparedCall{Call: domain.ToolCall{Name: "run_cmd", Arguments: esc}, Risk: domain.R3}
	if d := policy.Evaluate(call); d != domain.DecisionAsk {
		t.Fatalf("escalated run = %v, want ask (rules must not apply)", d)
	}
}

func TestDeriveRunCmdPrefixInterpreters(t *testing.T) {
	cases := []struct {
		argv []string
		want []string
		ok   bool
	}{
		{[]string{"node", "/home/u/.loom/skills/x/scripts/lx.js", "skill", "start"}, []string{"node", "/home/u/.loom/skills/x/scripts/lx.js"}, true},
		{[]string{"node", "server.js"}, []string{"node", "server.js"}, true},
		{[]string{"node", "-e", "code()"}, nil, false},
		{[]string{"node", "--eval", "code()"}, nil, false},
		{[]string{"node"}, nil, false},
		{[]string{"python3", "-c", "print(1)"}, nil, false},
		{[]string{"python3", "scripts/build.py", "--fast"}, []string{"python3", "scripts/build.py"}, true},
		{[]string{"/usr/bin/talos", "date", "resolve", "window"}, []string{"/usr/bin/talos", "date"}, true},
	}
	for _, tt := range cases {
		got, ok := DeriveRunCmdPrefix(tt.argv)
		if ok != tt.ok {
			t.Fatalf("DeriveRunCmdPrefix(%v) ok = %v, want %v", tt.argv, ok, tt.ok)
		}
		if tt.ok {
			if len(got) != len(tt.want) {
				t.Fatalf("DeriveRunCmdPrefix(%v) = %v, want %v", tt.argv, got, tt.want)
			}
			for i := range got {
				if got[i] != tt.want[i] {
					t.Fatalf("DeriveRunCmdPrefix(%v) = %v, want %v", tt.argv, got, tt.want)
				}
			}
		}
	}
}

func TestAppendRememberedRule(t *testing.T) {
	dir := t.TempDir()
	if err := AppendRememberedRule(dir, []string{"go", "test"}); err != nil {
		t.Fatal(err)
	}
	// Idempotent: appending the same prefix twice keeps one entry.
	if err := AppendRememberedRule(dir, []string{"go", "test"}); err != nil {
		t.Fatal(err)
	}
	if err := AppendRememberedRule(dir, []string{"git", "status"}); err != nil {
		t.Fatal(err)
	}
	set, errs := LoadRuleSets(dir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	if set.Size() != 2 {
		t.Fatalf("rules = %d, want 2", set.Size())
	}
	if d, _ := set.Evaluate([]string{"go", "test", "./..."}); d != domain.DecisionAllow {
		t.Fatalf("persisted rule must evaluate: %v", d)
	}
	// The file must carry the provenance justification.
	data, _ := os.ReadFile(filepath.Join(dir, RememberedRulesFile))
	if !json.Valid(data) || len(data) == 0 {
		t.Fatal("remembered.json must be valid JSON")
	}
}
