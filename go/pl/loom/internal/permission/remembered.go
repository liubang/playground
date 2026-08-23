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

// The remembered store: interactive "allow always" approvals persisted
// as user-scope capability packages in SQLite (schema v3 — one table,
// one row per binding). A single connection serializes writes; WAL +
// busy_timeout handle cross-process concurrency. Migration from the v2
// multi-table schema is a ONE-TIME operation performed by
// `loom rules migrate` — the store itself never auto-migrates.
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
	// RememberedDBName is the SQLite database filename, placed inside
	// the user rules directory (~/.loom/rules).
	RememberedDBName = "remembered.db"
	// RememberedSource labels packages loaded from the remembered store.
	RememberedSource = "remembered"
	// rememberedJustif is the default provenance of a stored approval.
	rememberedJustif = "remembered from an interactive loom approval"
)

// RememberedDBPath returns the full path of the remembered database
// inside the given user rules directory.
func RememberedDBPath(rulesDir string) string {
	return filepath.Join(rulesDir, RememberedDBName)
}

// RememberedStore persists user-scope capability packages.
type RememberedStore struct {
	db   *sql.DB
	path string
}

// OpenRememberedStore opens or creates the database and applies the v3
// schema. The parent directory is created on first use.
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

// bindKindNames map binding kinds to their storage tokens.
var bindKindNames = map[BindKind]string{
	BindArgv:      "argv",
	BindArgvExact: "argv_exact",
	BindHost:      "host",
	BindPath:      "path",
	BindTool:      "tool",
}

// encodeBinding renders a binding's storage key pair.
func encodeBinding(b Binding) (kind, value string, err error) {
	kind, ok := bindKindNames[b.Kind]
	if !ok {
		return "", "", fmt.Errorf("unknown binding kind %d", b.Kind)
	}
	switch b.Kind {
	case BindArgv, BindArgvExact:
		raw, err := json.Marshal(b.Argv)
		if err != nil {
			return "", "", fmt.Errorf("encode argv binding: %w", err)
		}
		return kind, string(raw), nil
	case BindHost:
		return kind, b.Host, nil
	case BindPath:
		return kind, b.Path, nil
	case BindTool:
		return kind, b.Tool, nil
	}
	return "", "", fmt.Errorf("unknown binding kind %d", b.Kind)
}

// decodeBinding restores a binding from its storage key pair.
func decodeBinding(kind, value string) (Binding, error) {
	switch kind {
	case "argv":
		var argv []string
		if err := json.Unmarshal([]byte(value), &argv); err != nil {
			return Binding{}, err
		}
		return Binding{Kind: BindArgv, Argv: argv}, nil
	case "argv_exact":
		var argv []string
		if err := json.Unmarshal([]byte(value), &argv); err != nil {
			return Binding{}, err
		}
		return Binding{Kind: BindArgvExact, Argv: argv}, nil
	case "host":
		return Binding{Kind: BindHost, Host: value}, nil
	case "path":
		return Binding{Kind: BindPath, Path: value}, nil
	case "tool":
		return Binding{Kind: BindTool, Tool: value}, nil
	}
	return Binding{}, fmt.Errorf("unknown stored binding kind %q", kind)
}

// Remember upserts one approval as a user-scope allow package. The
// latest approval wins: re-remembering the same binding updates grant,
// consequence ceiling, and justification.
func (s *RememberedStore) Remember(ctx context.Context, pkg Package) error {
	if pkg.Decision != domain.DecisionAllow {
		return fmt.Errorf("only allow packages can be remembered")
	}
	kind, value, err := encodeBinding(pkg.Bind)
	if err != nil {
		return err
	}
	grantJSON := ""
	if !pkg.Grant.IsZero() {
		b, err := json.Marshal(pkg.Grant)
		if err != nil {
			return fmt.Errorf("encode grant: %w", err)
		}
		grantJSON = string(b)
	}
	justif := pkg.Justification
	if justif == "" {
		justif = rememberedJustif
	}
	now := formatNowUTC()
	_, err = s.db.ExecContext(ctx, `
INSERT INTO remembered_packages(bind_kind, bind_value, grant, max_consequence, justification, created_at, updated_at)
VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(bind_kind, bind_value) DO UPDATE SET
    grant = excluded.grant,
    max_consequence = excluded.max_consequence,
    justification = excluded.justification,
    updated_at = excluded.updated_at`,
		kind, value, grantJSON, pkg.MaxConsequence.String(), justif, now, now)
	if err != nil {
		return fmt.Errorf("remember package: %w", err)
	}
	return nil
}

// Forget removes a remembered package. ok=false means the binding was
// not in the store.
func (s *RememberedStore) Forget(ctx context.Context, bind Binding) (bool, error) {
	kind, value, err := encodeBinding(bind)
	if err != nil {
		return false, err
	}
	res, err := s.db.ExecContext(ctx,
		"DELETE FROM remembered_packages WHERE bind_kind = ? AND bind_value = ?", kind, value)
	if err != nil {
		return false, fmt.Errorf("forget package: %w", err)
	}
	n, _ := res.RowsAffected()
	return n > 0, nil
}

// Load returns every remembered package (user scope).
func (s *RememberedStore) Load(ctx context.Context) ([]Package, error) {
	return queryRememberedPackages(ctx, s.db)
}

// LoadRememberedPackages reads the store read-only. A missing database
// yields an empty slice (no remembered approvals yet).
func LoadRememberedPackages(ctx context.Context, path string) ([]Package, error) {
	if _, err := os.Stat(path); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
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
	return queryRememberedPackages(ctx, db)
}

// --- internals ---

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
CREATE TABLE IF NOT EXISTS remembered_packages (
    bind_kind TEXT NOT NULL,
    bind_value TEXT NOT NULL,
    grant TEXT NOT NULL DEFAULT '',
    max_consequence TEXT NOT NULL DEFAULT '',
    justification TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (bind_kind, bind_value)
);`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return fmt.Errorf("apply remembered store schema: %w", err)
	}
	return nil
}

// queryRememberedPackages reads every row of the v3 table.
func queryRememberedPackages(ctx context.Context, db *sql.DB) ([]Package, error) {
	rows, err := db.QueryContext(ctx,
		"SELECT bind_kind, bind_value, grant, max_consequence, justification FROM remembered_packages")
	if err != nil {
		return nil, fmt.Errorf("query remembered packages: %w", err)
	}
	defer rows.Close()
	var out []Package
	for rows.Next() {
		var kind, value, grantJSON, consequence, justif string
		if err := rows.Scan(&kind, &value, &grantJSON, &consequence, &justif); err != nil {
			return nil, fmt.Errorf("scan remembered package: %w", err)
		}
		bind, err := decodeBinding(kind, value)
		if err != nil {
			continue // skip malformed rows rather than failing the set
		}
		p := Package{
			Bind:          bind,
			Decision:      domain.DecisionAllow,
			Justification: justif,
			Scope:         ScopeUser,
			Source:        RememberedSource,
		}
		if grantJSON != "" {
			if err := json.Unmarshal([]byte(grantJSON), &p.Grant); err != nil {
				continue
			}
		}
		c, err := ParseConsequence(consequence)
		if err != nil {
			continue
		}
		p.MaxConsequence = c
		out = append(out, p)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate remembered packages: %w", err)
	}
	return out, nil
}

func formatNowUTC() string {
	return time.Now().UTC().Format(time.RFC3339Nano)
}
