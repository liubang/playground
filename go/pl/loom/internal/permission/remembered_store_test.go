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

package permission

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func openTestStore(t *testing.T) (*RememberedStore, context.Context) {
	t.Helper()
	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	return store, ctx
}

func TestRememberedStoreRuleLifecycle(t *testing.T) {
	store, ctx := openTestStore(t)
	if err := store.RememberRule(ctx, []string{"go", "test"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	// Idempotent: same prefix, same grant.
	if err := store.RememberRule(ctx, []string{"go", "test"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberRule(ctx, []string{"git", "status"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberRule(ctx, []string{"talos"}, domain.ExecGrant{NetworkFull: true}); err != nil {
		t.Fatal(err)
	}
	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got := len(set.Rules()); got != 3 {
		t.Fatalf("rules = %d, want 3", got)
	}
	if d, _ := set.Evaluate([]string{"go", "test", "./..."}); d != domain.DecisionAllow {
		t.Fatalf("persisted rule must evaluate: %v", d)
	}
	// Grant round-trip.
	d, rule := set.Evaluate([]string{"talos", "query"})
	if d != domain.DecisionAllow || rule.Grant == nil || rule.Grant.Network != "full" {
		t.Fatalf("persisted grant lost: %v %+v", d, rule.Grant)
	}
	// Source must be "remembered".
	if rule.Source != RememberedSource {
		t.Fatalf("source = %q, want %q", rule.Source, RememberedSource)
	}
}

func TestRememberedStoreRuleGrantUpgradeLatestWins(t *testing.T) {
	store, ctx := openTestStore(t)
	if err := store.RememberRule(ctx, []string{"talos"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	// Re-remember with a wider grant: latest wins.
	if err := store.RememberRule(ctx, []string{"talos"}, domain.ExecGrant{NetworkFull: true}); err != nil {
		t.Fatal(err)
	}
	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got := len(set.Rules()); got != 1 {
		t.Fatalf("rules = %d, want 1 (upgrade in place)", got)
	}
	_, rule := set.Evaluate([]string{"talos", "query"})
	if rule.Grant == nil || rule.Grant.Network != "full" {
		t.Fatalf("persisted grant = %+v, want network full after upgrade", rule.Grant)
	}
}

func TestRememberedStoreRuleValidation(t *testing.T) {
	store, ctx := openTestStore(t)
	// Empty prefix is rejected.
	if err := store.RememberRule(ctx, nil, domain.ExecGrant{}); err == nil {
		t.Fatal("empty prefix must fail")
	}
	if err := store.RememberRule(ctx, []string{}, domain.ExecGrant{}); err == nil {
		t.Fatal("empty prefix must fail")
	}
	// Unsandboxed+Network mutual exclusion.
	if err := store.RememberRule(ctx, []string{"cmd"}, domain.ExecGrant{Unsandboxed: true, NetworkFull: true}); err == nil {
		t.Fatal("unsandboxed+network must fail")
	}
}

func TestRememberedStoreDomainLifecycle(t *testing.T) {
	store, ctx := openTestStore(t)
	if err := store.RememberDomain(ctx, "WWW.weather.com.cn"); err != nil {
		t.Fatal(err)
	}
	// Idempotent (case-normalized).
	if err := store.RememberDomain(ctx, "www.weather.com.cn"); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberDomain(ctx, "example.com"); err != nil {
		t.Fatal(err)
	}
	// Invalid host rejected.
	if err := store.RememberDomain(ctx, "https://bad/x"); err == nil {
		t.Fatal("invalid host must fail")
	}
	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if d, _ := set.EvaluateDomain("www.weather.com.cn"); d != domain.DecisionAllow {
		t.Fatal("persisted domain must evaluate")
	}
	if d, _ := set.EvaluateDomain("example.com"); d != domain.DecisionAllow {
		t.Fatal("second persisted domain must evaluate")
	}
}

func TestRememberedStoreForget(t *testing.T) {
	store, ctx := openTestStore(t)
	if err := store.RememberRule(ctx, []string{"go", "test"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberDomain(ctx, "example.com"); err != nil {
		t.Fatal(err)
	}
	ok, err := store.ForgetRule(ctx, []string{"go", "test"})
	if err != nil || !ok {
		t.Fatalf("forget rule: ok=%v err=%v", ok, err)
	}
	ok, err = store.ForgetRule(ctx, []string{"go", "test"})
	if err != nil || ok {
		t.Fatalf("second forget: ok=%v err=%v, want false", ok, err)
	}
	ok, err = store.ForgetDomain(ctx, "example.com")
	if err != nil || !ok {
		t.Fatalf("forget domain: ok=%v err=%v", ok, err)
	}
	ok, err = store.ForgetDomain(ctx, "example.com")
	if err != nil || ok {
		t.Fatalf("second forget domain: ok=%v err=%v, want false", ok, err)
	}
}

func TestRememberedStoreReopenPersistence(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	path := RememberedDBPath(dir)
	store, err := OpenRememberedStore(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.RememberRule(ctx, []string{"bazel", "build"}, domain.ExecGrant{}); err != nil {
		store.Close()
		t.Fatal(err)
	}
	if err := store.RememberDomain(ctx, "api.example.com"); err != nil {
		store.Close()
		t.Fatal(err)
	}
	store.Close()

	// Reopen and verify.
	store2, err := OpenRememberedStore(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	defer store2.Close()
	set, err := store2.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(set.Rules()) != 1 || len(set.Domains()) != 1 {
		t.Fatalf("rules=%d domains=%d, want 1/1", len(set.Rules()), len(set.Domains()))
	}
}

func TestLoadRememberedRulesMissing(t *testing.T) {
	ctx := context.Background()
	set, err := LoadRememberedRules(ctx, "/nonexistent/remembered.db")
	if err != nil {
		t.Fatalf("missing store: %v", err)
	}
	if got := len(set.Rules()); got != 0 {
		t.Fatalf("size = %d, want 0", got)
	}
}

func TestMigrateLegacyJSON(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	// Write a legacy remembered.json with rules + domains.
	legacy := ruleFile{
		Rules: []Rule{
			{ArgvPrefix: []string{"go", "test"}, Decision: string(domain.DecisionAllow), Justification: "legacy"},
			{ArgvPrefix: []string{"rm"}, Decision: string(domain.DecisionDeny)}, // non-allow, skipped
		},
		Domains: []DomainRule{
			{Host: "www.weather.com.cn", Decision: string(domain.DecisionAllow), Justification: "legacy domain"},
		},
	}
	data, _ := json.MarshalIndent(legacy, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, legacyRememberedFile), data, 0o600); err != nil {
		t.Fatal(err)
	}

	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	if err := store.MigrateLegacyJSON(ctx, dir); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	// Legacy file must be renamed.
	if _, err := os.Stat(filepath.Join(dir, legacyRememberedFile)); err == nil {
		t.Fatal("legacy file should be renamed")
	}
	if _, err := os.Stat(filepath.Join(dir, legacyRememberedFile+".migrated")); err != nil {
		t.Fatal("migrated file should exist")
	}

	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	// Only the allow rule + allow domain should be in the store (deny skipped).
	rules := set.Rules()
	if len(rules) != 1 {
		t.Fatalf("rules = %d, want 1 (deny skipped)", len(rules))
	}
	if d, _ := set.Evaluate([]string{"go", "test", "./..."}); d != domain.DecisionAllow {
		t.Fatalf("migrated rule: %v", d)
	}
	domains := set.Domains()
	if len(domains) != 1 {
		t.Fatalf("domains = %d, want 1", len(domains))
	}
	if d, _ := set.EvaluateDomain("www.weather.com.cn"); d != domain.DecisionAllow {
		t.Fatalf("migrated domain: %v", d)
	}

	// Idempotent: second migrate is a no-op (file gone).
	if err := store.MigrateLegacyJSON(ctx, dir); err != nil {
		t.Fatalf("second migrate: %v", err)
	}
}

func TestMigrateLegacyJSONExistingDBWins(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()

	// Write a legacy file.
	legacy := ruleFile{
		Rules: []Rule{
			{ArgvPrefix: []string{"go", "test"}, Decision: string(domain.DecisionAllow)},
		},
	}
	data, _ := json.MarshalIndent(legacy, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, legacyRememberedFile), data, 0o600); err != nil {
		t.Fatal(err)
	}

	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	// Pre-existing DB entry with a grant.
	if err := store.RememberRule(ctx, []string{"go", "test"}, domain.ExecGrant{NetworkFull: true}); err != nil {
		t.Fatal(err)
	}

	// Migration: INSERT OR IGNORE — existing DB row keeps its grant.
	if err := store.MigrateLegacyJSON(ctx, dir); err != nil {
		t.Fatalf("migrate: %v", err)
	}

	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	_, rule := set.Evaluate([]string{"go", "test"})
	if rule.Grant == nil || rule.Grant.Network != "full" {
		t.Fatalf("DB grant should win over legacy: %+v", rule.Grant)
	}
}

func TestImportRuleFile(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	// Write a rule file with custom justifications, a deny entry (skipped),
	// and a domain with its own justification.
	writePath := filepath.Join(t.TempDir(), "out")
	content := fmt.Sprintf(`{
  "rules": [
    {"argv_prefix": ["dsx"], "decision": "allow", "justification": "custom: dsx needs network", "grant": {"network": "full"}, "match": ["dsx query"]},
    {"argv_prefix": ["sql"], "decision": "allow", "justification": "custom: sql needs network", "grant": {"network": "full"}, "match": ["sql -e 1"]},
    {"argv_prefix": ["rm"], "decision": "deny", "justification": "never allow rm"}
  ],
  "domains": [
    {"host": "api.example.com", "decision": "allow", "justification": "internal API"}
  ]
}`)
	ruleFile := filepath.Join(dir, "custom.json")
	if err := os.WriteFile(ruleFile, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}

	if err := store.ImportRuleFile(ctx, ruleFile); err != nil {
		t.Fatalf("import: %v", err)
	}

	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}

	// Two allow rules + one domain; deny entry skipped.
	if len(set.Rules()) != 2 {
		t.Fatalf("rules = %d, want 2 (deny skipped)", len(set.Rules()))
	}
	if len(set.Domains()) != 1 {
		t.Fatalf("domains = %d, want 1", len(set.Domains()))
	}

	// Custom justifications must be preserved.
	_, r1 := set.Evaluate([]string{"dsx", "query"})
	if r1.Justification != "custom: dsx needs network" {
		t.Fatalf("justification = %q, want custom", r1.Justification)
	}
	_, r2 := set.Evaluate([]string{"sql", "-e", "1"})
	if r2.Justification != "custom: sql needs network" {
		t.Fatalf("justification = %q, want custom", r2.Justification)
	}

	// Domain justification preserved.
	doms := set.Domains()
	if doms[0].Justification != "internal API" {
		t.Fatalf("domain justification = %q, want custom", doms[0].Justification)
	}

	// Deny entry must not appear.
	if d, _ := set.Evaluate([]string{"rm", "-rf", "/"}); d == domain.DecisionDeny {
		t.Fatal("deny rule from file must not be imported")
	}

	// Import is idempotent: re-importing does not duplicate or overwrite.
	if err := store.ImportRuleFile(ctx, ruleFile); err != nil {
		t.Fatalf("re-import: %v", err)
	}
	set2, _ := store.Load(ctx)
	if len(set2.Rules()) != 2 {
		t.Fatalf("after re-import: rules = %d, want 2", len(set2.Rules()))
	}

	// Existing DB entry wins: pre-seed a rule, then import a file with
	// the same prefix but different justification — DB keeps original.
	_ = writePath // avoid unused warning
	store2, err2 := OpenRememberedStore(ctx, RememberedDBPath(t.TempDir()))
	if err2 != nil {
		t.Fatal(err2)
	}
	defer store2.Close()
	if err := store2.RememberRule(ctx, []string{"dsx"}, domain.ExecGrant{}); err != nil {
		t.Fatal(err)
	}
	if err := store2.ImportRuleFile(ctx, ruleFile); err != nil {
		t.Fatalf("import into store2: %v", err)
	}
	s3, _ := store2.Load(ctx)
	_, r3 := s3.Evaluate([]string{"dsx", "query"})
	// The DB entry (from RememberRule) had the default justification;
	// import must not overwrite it.
	if r3.Justification != rememberedJustif {
		t.Fatalf("DB justification = %q, want %q (DB wins)", r3.Justification, rememberedJustif)
	}
}

func TestRememberedStoreToolLifecycle(t *testing.T) {
	store, ctx := openTestStore(t)
	if err := store.RememberTool(ctx, "generate_image"); err != nil {
		t.Fatal(err)
	}
	// Idempotent.
	if err := store.RememberTool(ctx, "generate_image"); err != nil {
		t.Fatal(err)
	}
	// Ineligible tools can never enter the store.
	if err := store.RememberTool(ctx, "run_cmd"); err == nil {
		t.Fatal("run_cmd must be rejected from tool memory")
	}
	if err := store.RememberTool(ctx, "edit"); err == nil {
		t.Fatal("edit must be rejected from tool memory")
	}

	set, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	tools := set.Tools()
	if len(tools) != 1 || tools[0].Name != "generate_image" {
		t.Fatalf("tools = %+v, want [generate_image]", tools)
	}
	if tools[0].Decision != string(domain.DecisionAllow) || tools[0].Source != RememberedSource {
		t.Fatalf("tool rule = %+v, want allow from remembered", tools[0])
	}
	if d, _ := set.EvaluateTool("generate_image"); d != domain.DecisionAllow {
		t.Fatalf("EvaluateTool = %v, want allow", d)
	}

	ok, err := store.ForgetTool(ctx, "generate_image")
	if err != nil || !ok {
		t.Fatalf("forget tool: ok=%v err=%v", ok, err)
	}
	ok, err = store.ForgetTool(ctx, "generate_image")
	if err != nil || ok {
		t.Fatalf("second forget tool: ok=%v err=%v, want false", ok, err)
	}
	set, err = store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(set.Tools()) != 0 {
		t.Fatalf("tools after forget = %+v, want empty", set.Tools())
	}
}

func TestRememberedStoreToolPersistenceAcrossReopen(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	path := RememberedDBPath(dir)
	store, err := OpenRememberedStore(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.RememberTool(ctx, "generate_image"); err != nil {
		store.Close()
		t.Fatal(err)
	}
	store.Close()

	// The read-only loader (AttachRules path) sees the tool rule too.
	set, err := LoadRememberedRules(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	if d, _ := set.EvaluateTool("generate_image"); d != domain.DecisionAllow {
		t.Fatalf("reopened EvaluateTool = %v, want allow", d)
	}
}

// TestLoadRememberedRulesLegacySchema covers databases written before the
// remembered_tools table existed: the read-only loader must degrade to
// "no tool rules" instead of failing the whole policy load.
func TestLoadRememberedRulesLegacySchema(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	path := RememberedDBPath(dir)
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.ExecContext(ctx, `
CREATE TABLE remembered_rules (
    prefix TEXT PRIMARY KEY,
    grant TEXT NOT NULL DEFAULT '',
    justification TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE remembered_domains (
    host TEXT PRIMARY KEY,
    justification TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);`)
	if err != nil {
		db.Close()
		t.Fatal(err)
	}
	db.Close()

	set, err := LoadRememberedRules(ctx, path)
	if err != nil {
		t.Fatalf("legacy schema must load: %v", err)
	}
	if len(set.Tools()) != 0 {
		t.Fatalf("legacy schema tools = %+v, want empty", set.Tools())
	}
}
