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
// Created: 2026/08/06

package session

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestUpsertWorkspaceCreatesThenReusesByRoot(t *testing.T) {
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ctx := context.Background()

	root := filepath.Join(t.TempDir(), "proj")
	mkWorkspace := func(name string) domain.Workspace {
		return domain.Workspace{ID: domain.NewWorkspaceID(), Name: name, RootPath: root}
	}

	first, err := store.UpsertWorkspace(ctx, mkWorkspace("proj"))
	if err != nil {
		t.Fatalf("UpsertWorkspace: %v", err)
	}
	if first.ID.IsZero() || first.Name != "proj" || first.RootPath != root {
		t.Fatalf("unexpected workspace: %+v", first)
	}

	// Same canonical root registers again → reuse the stored ID (no dup).
	second, err := store.UpsertWorkspace(ctx, mkWorkspace("proj-renamed"))
	if err != nil {
		t.Fatalf("UpsertWorkspace reuse: %v", err)
	}
	if second.ID != first.ID {
		t.Fatalf("reuse returned different ID: %s vs %s", second.ID, first.ID)
	}
	// A non-empty incoming name refreshes the stored name.
	if second.Name != "proj-renamed" {
		t.Fatalf("name not refreshed: %q", second.Name)
	}

	// Distinct root → distinct workspace.
	other, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID: domain.NewWorkspaceID(), Name: "other", RootPath: filepath.Join(t.TempDir(), "other"),
	})
	if err != nil {
		t.Fatalf("UpsertWorkspace other: %v", err)
	}
	if other.ID == first.ID {
		t.Fatal("distinct roots must not share an ID")
	}
}

func TestWorkspaceLookupByIDAndRoot(t *testing.T) {
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ctx := context.Background()
	root := filepath.Join(t.TempDir(), "proj")
	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{ID: domain.NewWorkspaceID(), Name: "p", RootPath: root})
	if err != nil {
		t.Fatalf("UpsertWorkspace: %v", err)
	}

	byID, err := store.GetWorkspace(ctx, ws.ID)
	if err != nil || byID.RootPath != root {
		t.Fatalf("GetWorkspace: %+v err=%v", byID, err)
	}
	byRoot, err := store.GetWorkspaceByRoot(ctx, root)
	if err != nil || byRoot.ID != ws.ID {
		t.Fatalf("GetWorkspaceByRoot: %+v err=%v", byRoot, err)
	}
	if _, err := store.GetWorkspace(ctx, domain.NewWorkspaceID()); err == nil {
		t.Fatal("GetWorkspace unknown ID should fail")
	}
}

func TestSessionWorkspaceBindingAndBackfill(t *testing.T) {
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ctx := context.Background()

	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID: domain.NewWorkspaceID(), Name: "default", RootPath: filepath.Join(t.TempDir(), "default"),
	})
	if err != nil {
		t.Fatalf("UpsertWorkspace: %v", err)
	}

	// A session created against the workspace reports it via SessionWorkspace.
	bound := domain.NewSessionID()
	if err := store.CreateSession(ctx, bound, ws.ID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	got, err := store.SessionWorkspace(ctx, bound)
	if err != nil || got != ws.ID {
		t.Fatalf("SessionWorkspace bound: %s err=%v, want %s", got, err, ws.ID)
	}

	// A zero-workspace session simulates the pre-v5 upgrade tail; the backfill
	// reassigns it to the default workspace.
	legacy := domain.NewSessionID()
	if err := store.CreateSession(ctx, legacy, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession legacy: %v", err)
	}
	if got, _ := store.SessionWorkspace(ctx, legacy); !got.IsZero() {
		t.Fatalf("legacy session should start unassigned, got %s", got)
	}
	n, err := store.BackfillSessionWorkspaces(ctx, ws.ID)
	if err != nil {
		t.Fatalf("BackfillSessionWorkspaces: %v", err)
	}
	if n != 1 {
		t.Fatalf("backfill affected %d rows, want 1", n)
	}
	if got, _ := store.SessionWorkspace(ctx, legacy); got != ws.ID {
		t.Fatalf("legacy session after backfill: %s, want %s", got, ws.ID)
	}
	// Idempotent: a second backfill touches nothing.
	if n, _ := store.BackfillSessionWorkspaces(ctx, ws.ID); n != 0 {
		t.Fatalf("second backfill affected %d rows, want 0", n)
	}
}

