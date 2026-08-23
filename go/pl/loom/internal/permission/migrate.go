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

// One-time migration from the v2 rule formats (rule files with
// rules/domains/paths sections, the multi-table remembered.db, the
// legacy remembered.json) to capability packages (schema v3). This is
// invoked EXCLUSIVELY by `loom rules migrate` — nothing in the runtime
// path auto-migrates. Consequence ceilings are assigned conservatively:
// argv allows get shared-state (a remembered `git push` keeps covering
// normal pushes but NOT --force, closing the old §7.0 gradient), host /
// path / tool allows get confined (their effects never exceed it).
package permission

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// --- v2 read models (migration-only) ---

type ruleFileV2 struct {
	Rules []struct {
		ArgvPrefix    []string `json:"argv_prefix"`
		Decision      string   `json:"decision"`
		Justification string   `json:"justification,omitempty"`
		Grant         *struct {
			Network     string   `json:"network,omitempty"`
			Write       []string `json:"write,omitempty"`
			Unsandboxed bool     `json:"unsandboxed,omitempty"`
			GUIOpen     bool     `json:"gui_open,omitempty"`
		} `json:"grant,omitempty"`
	} `json:"rules"`
	Domains []struct {
		Host          string `json:"host"`
		Decision      string `json:"decision"`
		Justification string `json:"justification,omitempty"`
	} `json:"domains"`
	Paths []struct {
		Path          string `json:"path"`
		Decision      string `json:"decision"`
		Justification string `json:"justification,omitempty"`
	} `json:"paths"`
}

// MigrateReport summarizes one migration run.
type MigrateReport struct {
	FilesConverted   int
	FilesSkipped     int // already v3
	PackagesMigrated int
	DBMigrated       bool
	LegacyJSON       bool
}

// String renders the report for the CLI.
func (r MigrateReport) String() string {
	return fmt.Sprintf("files: %d converted, %d already v3; packages: %d migrated; remembered.db migrated: %v; legacy remembered.json imported: %v",
		r.FilesConverted, r.FilesSkipped, r.PackagesMigrated, r.DBMigrated, r.LegacyJSON)
}

// MigrateUserRules performs the one-time v2→v3 migration of the user
// rules directory: converts v2 rule files in place (originals kept as
// <name>.v2.bak), converts the v2 remembered.db tables (file backed up
// as remembered.db.v2.bak), and imports a legacy remembered.json into
// the store (renamed aside). Idempotent: v3 files and a v3-only
// database are left untouched.
func MigrateUserRules(ctx context.Context, rulesDir string) (MigrateReport, error) {
	var report MigrateReport
	if rulesDir == "" {
		return report, fmt.Errorf("rules dir is required")
	}
	if err := migrateRuleFiles(rulesDir, &report); err != nil {
		return report, err
	}
	dbPath := RememberedDBPath(rulesDir)
	migrated, n, err := migrateRememberedDB(ctx, dbPath)
	if err != nil {
		return report, err
	}
	report.DBMigrated = migrated
	report.PackagesMigrated += n
	legacy, n, err := migrateLegacyJSON(ctx, rulesDir)
	if err != nil {
		return report, err
	}
	report.LegacyJSON = legacy
	report.PackagesMigrated += n
	return report, nil
}

