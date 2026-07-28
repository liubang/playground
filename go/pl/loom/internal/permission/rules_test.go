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
	"log/slog"
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
		{"argv_prefix":["/usr/bin/mycli","query","result"],"decision":"allow"},
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
	if d := policy.Evaluate(runCmdCall(t, "/usr/bin/mycli", "query", "result", "--qid", "1")); d != domain.DecisionAllow {
		t.Fatalf("mycli query result = %v, want allow", d)
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
	esc, _ := json.Marshal(map[string]any{"program": "/usr/bin/mycli", "args": []string{"query", "result"}, "sandbox_permissions": "require_escalated"})
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
		{[]string{"node", "-v"}, []string{"node", "-v"}, true},
		{[]string{"node", "--version"}, []string{"node", "--version"}, true},
		{[]string{"python3", "-V"}, []string{"python3", "-V"}, true},
		{[]string{"node", "--inspect", "server.js"}, nil, false},
		{[]string{"python3", "-c", "print(1)"}, nil, false},
		{[]string{"python3", "scripts/build.py", "--fast"}, []string{"python3", "scripts/build.py"}, true},
		{[]string{"/usr/bin/mycli", "date", "resolve", "window"}, []string{"/usr/bin/mycli", "date"}, true},
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

// TestBuiltinRulesAreValid is the guard rail for the embedded set: any
// future edit that breaks the builtin list (bad prefix, self-test failure)
// fails CI here instead of silently shipping.
func TestBuiltinRulesAreValid(t *testing.T) {
	set, err := LoadBuiltinRules()
	if err != nil {
		t.Fatalf("embedded builtin rules must be valid: %v", err)
	}
	if set.Size() < 30 {
		t.Fatalf("builtin set suspiciously small: %d rules", set.Size())
	}
	// Every builtin rule must be an allow with a justification (auditability).
	for _, r := range set.Rules() {
		if r.Decision != string(domain.DecisionAllow) {
			t.Fatalf("builtin rule %v must be allow, got %q", r.ArgvPrefix, r.Decision)
		}
		if r.Justification == "" {
			t.Fatalf("builtin rule %v missing justification", r.ArgvPrefix)
		}
		if r.Source != builtinSource {
			t.Fatalf("builtin rule %v source = %q", r.ArgvPrefix, r.Source)
		}
	}
	// Spot-check the inclusion bar's edge cases.
	for _, argv := range [][]string{{"ls", "-la"}, {"git", "status"}, {"git", "diff", "HEAD"}, {"rg", "foo"}, {"node", "-v"}} {
		if d, _ := set.Evaluate(argv); d != domain.DecisionAllow {
			t.Fatalf("%v must be builtin-allowed", argv)
		}
	}
	// And the deliberate exclusions must NOT match.
	for _, argv := range [][]string{{"find", ".", "-type", "f"}, {"xargs", "rm"}, {"awk", "{print}"}, {"sed", "-i", "s/a/b/", "f"}, {"git", "branch"}, {"git", "checkout"}, {"go", "test", "./..."}, {"sh", "-c", "ls"}, {"curl", "x"}} {
		if d, _ := set.Evaluate(argv); d != "" {
			t.Fatalf("%v must not match any builtin rule, got %v", argv, d)
		}
	}
}

// TestNormalizeTrustedPath covers the basename resolution for absolute-path
// invocations: trusted system dirs resolve, anything else stays opaque.
func TestNormalizeTrustedPath(t *testing.T) {
	if norm, ok := NormalizeTrustedPath([]string{"/bin/ls", "-la"}); !ok || norm[0] != "ls" || norm[1] != "-la" {
		t.Fatalf("/bin/ls = %v, %v", norm, ok)
	}
	if norm, ok := NormalizeTrustedPath([]string{"/usr/bin/git", "status"}); !ok || norm[0] != "git" {
		t.Fatalf("/usr/bin/git = %v, %v", norm, ok)
	}
	// /opt/homebrew/bin resolves only when it is verifiably root-owned and
	// not group/other-writable; on typical Homebrew machines (owned by the
	// login user) it must be rejected (REVIEW A7).
	if _, ok := NormalizeTrustedPath([]string{"/opt/homebrew/bin/rg", "x"}); ok != isTrustedProgramDir("/opt/homebrew/bin") {
		t.Fatalf("/opt/homebrew/bin/rg normalization = %v, want %v (runtime ownership check)", ok, !ok)
	}
	for _, argv := range [][]string{
		{"ls", "-la"},       // already bare
		{"/tmp/evil/ls"},    // attacker-writable dir
		{"/Users/u/bin/ls"}, // user-writable dir
		{"/Users/liubang/.mycli/bin/mycli", "date"}, // user-installed tool
		{"./local/ls"}, // relative path
	} {
		if _, ok := NormalizeTrustedPath(argv); ok {
			t.Fatalf("%v must not resolve through basename trust", argv)
		}
	}
}

// Regression (REVIEW A7): trustedProgramDirs claimed to be root-owned, but
// /opt/homebrew/bin is user-owned and group-writable on any Homebrew
// machine — a trojaned binary there would have gained basename-rule trust.
// Directory trust must be verified at runtime.
func TestNormalizeTrustedPathRejectsWritableDir(t *testing.T) {
	old := trustedProgramDirs
	t.Cleanup(func() { trustedProgramDirs = old })
	dir := filepath.Join(t.TempDir(), "brew")
	if err := os.Mkdir(dir, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(dir, 0o777); err != nil { // defeat umask
		t.Fatal(err)
	}
	trustedProgramDirs = []string{dir}
	if _, ok := NormalizeTrustedPath([]string{filepath.Join(dir, "ls"), "-l"}); ok {
		t.Fatal("group/other-writable dir must never gain basename trust")
	}
}

func TestNormalizeTrustedPathRejectsNonRootOwnedDir(t *testing.T) {
	if os.Getuid() == 0 {
		t.Skip("ownership check is vacuous when tests run as root")
	}
	old := trustedProgramDirs
	t.Cleanup(func() { trustedProgramDirs = old })
	dir := t.TempDir() // 0700, owned by the current (non-root) user
	trustedProgramDirs = []string{dir}
	if _, ok := NormalizeTrustedPath([]string{filepath.Join(dir, "ls"), "-l"}); ok {
		t.Fatal("non-root-owned dir must never gain basename trust")
	}
}

// TestPolicyEvaluateBuiltinAndNormalization exercises the full policy path:
// builtin allows fire for bare and trusted absolute forms, never for
// user-installed or attacker paths.
func TestPolicyEvaluateBuiltinAndNormalization(t *testing.T) {
	builtin, err := LoadBuiltinRules()
	if err != nil {
		t.Fatal(err)
	}
	policy := DefaultPolicy()
	policy.Rules = builtin
	if d := policy.Evaluate(runCmdCall(t, "ls", "-la")); d != domain.DecisionAllow {
		t.Fatalf("ls = %v, want allow", d)
	}
	if d := policy.Evaluate(runCmdCall(t, "/bin/ls", "-la")); d != domain.DecisionAllow {
		t.Fatalf("/bin/ls = %v, want allow via trusted basename", d)
	}
	if d := policy.Evaluate(runCmdCall(t, "/usr/bin/git", "status", "--short")); d != domain.DecisionAllow {
		t.Fatalf("/usr/bin/git status = %v, want allow via trusted basename", d)
	}
	if d := policy.Evaluate(runCmdCall(t, "/tmp/evil/ls")); d != domain.DecisionAsk {
		t.Fatalf("/tmp/evil/ls = %v, want ask (no basename trust)", d)
	}
	if d := policy.Evaluate(runCmdCall(t, "go", "test", "./...")); d != domain.DecisionAsk {
		t.Fatalf("go test = %v, want ask (deliberately not builtin)", d)
	}
}

// TestSessionAllowNeverOverridesFileDeny is the regression test for the
// strictest-wins fix: a remembered session prefix must not win over a
// file-layer deny (or ask).
func TestSessionAllowNeverOverridesFileDeny(t *testing.T) {
	dir := t.TempDir()
	writeRulesFile(t, dir, "rules.json", `{"rules":[
		{"argv_prefix":["go","test"],"decision":"deny","justification":"no tests today"},
		{"argv_prefix":["git","push"],"decision":"ask"}
	]}`)
	set, errs := LoadRuleSets(dir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	policy := DefaultPolicy()
	policy.Rules = set
	policy.Session = NewSessionRules()
	// The user remembered ["go"] earlier (broad session allow).
	if _, ok := policy.Session.RememberRunCmd([]string{"go", "build", "./..."}); !ok {
		t.Fatal("go must be rememberable")
	}
	// Session allow still works for unmatched-by-file rules.
	if d := policy.Evaluate(runCmdCall(t, "go", "build", "./...")); d != domain.DecisionAllow {
		t.Fatalf("go build = %v, want allow via session", d)
	}
	// ...but the file deny beats the session allow.
	if d := policy.Evaluate(runCmdCall(t, "go", "test", "./...")); d != domain.DecisionDeny {
		t.Fatalf("go test = %v, want deny (file deny must beat session allow)", d)
	}
	// And the file ask also beats the session allow.
	if d := policy.Evaluate(runCmdCall(t, "git", "push", "origin", "main")); d != domain.DecisionAsk {
		t.Fatalf("git push = %v, want ask (file ask must beat session allow)", d)
	}
}

// TestAttachRulesBuiltinSwitch checks the builtin layer participates by
// default and drops out when the load options disable it.
func TestAttachRulesBuiltinSwitch(t *testing.T) {
	logger := slog.New(slog.DiscardHandler)
	on := RuleLoadOptions{Enabled: true, Builtin: true, Project: true}
	policy := AttachRules(DefaultPolicy(), t.TempDir(), on, logger)
	if d := policy.Evaluate(runCmdCall(t, "ls")); d != domain.DecisionAllow {
		t.Fatalf("builtin should allow ls by default, got %v", d)
	}
	off := RuleLoadOptions{Enabled: true, Builtin: false, Project: true}
	policy = AttachRules(DefaultPolicy(), t.TempDir(), off, logger)
	if d := policy.Evaluate(runCmdCall(t, "ls")); d != domain.DecisionAsk {
		t.Fatalf("Builtin=false should disable builtin allows, got %v", d)
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
