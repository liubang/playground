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

package domain

import (
	"context"
	"time"
)

// Workspace is a first-class logical project unit: an independent root path
// with its own rules/prompt/skill assembly, owning zero or more sessions
// (docs/WORKSPACE_DESIGN.md §3). It is not a git-repo synonym (the root may
// be a repo subdirectory) and not a process sandbox. The RootPath is the
// canonical (absolute + symlink-resolved) directory, unique across the store.
type Workspace struct {
	ID        WorkspaceID `json:"id"`
	Name      string      `json:"name"`
	RootPath  string      `json:"root_path"`
	CreatedAt time.Time   `json:"created_at"`
	UpdatedAt time.Time   `json:"updated_at"`
}

// IsZero reports whether the workspace is unset.
func (w Workspace) IsZero() bool { return w.ID.IsZero() && w.RootPath == "" }

// WorkspaceStore persists workspace entities and the session→workspace
// ownership mapping. It is implemented by session.SQLiteStore on the same
// connection as the event store (docs/WORKSPACE_DESIGN.md §7.3).
type WorkspaceStore interface {
	// UpsertWorkspace inserts or reuses a workspace keyed on RootPath
	// (idempotent by canonical root). On root conflict the stored row is
	// returned unchanged except for a refreshed name when one is supplied.
	UpsertWorkspace(ctx context.Context, ws Workspace) (Workspace, error)
	// GetWorkspace returns the workspace with the given ID.
	GetWorkspace(ctx context.Context, id WorkspaceID) (Workspace, error)
	// GetWorkspaceByRoot returns the workspace with the given canonical root.
	GetWorkspaceByRoot(ctx context.Context, canonicalRoot string) (Workspace, error)
	// ListWorkspaces returns every registered workspace, newest first.
	ListWorkspaces(ctx context.Context) ([]Workspace, error)
	// DeleteWorkspace removes the workspace entity with the given ID and
	// cascades to its sessions (docs/WORKSPACE_DESIGN.md §16.1): every
	// session owned by the workspace is deleted with all of its persisted
	// data in the same transaction. The on-disk root directory is never
	// touched.
	DeleteWorkspace(ctx context.Context, id WorkspaceID) error
	// SessionWorkspace is ResumeSession's lightweight ownership lookup: the
	// workspace a session belongs to, without loading its events.
	SessionWorkspace(ctx context.Context, sessionID SessionID) (WorkspaceID, error)
	// CountSessionsPerWorkspace returns live session counts keyed by
	// workspace ID, for the list-workspaces endpoint.
	CountSessionsPerWorkspace(ctx context.Context) (map[WorkspaceID]int, error)
}
