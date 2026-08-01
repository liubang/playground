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
// Created: 2026/07/27

package permission

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// TestValidateRuleGrant covers the schema-v2 grant invariants
// (PERMISSION_DESIGN §5).
func TestValidateRuleGrant(t *testing.T) {
	// os.UserHomeDir needs $HOME, which hermetic test runners (bazel)
	// do not provide.
	t.Setenv("HOME", t.TempDir())
	home, _ := os.UserHomeDir()
	cases := []struct {
		name    string
		rule    Rule
		wantErr string
	}{
		{"nil grant is fine", Rule{ArgvPrefix: []string{"go"}, Decision: "allow"}, ""},
		{"network full", Rule{ArgvPrefix: []string{"talos"}, Decision: "allow", Grant: &RuleGrant{Network: "full"}}, ""},
		{"unsandboxed", Rule{ArgvPrefix: []string{"make", "deploy"}, Decision: "allow", Grant: &RuleGrant{Unsandboxed: true}}, ""},
		{"grant on ask", Rule{ArgvPrefix: []string{"go"}, Decision: "ask", Grant: &RuleGrant{Network: "full"}}, "requires decision=allow"},
		{"grant on deny", Rule{ArgvPrefix: []string{"go"}, Decision: "deny", Grant: &RuleGrant{Unsandboxed: true}}, "requires decision=allow"},
		{"grant on empty prefix", Rule{ArgvPrefix: nil, Decision: "allow", Grant: &RuleGrant{Network: "full"}}, "empty argv_prefix"},
		{"unsandboxed with network", Rule{ArgvPrefix: []string{"x"}, Decision: "allow", Grant: &RuleGrant{Unsandboxed: true, Network: "full"}}, "mutually exclusive"},
		{"unsandboxed with write", Rule{ArgvPrefix: []string{"x"}, Decision: "allow", Grant: &RuleGrant{Unsandboxed: true, Write: []string{"/tmp"}}}, "mutually exclusive"},
		{"bad network value", Rule{ArgvPrefix: []string{"x"}, Decision: "allow", Grant: &RuleGrant{Network: "dns-only"}}, "must be \"full\""},
		{"relative write path", Rule{ArgvPrefix: []string{"x"}, Decision: "allow", Grant: &RuleGrant{Write: []string{"rel/dir"}}}, "not absolute"},
	}
	for _, tt := range cases {
		t.Run(tt.name, func(t *testing.T) {
			rule := tt.rule
			err := validateRule(&rule)
			if tt.wantErr == "" && err != nil {
				t.Fatalf("validateRule() = %v, want nil", err)
			}
			if tt.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
					t.Fatalf("validateRule() = %v, want error containing %q", err, tt.wantErr)
				}
			}
		})
	}

	// Tilde expansion + cleaning happen in place.
	r := Rule{ArgvPrefix: []string{"x"}, Decision: "allow", Grant: &RuleGrant{Write: []string{"~/.talos/"}}}
	if err := validateRule(&r); err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(home, ".talos")
	if len(r.Grant.Write) != 1 || r.Grant.Write[0] != want {
		t.Fatalf("write = %v, want [%s]", r.Grant.Write, want)
	}
}

// TestLoadRuleSetsStripsProjectGrants proves the untrusted-layer
// invariant: a checked-out repository may never loosen the sandbox, so
// project-layer grants are stripped while the rule's decision survives.
func TestLoadRuleSetsStripsProjectGrants(t *testing.T) {
	user := t.TempDir()
	project := t.TempDir()
	writeRulesFile(t, user, "u.json", `{"rules":[
		{"argv_prefix":["talos"],"decision":"allow","grant":{"network":"full"}}
	]}`)
	writeRulesFile(t, project, "p.json", `{"rules":[
		{"argv_prefix":["make"],"decision":"allow","grant":{"unsandboxed":true}}
	]}`)
	// ProjectAllows honors the project allow decision, but the grant —
	// a sandbox loosening — is still stripped.
	set, errs := LoadRuleSets(user, project, LoadOptions{ProjectAllows: true})
	if len(errs) != 1 || !strings.Contains(errs[0].Error(), "grant stripped") {
		t.Fatalf("errs = %v, want one grant-stripped warning", errs)
	}
	d, rule := set.Evaluate([]string{"make", "x"})
	if d != "allow" {
		t.Fatalf("project allow rule lost: %v", d)
	}
	if rule.Grant != nil {
		t.Fatalf("project grant must be stripped, got %+v", rule.Grant)
	}
	// The user-layer grant survives.
	d, rule = set.Evaluate([]string{"talos", "query"})
	if d != "allow" || rule.Grant == nil || rule.Grant.Network != "full" {
		t.Fatalf("user grant lost: %v %+v", d, rule.Grant)
	}
}

// TestRuleGrantExecGrant covers the rule→domain grant conversion.
func TestRuleGrantExecGrant(t *testing.T) {
	var nilGrant *RuleGrant
	if g := nilGrant.ExecGrant(); !g.IsZero() {
		t.Fatalf("nil grant = %+v, want zero", g)
	}
	g := (&RuleGrant{Network: "full", Write: []string{"/a"}}).ExecGrant()
	if !g.NetworkFull || g.Unsandboxed || len(g.WritablePaths) != 1 {
		t.Fatalf("grant = %+v", g)
	}
	if g := (&RuleGrant{Unsandboxed: true}).ExecGrant(); !g.Unsandboxed {
		t.Fatalf("grant = %+v", g)
	}
}

