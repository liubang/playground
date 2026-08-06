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
	"errors"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// UpsertWorkspace inserts a workspace or returns the existing row for the
// same canonical root (docs/WORKSPACE_DESIGN.md W2). On reuse, a non-empty
// incoming name refreshes the stored name; the stored ID and timestamps are
// otherwise preserved.
func (s *SQLiteStore) UpsertWorkspace(ctx context.Context, ws domain.Workspace) (domain.Workspace, error) {
	if ws.ID.IsZero() {
		return domain.Workspace{}, domain.NewError(domain.ErrInvalidInput, "workspace ID is required")
	}
	if ws.RootPath == "" {
		return domain.Workspace{}, domain.NewError(domain.ErrInvalidInput, "workspace root path is required")
	}
	now := time.Now().UTC()
	created := ws.CreatedAt
	if created.IsZero() {
		created = now
	}
	_, err := s.db.ExecContext(ctx, `
INSERT INTO workspaces(workspace_id, name, root_path, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?, ?)`,
		ws.ID.String(), ws.Name, ws.RootPath, formatTime(created), created.UnixNano(), formatTime(now), now.UnixNano())
	switch {
	case err == nil:
		stored := ws
		stored.CreatedAt = created
		stored.UpdatedAt = now
		return stored, nil
	case isUniqueConstraint(err):
		// Root already registered: reuse it (and refresh the name when the
		// caller supplied a new one).
		if ws.Name != "" {
			if _, err := s.db.ExecContext(ctx,
				"UPDATE workspaces SET name = ?, updated_at = ?, updated_at_unix_nano = ? WHERE root_path = ?",
				ws.Name, formatTime(now), now.UnixNano(), ws.RootPath); err != nil {
				return domain.Workspace{}, storeError("refresh workspace name", err)
			}
		}
		return s.GetWorkspaceByRoot(ctx, ws.RootPath)
	default:
		return domain.Workspace{}, storeError("upsert workspace", err)
	}
}

// GetWorkspace returns the workspace with the given ID.
func (s *SQLiteStore) GetWorkspace(ctx context.Context, id domain.WorkspaceID) (domain.Workspace, error) {
	if id.IsZero() {
		return domain.Workspace{}, domain.NewError(domain.ErrInvalidInput, "workspace ID is required")
	}
	row := s.db.QueryRowContext(ctx, `
SELECT workspace_id, name, root_path, created_at, updated_at FROM workspaces WHERE workspace_id = ?`, id.String())
	return scanWorkspace(row)
}

// GetWorkspaceByRoot returns the workspace with the given canonical root.
func (s *SQLiteStore) GetWorkspaceByRoot(ctx context.Context, canonicalRoot string) (domain.Workspace, error) {
	if canonicalRoot == "" {
		return domain.Workspace{}, domain.NewError(domain.ErrInvalidInput, "workspace root path is required")
	}
	row := s.db.QueryRowContext(ctx, `
SELECT workspace_id, name, root_path, created_at, updated_at FROM workspaces WHERE root_path = ?`, canonicalRoot)
	return scanWorkspace(row)
}

// ListWorkspaces returns every registered workspace, newest first.
func (s *SQLiteStore) ListWorkspaces(ctx context.Context) ([]domain.Workspace, error) {
	rows, err := s.db.QueryContext(ctx, `
SELECT workspace_id, name, root_path, created_at, updated_at FROM workspaces
ORDER BY updated_at_unix_nano DESC, workspace_id DESC`)
	if err != nil {
		return nil, storeError("list workspaces", err)
	}
	defer rows.Close()
	result := make([]domain.Workspace, 0)
	for rows.Next() {
		ws, err := scanWorkspace(rows)
		if err != nil {
			return nil, err
		}
		result = append(result, ws)
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate workspaces", err)
	}
	return result, nil
}

type workspaceScanner interface {
	Scan(dest ...any) error
}

func scanWorkspace(row workspaceScanner) (domain.Workspace, error) {
	var id, name, root, createdAt, updatedAt string
	if err := row.Scan(&id, &name, &root, &createdAt, &updatedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.Workspace{}, domain.NewError(domain.ErrUnavailable, "workspace not found")
		}
		return domain.Workspace{}, storeError("scan workspace", err)
	}
	wsID, err := domain.ParseWorkspaceID(id)
	if err != nil {
		return domain.Workspace{}, storeError("decode workspace ID", err)
	}
	created, err := time.Parse(time.RFC3339Nano, createdAt)
	if err != nil {
		return domain.Workspace{}, storeError("decode workspace creation time", err)
	}
	updated, err := time.Parse(time.RFC3339Nano, updatedAt)
	if err != nil {
		return domain.Workspace{}, storeError("decode workspace update time", err)
	}
	return domain.Workspace{ID: wsID, Name: name, RootPath: root, CreatedAt: created, UpdatedAt: updated}, nil
}

// SessionWorkspace returns the owning workspace of a session without loading
// its events (docs/WORKSPACE_DESIGN.md §7.3).
func (s *SQLiteStore) SessionWorkspace(ctx context.Context, sessionID domain.SessionID) (domain.WorkspaceID, error) {
	if sessionID.IsZero() {
		return domain.WorkspaceID{}, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	var raw string
	err := s.db.QueryRowContext(ctx,
		"SELECT workspace_id FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&raw)
	if errors.Is(err, sql.ErrNoRows) {
		return domain.WorkspaceID{}, domain.NewError(domain.ErrUnavailable, "session not found")
	}
	if err != nil {
		return domain.WorkspaceID{}, storeError("load session workspace", err)
	}
	return domain.ParseWorkspaceID(raw)
}

// BackfillSessionWorkspaces reassigns the upgrade tail (sessions whose
// workspace_id is the v5-migration default ”) to the given workspace
// (docs/WORKSPACE_DESIGN.md §7.2). Idempotent; safe to run at every startup.
func (s *SQLiteStore) BackfillSessionWorkspaces(ctx context.Context, id domain.WorkspaceID) (int64, error) {
	if id.IsZero() {
		return 0, domain.NewError(domain.ErrInvalidInput, "workspace ID is required")
	}
	res, err := s.db.ExecContext(ctx,
		"UPDATE sessions SET workspace_id = ? WHERE workspace_id = ''", id.String())
	if err != nil {
		return 0, storeError("backfill session workspaces", err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return 0, storeError("backfill session workspaces result", err)
	}
	return affected, nil
}

// CountSessionsPerWorkspace returns per-workspace session counts (all
// sessions, archived included) for the list-workspaces endpoint.
func (s *SQLiteStore) CountSessionsPerWorkspace(ctx context.Context) (map[domain.WorkspaceID]int, error) {
	rows, err := s.db.QueryContext(ctx, `
SELECT workspace_id, COUNT(*) FROM sessions WHERE workspace_id != '' GROUP BY workspace_id`)
	if err != nil {
		return nil, storeError("count sessions per workspace", err)
	}
	defer rows.Close()
	result := make(map[domain.WorkspaceID]int)
	for rows.Next() {
		var raw string
		var n int
		if err := rows.Scan(&raw, &n); err != nil {
			return nil, storeError("scan workspace session count", err)
		}
		id, err := domain.ParseWorkspaceID(raw)
		if err != nil {
			return nil, storeError("decode workspace ID in session count", err)
		}
		result[id] = n
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate workspace session counts", err)
	}
	return result, nil
}
