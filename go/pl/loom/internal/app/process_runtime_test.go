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

package app

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// One sweep pass purges sessions archived beyond gc_archived_after (and
// notifies the purge hook), collects orphaned artifacts past the grace
// period, and leaves active sessions and fresh blobs untouched. A repeat
// pass is a no-op.
func TestRunSessionSweepPurgesAndCollects(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}

	archived, active := domain.NewSessionID(), domain.NewSessionID()
	for _, id := range []domain.SessionID{archived, active} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
	}
	if _, err := store.SetSessionArchived(ctx, archived, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	oldOrphan, err := artStore.PutBytes(ctx, []byte("old orphan"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	digest := strings.TrimPrefix(oldOrphan.ID.String(), "art_sha256_")
	blobPath := filepath.Join(artStore.Root(), "sha256", digest[:2], digest[2:])
	oldTime := time.Now().Add(-artifact.DefaultGCGracePeriod - time.Hour)
	if err := os.Chtimes(blobPath, oldTime, oldTime); err != nil {
		t.Fatalf("Chtimes: %v", err)
	}
	fresh, err := artStore.PutBytes(ctx, []byte("fresh orphan"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	// A 1ns retention purges anything archived before the pass runs — the
	// artifact writes between archiving and sweeping make the gap orders
	// of magnitude wider than the retention.
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{GCArchivedAfter: time.Nanosecond},
	})
	var purged []domain.SessionID
	proc.SetSessionPurgeHook(func(_ context.Context, ids []domain.SessionID) {
		purged = append(purged, ids...)
	})

	proc.runSessionSweepOnce(ctx, store, artStore)

	if len(purged) != 1 || purged[0] != archived {
		t.Fatalf("purged = %v, want [%s]", purged, archived)
	}
	def, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions default: %v", err)
	}
	if len(def) != 1 || def[0].ID != active {
		t.Fatalf("default listing = %+v, want only the active session", def)
	}
	arch, _, err := store.ListSessions(ctx, "", 10, true, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions archived: %v", err)
	}
	if len(arch) != 0 {
		t.Fatalf("archived listing = %+v, want empty", arch)
	}
	if _, err := os.Stat(blobPath); !os.IsNotExist(err) {
		t.Fatalf("old orphan still exists: %v", err)
	}
	if _, ok := artStore.PathForRef(fresh); !ok {
		t.Fatal("fresh blob must survive the grace period")
	}

	// A repeat pass finds nothing: the hook must not fire again.
	proc.runSessionSweepOnce(ctx, store, artStore)
	if len(purged) != 1 {
		t.Fatalf("purged after repeat pass = %v, want unchanged", purged)
	}
}

// A blob shared by two sessions (content addressing makes identical
// outputs the same blob) must survive the purge of one referrer: the
// remaining session's artifact_refs row keeps it referenced.
func TestRunSessionSweepKeepsSharedArtifact(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	shared, err := artStore.PutBytes(ctx, []byte("shared blob"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	expired, active := domain.NewSessionID(), domain.NewSessionID()
	for _, id := range []domain.SessionID{expired, active} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
		appendArtifactRef(t, store, id, shared)
	}
	if _, err := store.SetSessionArchived(ctx, expired, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{GCArchivedAfter: time.Nanosecond},
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	if _, err := store.IsSessionArchived(ctx, expired); err == nil {
		t.Fatal("expired session must be purged")
	}
	if _, ok := artStore.PathForRef(shared); !ok {
		t.Fatal("shared blob must survive: the active session still references it")
	}
}

// A session archived by the sweep itself must never be purged by the same
// pass: archiving stamps archived_at after the pass cutoff.
func TestRunSessionSweepDoesNotPurgeFreshlyArchived(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	stale := domain.NewSessionID()
	if err := store.CreateSession(ctx, stale, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{
			AutoArchiveAfter: time.Nanosecond,
			GCArchivedAfter:  time.Nanosecond,
		},
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	archived, err := store.IsSessionArchived(ctx, stale)
	if err != nil {
		t.Fatalf("IsSessionArchived: %v (the session must survive the pass)", err)
	}
	if !archived {
		t.Fatal("stale session must be archived by the pass")
	}
}

// appendArtifactRef persists one event plus a checkpoint referencing the
// artifact — the durable reference artifact GC consults.
func appendArtifactRef(t *testing.T, store *session.SQLiteStore, id domain.SessionID, ref domain.ArtifactRef) {
	t.Helper()
	now := time.Now().UTC()
	evt := domain.Event{
		ID: domain.NewEventID(), SessionID: id, Sequence: 1,
		Type: domain.EventSessionCreated, Timestamp: now,
	}
	ckpt := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: id, Sequence: 1,
		Messages: []domain.Message{{
			ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: now,
			Parts: []domain.ContentPart{{Kind: domain.PartArtifact, Artifact: &ref}},
		}},
		CreatedAt: now,
	}
	if err := store.AppendEventsAndCheckpoint(context.Background(), id, 0, []domain.Event{evt}, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
}

// With every knob off the sweep still collects orphaned artifacts but
// never purges a session.
func TestRunSessionSweepKeepsArchivedSessionsWhenPurgeDisabled(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	archived := domain.NewSessionID()
	if err := store.CreateSession(ctx, archived, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, err := store.SetSessionArchived(ctx, archived, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{})
	proc.SetSessionPurgeHook(func(_ context.Context, ids []domain.SessionID) {
		t.Fatalf("purge hook fired with purge disabled: %v", ids)
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	arch, _, err := store.ListSessions(ctx, "", 10, true, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions archived: %v", err)
	}
	if len(arch) != 1 || arch[0].ID != archived {
		t.Fatalf("archived listing = %+v, want the archived session kept", arch)
	}
}