// migrateRuleFiles converts every v2 rule file in rulesDir to schema v3,
// leaving a .v2.bak copy next to it.
func migrateRuleFiles(rulesDir string, report *MigrateReport) error {
	entries, err := os.ReadDir(rulesDir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("read rules dir: %w", err)
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for _, name := range names {
		path := filepath.Join(rulesDir, name)
		data, err := os.ReadFile(path)
		if err != nil {
			return fmt.Errorf("read %s: %w", path, err)
		}
		var probe struct {
			Packages []json.RawMessage `json:"packages"`
		}
		if err := json.Unmarshal(data, &probe); err != nil {
			return fmt.Errorf("parse %s: %w", path, err)
		}
		if probe.Packages != nil {
			report.FilesSkipped++
			continue // already v3
		}
		converted, err := convertV2File(data)
		if err != nil {
			return fmt.Errorf("convert %s: %w", path, err)
		}
		if err := os.Rename(path, path+".v2.bak"); err != nil {
			return fmt.Errorf("backup %s: %w", path, err)
		}
		if err := os.WriteFile(path, converted, 0o644); err != nil {
			return fmt.Errorf("write %s: %w", path, err)
		}
		report.FilesConverted++
	}
	return nil
}

// convertV2File converts one v2 rule file to schema v3 JSON.
func convertV2File(data []byte) ([]byte, error) {
	var f ruleFileV2
	if err := json.Unmarshal(data, &f); err != nil {
		return nil, err
	}
	out := packageFileV3{}
	for _, r := range f.Rules {
		pj := packageJSON{
			Bind:          bindingJSON{ArgvPrefix: r.ArgvPrefix},
			Decision:      r.Decision,
			Justification: r.Justification,
		}
		if domain.Decision(r.Decision) == domain.DecisionAllow {
			pj.Consequence = migratedConsequence(r.ArgvPrefix).String()
		}
		if r.Grant != nil {
			g := &PackageGrant{
				Unsandboxed:   r.Grant.Unsandboxed,
				NetworkFull:   r.Grant.Network == "full",
				WritablePaths: r.Grant.Write,
				GUIOpen:       r.Grant.GUIOpen,
			}
			pj.Grant = g
		}
		out.Packages = append(out.Packages, pj)
	}
	for _, d := range f.Domains {
		out.Packages = append(out.Packages, packageJSON{
			Bind:          bindingJSON{Host: d.Host},
			Decision:      d.Decision,
			Justification: d.Justification,
		})
	}
	for _, p := range f.Paths {
		out.Packages = append(out.Packages, packageJSON{
			Bind:          bindingJSON{Path: p.Path},
			Decision:      p.Decision,
			Justification: p.Justification,
		})
	}
	body, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(body, '\n'), nil
}

// v2 remembered.db table names.
var v2RememberedTables = []string{
	"remembered_rules", "remembered_domains", "remembered_paths", "remembered_tools",
}

// migrateRememberedDB converts the v2 multi-table remembered.db to the
// v3 single-table schema. The file is backed up first; the old tables
// are dropped only after every row converts. Returns (migrated, count).
func migrateRememberedDB(ctx context.Context, dbPath string) (bool, int, error) {
	if _, err := os.Stat(dbPath); errors.Is(err, os.ErrNotExist) {
		return false, 0, nil
	}
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return false, 0, fmt.Errorf("open remembered store: %w", err)
	}
	defer db.Close()
	db.SetMaxOpenConns(1)
	for _, stmt := range []string{"PRAGMA busy_timeout = 5000"} {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			return false, 0, err
		}
	}
	// Detect the v2 tables; none means either a v3-only or an empty db.
	hasV2 := false
	for _, table := range v2RememberedTables {
		var found string
		err := db.QueryRowContext(ctx,
			"SELECT name FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&found)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return false, 0, err
		}
		if found != "" {
			hasV2 = true
		}
	}
	if !hasV2 {
		return false, 0, nil
	}
	// Backup the file before touching it.
	backup := dbPath + ".v2.bak"
	if _, err := os.Stat(backup); errors.Is(err, os.ErrNotExist) {
		data, err := os.ReadFile(dbPath)
		if err != nil {
			return false, 0, fmt.Errorf("read db for backup: %w", err)
		}
		if err := os.WriteFile(backup, data, 0o600); err != nil {
			return false, 0, fmt.Errorf("backup db: %w", err)
		}
	}
	store := &RememberedStore{db: db, path: dbPath}
	if err := store.init(ctx); err != nil {
		return false, 0, err
	}
	n := 0
	// argv rules → shared-state ceiling. Rows are buffered BEFORE any
	// write: the store uses one connection, and writing while an
	// iterator holds it would deadlock the pool.
	type v2RuleRow struct {
		prefix, grant, justif string
	}
	var ruleRows []v2RuleRow
	if err := queryV2Table(ctx, db, "remembered_rules", "prefix, grant, justification",
		func(scan func(dest ...any) error) error {
			var r v2RuleRow
			if err := scan(&r.prefix, &r.grant, &r.justif); err != nil {
				return err
			}
			ruleRows = append(ruleRows, r)
			return nil
		}); err != nil {
		return false, 0, err
	}
	for _, r := range ruleRows {
		var prefix []string
		if err := json.Unmarshal([]byte(r.prefix), &prefix); err != nil {
			continue // skip malformed rows
		}
		p := Package{
			Bind:           Binding{Kind: BindArgv, Argv: prefix},
			Decision:       domain.DecisionAllow,
			Justification:  r.justif,
			MaxConsequence: migratedConsequence(prefix),
		}
		if r.grant != "" {
			var v2g struct {
				Network     string   `json:"network,omitempty"`
				Write       []string `json:"write,omitempty"`
				Unsandboxed bool     `json:"unsandboxed,omitempty"`
				GUIOpen     bool     `json:"gui_open,omitempty"`
			}
			if err := json.Unmarshal([]byte(r.grant), &v2g); err == nil {
				p.Grant = PackageGrant{
					Unsandboxed:   v2g.Unsandboxed,
					NetworkFull:   v2g.Network == "full",
					WritablePaths: v2g.Write,
					GUIOpen:       v2g.GUIOpen,
				}
			}
		}
		if err := store.Remember(ctx, p); err != nil {
			return false, 0, err
		}
		n++
	}
	// domains / paths / tools → confined ceiling.
	simple := []struct {
		table, keyCol string
		bind          func(string) Binding
	}{
		{"remembered_domains", "host", func(v string) Binding { return Binding{Kind: BindHost, Host: v} }},
		{"remembered_paths", "path", func(v string) Binding { return Binding{Kind: BindPath, Path: v} }},
		{"remembered_tools", "name", func(v string) Binding { return Binding{Kind: BindTool, Tool: v} }},
	}
	for _, spec := range simple {
		type row struct{ value, justif string }
		var rows []row
		if err := queryV2Table(ctx, db, spec.table, spec.keyCol+", justification",
			func(scan func(dest ...any) error) error {
				var r row
				if err := scan(&r.value, &r.justif); err != nil {
					return err
				}
				rows = append(rows, r)
				return nil
			}); err != nil {
			return false, 0, err
		}
		for _, r := range rows {
			if err := store.Remember(ctx, Package{
				Bind:           spec.bind(r.value),
				Decision:       domain.DecisionAllow,
				Justification:  r.justif,
				MaxConsequence: ConsequenceConfined,
			}); err != nil {
				return false, 0, err
			}
			n++
		}
	}
	// Every row converted: drop the v2 tables.
	for _, table := range v2RememberedTables {
		if _, err := db.ExecContext(ctx, "DROP TABLE IF EXISTS "+table); err != nil {
			return false, 0, fmt.Errorf("drop v2 table %s: %w", table, err)
		}
	}
	return true, n, nil
}