// TestDangerousCommand covers the heuristic danger screen
// (PERMISSION_DESIGN §6.1).
func TestDangerousCommand(t *testing.T) {
	dangerous := [][]string{
		{"dd", "if=/dev/zero", "of=/dev/disk0"},
		{"mkfs", "/dev/disk0"},
		{"shred", "secret"},
		{"rm", "-rf", "/"},
		{"rm", "-rf", "~"},
		{"rm", "-rf", "/Users"},
		{"rm", "-r", "../outside"},
		{"chmod", "-R", "777", "/etc"},
		{"git", "push", "--force", "origin", "main"},
		{"git", "push", "-f"},
		{"git", "-C", "/repo", "push", "--force"}, // global value-flag before the subcommand
		{"git", "-c", "x=y", "push", "--delete"},  // ditto
		{"git", "--git-dir", "/r/.git", "push", "-f"},
		{"/usr/bin/dd", "if=x"},
	}
	for _, argv := range dangerous {
		if reason := DangerousCommand(argv); reason == "" {
			t.Errorf("DangerousCommand(%v) = \"\", want a reason", argv)
		}
	}
	safe := [][]string{
		{"rm", "-rf", "./node_modules"},
		{"rm", "file.txt"},
		{"git", "push", "origin", "main"},
		{"git", "push", "--force-with-lease=false"}, // not the bare flag
		{"go", "test", "./..."},
		{"ls", "-la"},
		{"chmod", "644", "file"},
		nil,
	}
	for _, argv := range safe {
		if reason := DangerousCommand(argv); reason != "" {
			t.Errorf("DangerousCommand(%v) = %q, want \"\"", argv, reason)
		}
	}
}

// TestNeverModeDangerDenied checks that dangerous commands are denied
// outright in never mode (an ask would hang an unattended run forever).
func TestNeverModeDangerDenied(t *testing.T) {
	d := DefaultPolicy().Decider(ModeNever)
	raw := []byte(`{"program":"rm","args":["-rf","/"]}`)
	v := d.Evaluate(domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: raw},
		Risk: domain.R2,
	})
	if v.Decision != domain.DecisionDeny || v.Source != SourceDanger {
		t.Fatalf("rm -rf / in never = %s (%s), want deny from danger", v.Decision, v.Source)
	}
}

// TestRememberUpgradesGrant proves the latest approval wins: remembering
// the same prefix with a wider grant replaces the stored one, and v1
// grant-less persisted entries no longer block grant persistence.
func TestRememberUpgradesGrant(t *testing.T) {
	s := NewSessionRules()
	if _, ok := s.RememberRunCmd([]string{"talos", "query"}, domain.ExecGrant{}); !ok {
		t.Fatal("remember failed")
	}
	if _, ok := s.RememberRunCmd([]string{"talos", "query"}, domain.ExecGrant{NetworkFull: true}); !ok {
		t.Fatal("re-remember failed")
	}
	grant, ok := s.Match([]string{"talos", "query", "submit"})
	if !ok || !grant.NetworkFull {
		t.Fatalf("grant after upgrade = %+v ok=%v, want network grant", grant, ok)
	}

	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if err := store.RememberRule(ctx, []string{"talos"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberRule(ctx, []string{"talos"}, domain.ExecGrant{NetworkFull: true}); err != nil {
		t.Fatal(err)
	}
	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if set.Size() != 1 {
		t.Fatalf("rules = %d, want 1 (upgrade in place)", set.Size())
	}
	_, rule := set.Evaluate([]string{"talos", "query"})
	if rule.Grant == nil || rule.Grant.Network != "full" {
		t.Fatalf("persisted grant = %+v, want network full after upgrade", rule.Grant)
	}
}

// TestParseRunCmdCall covers the argv + execution-mode flag extraction.
func TestParseRunCmdCall(t *testing.T) {
	info, ok := ParseRunCmdCall([]byte(`{"program":"talos","args":["query"],"needs_network":true}`))
	if !ok || info.Escalated || !info.NeedsNetwork {
		t.Fatalf("info = %+v ok=%v", info, ok)
	}
	if len(info.Argv) != 2 || info.Argv[0] != "talos" || info.Argv[1] != "query" {
		t.Fatalf("argv = %v", info.Argv)
	}
	info, ok = ParseRunCmdCall([]byte(`{"program":"make","sandbox_permissions":"require_escalated"}`))
	if !ok || !info.Escalated || info.NeedsNetwork {
		t.Fatalf("info = %+v ok=%v", info, ok)
	}
	if _, ok := ParseRunCmdCall([]byte(`{}`)); ok {
		t.Fatal("empty program must not parse")
	}
	if _, ok := ParseRunCmdCall([]byte(`not json`)); ok {
		t.Fatal("invalid JSON must not parse")
	}
}
