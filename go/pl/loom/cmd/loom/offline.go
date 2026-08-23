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
// Created: 2026/08/16

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/server"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// gcReport is the loom gc result: the artifact sweep plus the number of
// archived sessions purged beforehand (0 when sessions.gc_archived_after
// is unset).
type gcReport struct {
	artifact.GCReport
	PurgedSessions int `json:"purged_sessions"`
}

// collectGarbage implements loom gc: it purges sessions archived longer
// than sessions.gc_archived_after, then removes artifact blobs no
// remaining session references (past the grace period) — mirroring the
// online maintenance sweep, so the offline path behaves identically.
// The purge hook is process-local: a running serve's live handles for
// purged sessions are not evicted here — they fail their next write and
// are reaped by the service's idle sweeper.
func collectGarbage(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	var report gcReport
	if resolved.Sessions.GCArchivedAfter > 0 {
		cutoff := time.Now().UTC().Add(-resolved.Sessions.GCArchivedAfter)
		ids, err := store.DeleteExpiredArchivedSessions(ctx, cutoff)
		if err != nil {
			return fmt.Errorf("purge archived sessions: %w", err)
		}
		report.PurgedSessions = len(ids)
	}
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		return fmt.Errorf("list artifact references: %w", err)
	}
	artifactStore, err := artifact.Open(
		filepath.Join(resolved.Storage.SessionsDir(), artifactDirectoryName),
		resolved.Limits.MaxArtifactBytes,
	)
	if err != nil {
		return fmt.Errorf("open artifact store: %w", err)
	}
	artifactReport, err := artifactStore.CollectGarbage(ctx, refs, artifact.DefaultGCGracePeriod, time.Now())
	if err != nil {
		return fmt.Errorf("collect artifact garbage: %w", err)
	}
	report.GCReport = artifactReport
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetEscapeHTML(false)
	return encoder.Encode(report)
}

// listSessions implements loom sessions: it prints every persisted session
// (id, version, last update) from the read-only store view.
func listSessions(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	summaries, _, err := store.ListSessions(ctx, "", 100, false, domain.WorkspaceID{})
	if err != nil {
		return err
	}
	for _, summary := range summaries {
		fmt.Printf("%s\t%d\t%s\n", summary.ID, summary.Version, summary.UpdatedAt.UTC().Format(time.RFC3339Nano))
	}
	return nil
}

// listWorkspaces prints every registered workspace (loom workspace list).
// Read-only path: works without a running serve process.
func listWorkspaces(ctx context.Context) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	workspaces, err := store.ListWorkspaces(ctx)
	if err != nil {
		return err
	}
	for _, ws := range workspaces {
		fmt.Printf("%s\t%s\t%s\n", ws.ID, ws.Name, ws.RootPath)
	}
	return nil
}

// addWorkspace registers a workspace entity (loom workspace add <path>).
// It writes through the local store under the data-dir flock — mutually
// exclusive with a running serve, so a live serve means "use the Web UI or
// POST /v1/workspaces instead". The workspace's runtime is assembled lazily
// on the next Resolve (chat/serve startup), not in this short-lived process.
func addWorkspace(ctx context.Context, root, name string) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	// Canonical validation mirrors the registry's (docs/WORKSPACE_DESIGN.md W2).
	abs, err := filepath.Abs(strings.TrimSpace(root))
	if err != nil {
		return fmt.Errorf("workspace root: %w", err)
	}
	canonical, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return fmt.Errorf("workspace root: %w", err)
	}
	if info, err := os.Stat(canonical); err != nil || !info.IsDir() {
		return fmt.Errorf("workspace root %q is not an existing directory", canonical)
	}
	lock, err := server.AcquireDataDirLock(resolved.Storage.SessionsDir())
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			return errors.New("a loom serve process owns the data directory; add the workspace via the Web UI or POST /v1/workspaces instead")
		}
		return err
	}
	defer lock.Release()
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	if name == "" {
		name = filepath.Base(canonical)
	}
	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID:       domain.NewWorkspaceID(),
		Name:     name,
		RootPath: canonical,
	})
	if err != nil {
		return fmt.Errorf("register workspace: %w", err)
	}
	fmt.Printf("%s\t%s\t%s\n", ws.ID, ws.Name, ws.RootPath)
	return nil
}

// removeWorkspace deletes a workspace entity (loom workspace rm <id>) and
// cascades to its sessions: every session the workspace owns is deleted
// with its persisted history in the same transaction
// (docs/WORKSPACE_DESIGN.md §16.1). The on-disk root directory is never
// touched. Like `add`, it writes through the local store under the
// data-dir flock: a running serve owns the directory, so deletion then
// goes through the Web UI or DELETE /v1/workspaces/{id} (which
// additionally guards the default workspace).
func removeWorkspace(ctx context.Context, rawID string) error {
	id, err := domain.ParseWorkspaceID(rawID)
	if err != nil || !domain.HasPrefix(id, "ws_") {
		return fmt.Errorf("invalid workspace id %q", rawID)
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	lock, err := server.AcquireDataDirLock(resolved.Storage.SessionsDir())
	if err != nil {
		if errors.Is(err, server.ErrDataDirLocked) {
			return errors.New("a loom serve process owns the data directory; delete the workspace via the Web UI or DELETE /v1/workspaces/{id} instead")
		}
		return err
	}
	defer lock.Release()
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	ws, err := store.GetWorkspace(ctx, id)
	if err != nil {
		return fmt.Errorf("workspace not found: %s", id)
	}
	counts, err := store.CountSessionsPerWorkspace(ctx)
	if err != nil {
		return fmt.Errorf("count workspace sessions: %w", err)
	}
	if err := store.DeleteWorkspace(ctx, id); err != nil {
		return fmt.Errorf("delete workspace: %w", err)
	}
	fmt.Printf("deleted\t%s\t%s\t%s\t(%d session(s) cascaded)\n", ws.ID, ws.Name, ws.RootPath, counts[id])
	return nil
}

// inspectSession dumps one session's durable state (loom inspect
// <session-id>) as JSON: header, latest checkpoint, transcript and event
// counts, from the read-only store view.
func inspectSession(ctx context.Context, rawSessionID string) error {
	sessionID, err := parseSessionID(rawSessionID)
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, false); err != nil {
		return err
	}
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return errors.New("session store does not exist")
		}
		return fmt.Errorf("inspect session store: %w", err)
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return fmt.Errorf("open session store: %w", err)
	}
	defer store.Close()
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		return fmt.Errorf("inspect session: %w", err)
	}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(inspection); err != nil {
		return fmt.Errorf("encode session inspection: %w", err)
	}
	return nil
}
