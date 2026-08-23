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
	"context"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// openTestStore opens a remembered store in a temp dir.
func openTestStore(t *testing.T) *RememberedStore {
	t.Helper()
	store, err := OpenRememberedStore(context.Background(), filepath.Join(t.TempDir(), "remembered.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { store.Close() })
	return store
}

func TestRememberedStoreRoundTrip(t *testing.T) {
	ctx := context.Background()
	store := openTestStore(t)

	pkgs := []Package{
		{
			Bind:           Binding{Kind: BindArgv, Argv: []string{"git", "push"}},
			Decision:       domain.DecisionAllow,
			Grant:          PackageGrant{NetworkFull: true},
			MaxConsequence: ConsequenceSharedState,
			Justification:  "everyday push",
		},
		{
			Bind:           Binding{Kind: BindArgvExact, Argv: []string{"sudo", "rm", "-rf", "/"}},
			Decision:       domain.DecisionAllow,
			Grant:          PackageGrant{Unsandboxed: true},
			MaxConsequence: ConsequenceSharedDestructive,
		},
		{
			Bind:           Binding{Kind: BindHost, Host: "docs.example.com"},
			Decision:       domain.DecisionAllow,
			MaxConsequence: ConsequenceConfined,
		},
		{
			Bind:           Binding{Kind: BindPath, Path: "/Users/x/notes"},
			Decision:       domain.DecisionAllow,
			MaxConsequence: ConsequenceConfined,
		},
		{
			Bind:           Binding{Kind: BindTool, Tool: "generate_image"},
			Decision:       domain.DecisionAllow,
			MaxConsequence: ConsequenceConfined,
		},
	}
	for _, p := range pkgs {
		if err := store.Remember(ctx, p); err != nil {
			t.Fatalf("remember %v: %v", p.Bind, err)
		}
	}
	loaded, err := store.Load(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded) != len(pkgs) {
		t.Fatalf("loaded %d packages, want %d", len(loaded), len(pkgs))
	}
	byKind := map[BindKind]Package{}
	for _, p := range loaded {
		byKind[p.Bind.Kind] = p
	}
	if got := byKind[BindArgv]; got.MaxConsequence != ConsequenceSharedState || !got.Grant.NetworkFull {
		t.Errorf("argv package = %+v", got)
	}
	if got := byKind[BindArgvExact]; !got.Grant.Unsandboxed {
		t.Errorf("exact package = %+v", got)
	}

	// Latest approval wins on the same binding.
	if err := store.Remember(ctx, Package{
		Bind:           Binding{Kind: BindArgv, Argv: []string{"git", "push"}},
		Decision:       domain.DecisionAllow,
		Grant:          PackageGrant{Unsandboxed: true},
		MaxConsequence: ConsequenceSharedDestructive,
	}); err != nil {
		t.Fatal(err)
	}
	loaded, _ = store.Load(ctx)
	for _, p := range loaded {
		if p.Bind.Kind == BindArgv {
			if !p.Grant.Unsandboxed || p.MaxConsequence != ConsequenceSharedDestructive {
				t.Errorf("upsert must replace grant and ceiling: %+v", p)
			}
		}
	}

	// Forget.
	ok, err := store.Forget(ctx, Binding{Kind: BindHost, Host: "docs.example.com"})
	if err != nil || !ok {
		t.Fatalf("forget host = %v, %v", ok, err)
	}
	ok, _ = store.Forget(ctx, Binding{Kind: BindHost, Host: "absent.example.com"})
	if ok {
		t.Fatal("forgetting an absent binding must report ok=false")
	}
}

func TestRememberedStoreReadOnly(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()
	path := filepath.Join(dir, "remembered.db")
	store, err := OpenRememberedStore(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Remember(ctx, Package{
		Bind:           Binding{Kind: BindArgv, Argv: []string{"go", "test"}},
		Decision:       domain.DecisionAllow,
		MaxConsequence: ConsequenceConfined,
	}); err != nil {
		t.Fatal(err)
	}
	store.Close()

	loaded, err := LoadRememberedPackages(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded) != 1 || loaded[0].Scope != ScopeUser {
		t.Fatalf("read-only load = %+v", loaded)
	}
	// A missing database yields an empty set.
	loaded, err = LoadRememberedPackages(ctx, filepath.Join(dir, "absent.db"))
	if err != nil || len(loaded) != 0 {
		t.Fatalf("missing db = %v, %v", loaded, err)
	}
}

func TestRememberedStoreRejectsNonAllow(t *testing.T) {
	store := openTestStore(t)
	if err := store.Remember(context.Background(), Package{
		Bind:     Binding{Kind: BindArgv, Argv: []string{"rm"}},
		Decision: domain.DecisionDeny,
	}); err == nil {
		t.Fatal("remembering a deny package must fail")
	}
}

func TestMigrateV2DB(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "remembered.db")
	// Build a v2-shaped database by hand.
	store, err := OpenRememberedStore(ctx, dbPath)
	if err != nil {
		t.Fatal(err)
	}
	for _, stmt := range []string{
		`CREATE TABLE remembered_rules (prefix TEXT PRIMARY KEY, grant TEXT NOT NULL DEFAULT '', justification TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL)`,
		`INSERT INTO remembered_rules VALUES ('["git","push"]', '{"network":"full"}', 'old approval', 'now', 'now')`,
		`CREATE TABLE remembered_domains (host TEXT PRIMARY KEY, justification TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL)`,
		`INSERT INTO remembered_domains VALUES ('docs.example.com', 'old host', 'now')`,
	} {
		if _, err := store.db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}
	store.Close()

	migrated, n, err := migrateRememberedDB(ctx, dbPath)
	if err != nil || !migrated || n != 2 {
		t.Fatalf("migrate = %v, %d, %v", migrated, n, err)
	}
	loaded, err := LoadRememberedPackages(ctx, dbPath)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded) != 2 {
		t.Fatalf("loaded = %d, want 2", len(loaded))
	}
	for _, p := range loaded {
		if p.Bind.Kind == BindArgv && p.MaxConsequence != ConsequenceSharedState {
			t.Errorf("migrated argv package ceiling = %s, want shared-state", p.MaxConsequence)
		}
		if p.Bind.Kind == BindHost && p.MaxConsequence != ConsequenceConfined {
			t.Errorf("migrated host package ceiling = %s, want confined", p.MaxConsequence)
		}
	}
	// Idempotent: no v2 tables left.
	migrated, _, err = migrateRememberedDB(ctx, dbPath)
	if err != nil || migrated {
		t.Fatalf("second migrate = %v, %v (must be a no-op)", migrated, err)
	}
}

func TestMigrateV2RuleFile(t *testing.T) {
	v2 := []byte(`{"rules": [{
	  "argv_prefix": ["go", "test"],
	  "decision": "allow",
	  "grant": {"network": "full"},
	  "justification": "tests with module downloads"
	}], "domains": [{"host": "webhook.site", "decision": "deny"}]}`)
	converted, err := convertV2File(v2)
	if err != nil {
		t.Fatal(err)
	}
	pkgs, err := parsePackageFile(converted, "test")
	if err != nil {
		t.Fatalf("converted file must parse as v3: %v\n%s", err, converted)
	}
	if len(pkgs) != 2 {
		t.Fatalf("converted = %d packages, want 2", len(pkgs))
	}
	var argvPkg *Package
	for i := range pkgs {
		if pkgs[i].Bind.Kind == BindArgv {
			argvPkg = &pkgs[i]
		}
	}
	if argvPkg == nil || !argvPkg.Grant.NetworkFull || argvPkg.MaxConsequence != ConsequenceConfined {
		t.Errorf("converted argv package = %+v (go test must migrate to confined)", argvPkg)
	}
}
