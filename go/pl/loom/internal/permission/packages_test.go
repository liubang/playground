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
// Created: 2026/08/23

package permission

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// writePackageFile writes one v3 package file into dir.
func writePackageFile(t *testing.T, dir, name, content string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestLoadBuiltinPackages(t *testing.T) {
	pkgs, err := LoadBuiltinPackages()
	if err != nil {
		t.Fatalf("builtin packages must load: %v", err)
	}
	if len(pkgs) == 0 {
		t.Fatal("builtin set must not be empty")
	}
	for _, p := range pkgs {
		if !p.Grant.IsZero() {
			t.Errorf("builtin package %v must not carry a grant", p.Bind)
		}
		if p.MaxConsequence > ConsequenceConfined {
			t.Errorf("builtin package %v must not exceed confined", p.Bind)
		}
	}
}

func TestLoadPackageSetsLayers(t *testing.T) {
	userDir := t.TempDir()
	projectDir := t.TempDir()
	writePackageFile(t, userDir, "user.json", `{"packages": [
	  {"bind": {"argv_prefix": ["git", "push"]}, "decision": "allow",
	   "grant": {"network_full": true}, "consequence": "shared-state",
	   "justification": "everyday push"},
	  {"bind": {"host": "docs.example.com"}, "decision": "allow"},
	  {"bind": {"path": "/private/notes"}, "decision": "deny"}
	]}`)
	writePackageFile(t, projectDir, "proj.json", `{"packages": [
	  {"bind": {"argv_prefix": ["make", "deploy"]}, "decision": "allow",
	   "grant": {"unsandboxed": true}, "consequence": "shared-destructive"},
	  {"bind": {"argv_prefix": ["rm"]}, "decision": "deny"}
	]}`)

	// Project allows disabled: the project allow is dropped (no
	// warnings — the drop happens before grant stripping), the deny kept.
	pkgs, errs := LoadPackageSets(userDir, projectDir, LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("expected no load warnings, got %v", errs)
	}
	var hasUserPush, hasProjAllow, hasProjDeny bool
	for _, p := range pkgs {
		if p.Bind.Kind == BindArgv && len(p.Bind.Argv) == 2 && p.Bind.Argv[1] == "push" {
			hasUserPush = true
			if p.MaxConsequence != ConsequenceSharedState || !p.Grant.NetworkFull {
				t.Errorf("user push package = %+v", p)
			}
		}
		if p.Bind.Kind == BindArgv && len(p.Bind.Argv) == 2 && p.Bind.Argv[1] == "deploy" {
			hasProjAllow = true
		}
		if p.Decision == domain.DecisionDeny && p.Bind.Kind == BindArgv {
			hasProjDeny = true
		}
	}
	if !hasUserPush || !hasProjDeny {
		t.Errorf("hasUserPush=%v hasProjDeny=%v", hasUserPush, hasProjDeny)
	}
	if hasProjAllow {
		t.Error("project-layer allow must be dropped without project_allow")
	}

	// Project allows enabled: the allow survives but is tightened (grant
	// stripped, consequence capped at confined).
	pkgs, errs = LoadPackageSets(userDir, projectDir, LoadOptions{ProjectAllows: true})
	var projAllow *Package
	for i := range pkgs {
		if pkgs[i].Bind.Kind == BindArgv && len(pkgs[i].Bind.Argv) == 2 && pkgs[i].Bind.Argv[1] == "deploy" {
			projAllow = &pkgs[i]
		}
	}
	if projAllow == nil {
		t.Fatal("project allow must survive with project_allow")
	}
	if !projAllow.Grant.IsZero() || projAllow.MaxConsequence != ConsequenceConfined {
		t.Errorf("project allow must be tightened: %+v", projAllow)
	}
	_ = errs
}

func TestPackageSelfTest(t *testing.T) {
	dir := t.TempDir()
	writePackageFile(t, dir, "bad.json", `{"packages": [
	  {"bind": {"argv_prefix": ["go", "test"]}, "decision": "allow",
	   "match": ["go run ."]}
	]}`)
	_, errs := LoadPackageSets(dir, "", LoadOptions{})
	if len(errs) != 1 {
		t.Fatalf("a failing self-test must reject the file: %v", errs)
	}
}

func TestValidateGrantInvariants(t *testing.T) {
	dir := t.TempDir()
	writePackageFile(t, dir, "bad.json", `{"packages": [
	  {"bind": {"argv_prefix": ["make"]}, "decision": "allow",
	   "grant": {"unsandboxed": true, "network_full": true}}
	]}`)
	if _, errs := LoadPackageSets(dir, "", LoadOptions{}); len(errs) != 1 {
		t.Fatalf("unsandboxed+network must be rejected: %v", errs)
	}
	writePackageFile(t, dir, "bad2.json", `{"packages": [
	  {"bind": {"argv_prefix": ["make"]}, "decision": "ask",
	   "grant": {"network_full": true}}
	]}`)
	if _, errs := LoadPackageSets(dir, "", LoadOptions{}); len(errs) < 1 {
		t.Fatal("grant on a non-allow package must be rejected")
	}
}

func TestNormalizeHostPattern(t *testing.T) {
	tests := map[string]string{
		"Example.COM":              "example.com",
		"https://example.com/path": "example.com",
		"example.com:8080":         "example.com",
		"*.Example.com":            "*.example.com",
		"example.com.":             "example.com",
	}
	for in, want := range tests {
		got, err := normalizeHostPattern(in)
		if err != nil || got != want {
			t.Errorf("normalizeHostPattern(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	for _, bad := range []string{"", "*.", "a b.com", "/path"} {
		if _, err := normalizeHostPattern(bad); err == nil {
			t.Errorf("normalizeHostPattern(%q) must fail", bad)
		}
	}
}

func TestHostMatchesPattern(t *testing.T) {
	if !hostMatchesPattern("example.com", "example.com") {
		t.Error("exact host must match")
	}
	if hostMatchesPattern("example.com", "sub.example.com") {
		t.Error("exact host must not match subdomain")
	}
	if !hostMatchesPattern("*.example.com", "a.b.example.com") {
		t.Error("wildcard must match nested subdomain")
	}
	if hostMatchesPattern("*.example.com", "example.com") {
		t.Error("wildcard must not match the apex")
	}
	if hostMatchesPattern("*.example.com", "notexample.com") {
		t.Error("wildcard must not match a suffix-sharing stranger")
	}
}

func TestParseConsequence(t *testing.T) {
	for _, s := range []string{"", "confined", "local-destructive", "shared-state", "shared-destructive"} {
		if _, err := ParseConsequence(s); err != nil {
			t.Errorf("ParseConsequence(%q) = %v", s, err)
		}
	}
	if _, err := ParseConsequence("bogus"); err == nil {
		t.Error("bogus consequence must fail")
	}
}