func TestListSessionsFiltersByWorkspace(t *testing.T) {
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ctx := context.Background()

	wsA, _ := store.UpsertWorkspace(ctx, domain.Workspace{ID: domain.NewWorkspaceID(), Name: "a", RootPath: filepath.Join(t.TempDir(), "a")})
	wsB, _ := store.UpsertWorkspace(ctx, domain.Workspace{ID: domain.NewWorkspaceID(), Name: "b", RootPath: filepath.Join(t.TempDir(), "b")})
	sessA := domain.NewSessionID()
	sessB := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessA, wsA.ID); err != nil {
		t.Fatalf("CreateSession A: %v", err)
	}
	if err := store.CreateSession(ctx, sessB, wsB.ID); err != nil {
		t.Fatalf("CreateSession B: %v", err)
	}

	all, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil || len(all) != 2 {
		t.Fatalf("ListSessions all: n=%d err=%v, want 2", len(all), err)
	}
	onlyA, _, err := store.ListSessions(ctx, "", 10, false, wsA.ID)
	if err != nil || len(onlyA) != 1 || onlyA[0].ID != sessA {
		t.Fatalf("ListSessions A: %+v err=%v", onlyA, err)
	}
	if onlyA[0].WorkspaceID != wsA.ID {
		t.Fatalf("summary workspace = %s, want %s", onlyA[0].WorkspaceID, wsA.ID)
	}

	counts, err := store.CountSessionsPerWorkspace(ctx)
	if err != nil {
		t.Fatalf("CountSessionsPerWorkspace: %v", err)
	}
	if counts[wsA.ID] != 1 || counts[wsB.ID] != 1 {
		t.Fatalf("counts = %+v, want 1 each", counts)
	}
}

// TestMigrateV5FromV4 drives the upgrade path: a hand-built v4 database (no
// workspace_id column) opened by the current store must gain the column and
// the workspaces table, and accept workspace-bound sessions afterwards.
func TestMigrateV5FromV4(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sessions.db")
	ctx := context.Background()

	// Build a minimal v4-shaped database: v4 sessions table (archived_at, no
	// workspace_id) plus a schema_migrations row pinning version 4.
	raw, err := sql.Open("sqlite", "file:"+filepath.ToSlash(path))
	if err != nil {
		t.Fatalf("sql.Open: %v", err)
	}
	_, err = raw.ExecContext(ctx, `
CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);
INSERT INTO schema_migrations(version, applied_at) VALUES (4, '`+time.Now().UTC().Format(time.RFC3339Nano)+`');
CREATE TABLE sessions (
    session_id TEXT PRIMARY KEY,
    version INTEGER NOT NULL CHECK (version >= 0),
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    updated_at_unix_nano INTEGER NOT NULL,
    archived_at_unix_nano INTEGER
);`)
	if err != nil {
		t.Fatalf("build v4 database: %v", err)
	}
	if err := raw.Close(); err != nil {
		t.Fatalf("close v4 database: %v", err)
	}

	store := openTestSQLiteStore(t, path)
	// After migration the store accepts a workspace-bound session.
	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{ID: domain.NewWorkspaceID(), Name: "w", RootPath: filepath.Join(t.TempDir(), "w")})
	if err != nil {
		t.Fatalf("UpsertWorkspace after migration: %v", err)
	}
	sid := domain.NewSessionID()
	if err := store.CreateSession(ctx, sid, ws.ID); err != nil {
		t.Fatalf("CreateSession after migration: %v", err)
	}
	if got, _ := store.SessionWorkspace(ctx, sid); got != ws.ID {
		t.Fatalf("SessionWorkspace after migration: %s, want %s", got, ws.ID)
	}
}
