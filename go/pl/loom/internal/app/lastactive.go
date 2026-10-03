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
// Created: 2026/10/03

package app

import (
	"context"
	"os"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// LastActiveWorkspaceRoot derives the default workspace for a GUI-launched
// process from the most recently updated session, so a Finder-launched Loom
// reopens in the project the user last worked in — the same source of truth
// the SPA's sidebar focus and composer default use. Returns "" when there is
// no usable history (first launch, legacy rows, deleted/moved workspace); the
// caller then picks its own fallback. Read-only: safe to run while another
// loom process owns the data dir (WAL allows concurrent readers).
func LastActiveWorkspaceRoot(ctx context.Context, resolved *config.ResolvedConfig) string {
	dbPath := resolved.Storage.SessionDBPath()
	if _, err := os.Stat(dbPath); err != nil {
		return ""
	}
	store, err := session.OpenSQLiteStoreReadOnly(ctx, dbPath)
	if err != nil {
		return ""
	}
	defer store.Close()
	summaries, _, err := store.ListSessions(ctx, "", 1, false, domain.WorkspaceID{})
	if err != nil || len(summaries) == 0 {
		return ""
	}
	wsID, err := store.SessionWorkspace(ctx, summaries[0].ID)
	if err != nil {
		return ""
	}
	ws, err := store.GetWorkspace(ctx, wsID)
	if err != nil || ws.RootPath == "" {
		return ""
	}
	// The directory may have moved since the workspace was registered.
	if info, err := os.Stat(ws.RootPath); err != nil || !info.IsDir() {
		return ""
	}
	return ws.RootPath
}

// IsLauncherRoot reports whether root is one of the cwd values a GUI
// launcher can hand a child process that never name a real project:
// LaunchServices starts apps in "/", and the bundled Swift wrapper picks
// the home directory merely to escape "/" (swift/pl/loom ServerManager).
// Registering either as the persisted default workspace is wrong — home
// additionally shadows the user-scope skill roots, duplicating every user
// skill in the settings overview.
func IsLauncherRoot(root string) bool {
	if root == "/" {
		return true
	}
	if home, err := os.UserHomeDir(); err == nil {
		return root == home
	}
	return false
}
