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
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	_ "modernc.org/sqlite"
)

const (
	// RememberedDBName is the SQLite database filename for the remembered
	// rules store, placed inside the user rules directory (~/.loom/rules).
	RememberedDBName = "remembered.db"
	// RememberedSource labels rules loaded from the remembered store.
	RememberedSource = "remembered"
	// legacyRememberedFile is the former JSON persistence file, kept only
	// for one-time migration into the SQLite store.
	legacyRememberedFile = "remembered.json"
	rememberedJustif    = "remembered from an interactive loom approval"
)

// RememberedDBPath returns the full path to the remembered rules database
// inside the given user rules directory.
func RememberedDBPath(rulesDir string) string {
	return filepath.Join(rulesDir, RememberedDBName)
}

// RememberedStore persists interactive "allow always" memories in SQLite.
// A single connection serializes writes; WAL + busy_timeout handle
// cross-process concurrency.
type RememberedStore struct {
	db   *sql.DB
	path string
}

// OpenRememberedStore opens or creates the remembered rules database and
// applies the schema. The parent directory is created on first use.
func OpenRememberedStore(ctx context.Context, path string) (*RememberedStore, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, fmt.Errorf("remembered store path is required")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("create remembered store directory: %w", err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open remembered store: %w", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	store := &RememberedStore{db: db, path: path}
	if err := store.init(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	return store, nil
}

// Path returns the database file path (diagnostics, status display).
func (s *RememberedStore) Path() string { return s.path }

// Close releases the database connection.
func (s *RememberedStore) Close() error {
	if s == nil || s.db == nil {
		return nil
	}
	return s.db.Close()
}

// RememberRule upserts an argv-prefix allow rule with its approved grant.
// The latest approval wins: re-remembering the same prefix with a different
// grant updates the stored grant (mirroring SessionRules.RememberRunCmd
// semantics).
func (s *RememberedStore) RememberRule(ctx context.Context, prefix []string, grant domain.ExecGrant) error {
	if len(prefix) == 0 {
		return fmt.Errorf("argv prefix is required")
	}
	r := &Rule{
		ArgvPrefix: prefix,
		Decision:   string(domain.DecisionAllow),
		Grant:      ruleGrantFromExec(grant),
	}
	if err := validateRuleGrant(r); err != nil {
		return err
	}
	key, err := json.Marshal(prefix)
	if err != nil {
		return fmt.Errorf("encode prefix: %w", err)
	}
	grantJSON := ""
	if r.Grant != nil {
		b, _ := json.Marshal(r.Grant)
		grantJSON = string(b)
	}
	now := formatNowUTC()
	_, err = s.db.ExecContext(ctx, `
INSERT INTO remembered_rules(prefix, grant, justification, created_at, updated_at)
VALUES (?, ?, ?, ?, ?)
ON CONFLICT(prefix) DO UPDATE SET grant = excluded.grant, updated_at = excluded.updated_at`,
		string(key), grantJSON, rememberedJustif, now, now)
	if err != nil {
		return fmt.Errorf("remember rule: %w", err)
	}
	return nil
}

// RememberDomain upserts an exact-host allow rule for web_fetch calls.
// The host is normalized (lowercase, no scheme/port). Duplicate hosts are
// idempotent.
func (s *RememberedStore) RememberDomain(ctx context.Context, host string) error {
	host, err := normalizeDomainHost(host)
	if err != nil {
		return err
	}
	now := formatNowUTC()
	_, err = s.db.ExecContext(ctx, `
INSERT INTO remembered_domains(host, justification, created_at)
VALUES (?, ?, ?)
ON CONFLICT(host) DO NOTHING`,
		host, rememberedJustif, now)
	if err != nil {
		return fmt.Errorf("remember domain: %w", err)
	}
	return nil
}

// ForgetRule removes a remembered argv-prefix rule. ok=false means the
// prefix was not in the store.
func (s *RememberedStore) ForgetRule(ctx context.Context, prefix []string) (bool, error) {
	key, err := json.Marshal(prefix)
	if err != nil {
		return false, fmt.Errorf("encode prefix: %w", err)
	}
	res, err := s.db.ExecContext(ctx, "DELETE FROM remembered_rules WHERE prefix = ?", string(key))
	if err != nil {
		return false, fmt.Errorf("forget rule: %w", err)
	}
	n, _ := res.RowsAffected()
	return n > 0, nil
}

// ForgetDomain removes a remembered domain rule. ok=false means the host
// was not in the store.
func (s *RememberedStore) ForgetDomain(ctx context.Context, host string) (bool, error) {
	host, err := normalizeDomainHost(host)
	if err != nil {
		return false, err
	}
	res, err := s.db.ExecContext(ctx, "DELETE FROM remembered_domains WHERE host = ?", host)
	if err != nil {
		return false, fmt.Errorf("forget domain: %w", err)
	}
	n, _ := res.RowsAffected()
	return n > 0, nil
}

// Load returns all remembered rules and domains as a RuleSet for merging
// into the policy evaluation chain. The Source field is set to
// RememberedSource.
func (s *RememberedStore) Load(ctx context.Context) (*RuleSet, error) {
	return queryRemembered(ctx, s.db)
}

// MigrateLegacyJSON imports a legacy remembered.json (JSON file layer)
// into the store, then renames the file aside so LoadRuleSets no longer
// picks it up. A missing file is a no-op.
func (s *RememberedStore) MigrateLegacyJSON(ctx context.Context, rulesDir string) error {
	legacyPath := filepath.Join(rulesDir, legacyRememberedFile)
	if _, err := os.Stat(legacyPath); errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err := s.ImportRuleFile(ctx, legacyPath); err != nil {
		return fmt.Errorf("parse legacy remembered.json: %w", err)
	}
	// Rename the legacy file so LoadRuleSets no longer picks it up.
	// Failure here is non-fatal: both sources will load (harmless
	// duplicates, strictest-wins dedup at evaluation).
	if err := os.Rename(legacyPath, legacyPath+".migrated"); err != nil {
		return fmt.Errorf("rename legacy file (import succeeded, duplicates are harmless): %w", err)
	}
	return nil
}

// ImportRuleFile imports the allow rules and allow domains of a
// declarative rule file (the same schema LoadRuleSets reads) into the
// store, preserving each entry's own justification and grant. Existing
// entries win (INSERT OR IGNORE) — the database is the newer source of
// truth. Self-test examples (match/not_match) are validated at parse
// time but not persisted; grant write paths are expanded and cleaned
// the same way file-layer loading does.
func (s *RememberedStore) ImportRuleFile(ctx context.Context, path string) error {
	rules, domains, err := loadRuleFile(path)
	if err != nil {
		return err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin import tx: %w", err)
	}
	defer tx.Rollback()
	now := formatNowUTC()
	for _, r := range rules {
		if domain.Decision(r.Decision) != domain.DecisionAllow {
			continue
		}
		key, err := json.Marshal(r.ArgvPrefix)
		if err != nil {
			continue
		}
		grantJSON := ""
		if r.Grant != nil {
			b, _ := json.Marshal(r.Grant)
			grantJSON = string(b)
		}
		justif := r.Justification
		if justif == "" {
			justif = rememberedJustif
		}
		if _, err := tx.ExecContext(ctx, `
INSERT OR IGNORE INTO remembered_rules(prefix, grant, justification, created_at, updated_at)
VALUES (?, ?, ?, ?, ?)`,
			string(key), grantJSON, justif, now, now); err != nil {
			return fmt.Errorf("import rule %v: %w", r.ArgvPrefix, err)
		}
	}
	for _, d := range domains {
		if domain.Decision(d.Decision) != domain.DecisionAllow {
			continue
		}
		justif := d.Justification
		if justif == "" {
			justif = rememberedJustif
		}
		if _, err := tx.ExecContext(ctx, `
INSERT OR IGNORE INTO remembered_domains(host, justification, created_at)
VALUES (?, ?, ?)`,
			d.Host, justif, now); err != nil {
			return fmt.Errorf("import domain %s: %w", d.Host, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit import: %w", err)
	}
	return nil
}

// --- read-only loading (used by AttachRules) ---

// LoadRememberedRules reads the remembered store read-only. A missing
// database yields an empty set (no remembered approvals yet).
func LoadRememberedRules(ctx context.Context, path string) (*RuleSet, error) {
	if _, err := os.Stat(path); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return &RuleSet{}, nil
		}
		return nil, fmt.Errorf("stat remembered store: %w", err)
	}
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(path)+"?mode=ro")
	if err != nil {
		return nil, fmt.Errorf("open remembered store read-only: %w", err)
	}
	defer db.Close()
	db.SetMaxOpenConns(1)
	if _, err := db.ExecContext(ctx, "PRAGMA busy_timeout = 5000"); err != nil {
		return nil, fmt.Errorf("configure remembered store read-only: %w", err)
	}
	return queryRemembered(ctx, db)
}

// --- internal helpers ---

func (s *RememberedStore) init(ctx context.Context) error {
	for _, stmt := range []string{
		"PRAGMA journal_mode = WAL",
		"PRAGMA synchronous = FULL",
		"PRAGMA busy_timeout = 5000",
	} {
		if _, err := s.db.ExecContext(ctx, stmt); err != nil {
			return fmt.Errorf("configure remembered store: %w", err)
		}
	}
	if err := s.db.PingContext(ctx); err != nil {
		return fmt.Errorf("ping remembered store: %w", err)
	}
	const schema = `
CREATE TABLE IF NOT EXISTS remembered_rules (
    prefix TEXT PRIMARY KEY,
    grant TEXT NOT NULL DEFAULT '',
    justification TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS remembered_domains (
    host TEXT PRIMARY KEY,
    justification TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return fmt.Errorf("apply remembered store schema: %w", err)
	}
	return nil
}

// queryRemembered reads both tables into a RuleSet.
func queryRemembered(ctx context.Context, db *sql.DB) (*RuleSet, error) {
	set := &RuleSet{}
	// argv-prefix rules
	rows, err := db.QueryContext(ctx, "SELECT prefix, grant, justification FROM remembered_rules")
	if err != nil {
		return nil, fmt.Errorf("query remembered rules: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var prefixJSON, grantJSON, justif string
		if err := rows.Scan(&prefixJSON, &grantJSON, &justif); err != nil {
			return nil, fmt.Errorf("scan remembered rule: %w", err)
		}
		var prefix []string
		if err := json.Unmarshal([]byte(prefixJSON), &prefix); err != nil {
			continue // skip malformed
		}
		var grant *RuleGrant
		if grantJSON != "" {
			var g RuleGrant
			if err := json.Unmarshal([]byte(grantJSON), &g); err == nil {
				grant = &g
			}
		}
		set.rules = append(set.rules, Rule{
			ArgvPrefix:    prefix,
			Decision:      string(domain.DecisionAllow),
			Justification: justif,
			Grant:         grant,
			Source:        RememberedSource,
		})
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate remembered rules: %w", err)
	}
	// domain rules
	drows, err := db.QueryContext(ctx, "SELECT host, justification FROM remembered_domains")
	if err != nil {
		return nil, fmt.Errorf("query remembered domains: %w", err)
	}
	defer drows.Close()
	for drows.Next() {
		var host, justif string
		if err := drows.Scan(&host, &justif); err != nil {
			return nil, fmt.Errorf("scan remembered domain: %w", err)
		}
		set.domains = append(set.domains, DomainRule{
			Host:          host,
			Decision:      string(domain.DecisionAllow),
			Justification: justif,
			Source:        RememberedSource,
		})
	}
	if err := drows.Err(); err != nil {
		return nil, fmt.Errorf("iterate remembered domains: %w", err)
	}
	return set, nil
}

// ruleGrantFromExec converts a domain grant into its rule-file form (nil
// for the zero grant, keeping the store clean). Moved here from rules.go
// — the JSON write path is gone but the grant↔RuleGrant conversion is
// shared by the store and validateRuleGrant.
func ruleGrantFromExec(grant domain.ExecGrant) *RuleGrant {
	if grant.IsZero() {
		return nil
	}
	rg := &RuleGrant{Unsandboxed: grant.Unsandboxed, Write: grant.WritablePaths}
	if grant.NetworkFull {
		rg.Network = "full"
	}
	return rg
}

func formatNowUTC() string {
	return time.Now().UTC().Format(time.RFC3339Nano)
}
