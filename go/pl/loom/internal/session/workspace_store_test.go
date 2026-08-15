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
	"path/filepath"
	"strings"
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

func TestSessionWorkspaceBinding(t *testing.T) {
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

	// A session created without a workspace reports the zero ID.
	unbound := domain.NewSessionID()
	if err := store.CreateSession(ctx, unbound, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession unbound: %v", err)
	}
	if got, _ := store.SessionWorkspace(ctx, unbound); !got.IsZero() {
		t.Fatalf("unbound session should report the zero workspace, got %s", got)
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

// TestDeleteWorkspace locks the deletion semantics (docs/WORKSPACE_DESIGN.md
// §16.1): deleting a workspace cascades to its sessions in one transaction —
// session rows go away with their events/checkpoints/artifact_refs (FK
// cascade) and file_changes/memory_jobs (explicit deletes) — while sessions
// of other workspaces are untouched.
func TestDeleteWorkspace(t *testing.T) {
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ctx := context.Background()

	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID: domain.NewWorkspaceID(), Name: "doomed", RootPath: filepath.Join(t.TempDir(), "doomed"),
	})
	if err != nil {
		t.Fatalf("UpsertWorkspace: %v", err)
	}
	other, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID: domain.NewWorkspaceID(), Name: "other", RootPath: filepath.Join(t.TempDir(), "other"),
	})
	if err != nil {
		t.Fatalf("UpsertWorkspace other: %v", err)
	}

	// Seed the doomed workspace's session with one row in every per-session
	// table so the cascade is exercised end to end.
	sid := domain.NewSessionID()
	if err := store.CreateSession(ctx, sid, ws.ID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	artifactID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("8", 64))
	ckpt := testCheckpoint(sid, 1, time.Now().UTC())
	ckpt.Messages[0].Parts = []domain.ContentPart{
		{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: artifactID, Size: 7}},
	}
	events := []domain.Event{newEvent(sid, 1, domain.EventSessionCreated, nil)}
	if err := store.AppendEventsAndCheckpoint(ctx, sid, 0, events, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
	if err := store.RecordFileChange(ctx, sid, "a.go", true, "h1", []byte("v1"), "h2"); err != nil {
		t.Fatalf("RecordFileChange: %v", err)
	}
	if err := store.EnqueueMemoryJob(ctx, sid, ws.RootPath); err != nil {
		t.Fatalf("EnqueueMemoryJob: %v", err)
	}

	// A session in another workspace must survive the cascade.
	keep := domain.NewSessionID()
	if err := store.CreateSession(ctx, keep, other.ID); err != nil {
		t.Fatalf("CreateSession other: %v", err)
	}

	// Deleting an unknown workspace is an error.
	if err := store.DeleteWorkspace(ctx, domain.NewWorkspaceID()); err == nil {
		t.Fatal("DeleteWorkspace unknown ID should fail")
	}

	if err := store.DeleteWorkspace(ctx, ws.ID); err != nil {
		t.Fatalf("DeleteWorkspace: %v", err)
	}
	if _, err := store.GetWorkspace(ctx, ws.ID); err == nil {
		t.Fatal("GetWorkspace after delete should fail")
	}
	if _, err := store.GetWorkspaceByRoot(ctx, ws.RootPath); err == nil {
		t.Fatal("GetWorkspaceByRoot after delete should fail")
	}
	// Idempotency is NOT a goal: a second delete reports not-found.
	if err := store.DeleteWorkspace(ctx, ws.ID); err == nil {
		t.Fatal("second DeleteWorkspace should fail")
	}

	// The workspace's session is gone with all of its per-session rows.
	if _, err := store.SessionWorkspace(ctx, sid); err == nil {
		t.Fatal("SessionWorkspace after delete should fail (session cascaded)")
	}
	for _, table := range []string{"sessions", "events", "checkpoints", "artifact_refs", "file_changes", "memory_jobs"} {
		var n int
		if err := store.db.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM "+table+" WHERE session_id = ?", sid.String()).Scan(&n); err != nil {
			t.Fatalf("count %s: %v", table, err)
		}
		if n != 0 {
			t.Fatalf("%s rows after DeleteWorkspace = %d, want 0", table, n)
		}
	}

	// The other workspace and its session are untouched.
	if got, err := store.SessionWorkspace(ctx, keep); err != nil || got != other.ID {
		t.Fatalf("SessionWorkspace surviving session: %s err=%v, want %s", got, err, other.ID)
	}
	summaries, _, err := store.ListSessions(ctx, "", 10, false, other.ID)
	if err != nil || len(summaries) != 1 || summaries[0].ID != keep {
		t.Fatalf("ListSessions other workspace: %+v err=%v, want the surviving session", summaries, err)
	}
}