// queryV2Table reads every row of a v2 table if it exists.
func queryV2Table(ctx context.Context, db *sql.DB, table, columns string, consume func(scan func(dest ...any) error) error) error {
	var found string
	err := db.QueryRowContext(ctx,
		"SELECT name FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&found)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return err
	}
	rows, err := db.QueryContext(ctx, "SELECT "+columns+" FROM "+table)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		if err := consume(rows.Scan); err != nil {
			return err
		}
	}
	return rows.Err()
}

// migratedConsequence computes the consequence ceiling for a migrated
// argv prefix by running it through the semantic deriver: a [git push]
// memory migrates to shared-state (normal pushes covered, --force still
// asks); anything the deriver cannot prove (unknown programs, dynamic
// forms) gets confined — the conservative direction.
func migratedConsequence(prefix []string) Consequence {
	e := deriveStep(ExecStep{Argv: prefix})
	if !e.Proven {
		return ConsequenceConfined
	}
	return e.Consequence
}

// legacyRememberedFile is the pre-SQLite JSON persistence file.
const legacyRememberedFile = "remembered.json"

// migrateLegacyJSON imports a legacy remembered.json (v2 file schema)
// into the v3 store, then renames the file aside. A missing file is a
// no-op.
func migrateLegacyJSON(ctx context.Context, rulesDir string) (bool, int, error) {
	legacyPath := filepath.Join(rulesDir, legacyRememberedFile)
	if _, err := os.Stat(legacyPath); errors.Is(err, os.ErrNotExist) {
		return false, 0, nil
	}
	data, err := os.ReadFile(legacyPath)
	if err != nil {
		return false, 0, fmt.Errorf("read legacy remembered.json: %w", err)
	}
	converted, err := convertV2File(data)
	if err != nil {
		return false, 0, fmt.Errorf("convert legacy remembered.json: %w", err)
	}
	var f packageFileV3
	if err := json.Unmarshal(converted, &f); err != nil {
		return false, 0, err
	}
	store, err := OpenRememberedStore(ctx, RememberedDBPath(rulesDir))
	if err != nil {
		return false, 0, err
	}
	defer store.Close()
	n := 0
	for _, pj := range f.Packages {
		p, err := pj.materialize(legacyRememberedFile)
		if err != nil || p.Decision != domain.DecisionAllow {
			continue
		}
		if err := store.Remember(ctx, p); err != nil {
			return false, 0, err
		}
		n++
	}
	if err := os.Rename(legacyPath, legacyPath+".migrated"); err != nil {
		return false, 0, fmt.Errorf("rename legacy file (import succeeded): %w", err)
	}
	return true, n, nil
}
