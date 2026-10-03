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
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// writeSessionStore creates the sessions.db layout LastActiveWorkspaceRoot
// reads (sessions/sessions.db under the loom home) with one workspace and
// one session bound to it.
func writeSessionStore(t *testing.T, home, wsRoot string) {
	t.Helper()
	ctx := context.Background()
	resolved := config.ResolvedStorage{BaseDir: home}
	if err := os.MkdirAll(resolved.SessionsDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	store, err := session.OpenSQLiteStore(ctx, resolved.SessionDBPath())
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()
	ws, err := store.UpsertWorkspace(ctx, domain.Workspace{
		ID: domain.NewWorkspaceID(), Name: "proj", RootPath: wsRoot,
	})
	if err != nil {
		t.Fatalf("UpsertWorkspace: %v", err)
	}
	if err := store.CreateSession(ctx, domain.NewSessionID(), ws.ID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
}

// With no session store there is nothing to derive from — the caller must
// get "" so it can pick its own fallback.
func TestLastActiveWorkspaceRootNoHistory(t *testing.T) {
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: t.TempDir()}}
	if got := LastActiveWorkspaceRoot(context.Background(), resolved); got != "" {
		t.Fatalf("LastActiveWorkspaceRoot without store = %q, want empty", got)
	}
}

func TestLastActiveWorkspaceRootReturnsRecentWorkspaceRoot(t *testing.T) {
	home := t.TempDir()
	wsRoot := t.TempDir()
	writeSessionStore(t, home, wsRoot)
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: home}}
	if got := LastActiveWorkspaceRoot(context.Background(), resolved); got != wsRoot {
		t.Fatalf("LastActiveWorkspaceRoot = %q, want %q", got, wsRoot)
	}
}

// A workspace whose root was deleted after registration is not a usable
// launch target — derive nothing rather than resurrect a stale path.
func TestLastActiveWorkspaceRootSkipsMissingRoot(t *testing.T) {
	home := t.TempDir()
	wsRoot := filepath.Join(t.TempDir(), "gone")
	writeSessionStore(t, home, wsRoot)
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: home}}
	if got := LastActiveWorkspaceRoot(context.Background(), resolved); got != "" {
		t.Fatalf("LastActiveWorkspaceRoot with missing root = %q, want empty", got)
	}
}

func TestIsLauncherRoot(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	if !IsLauncherRoot("/") {
		t.Fatal(`IsLauncherRoot("/") = false, want true`)
	}
	if !IsLauncherRoot(home) {
		t.Fatalf("IsLauncherRoot(%q) = false, want true (home)", home)
	}
	if IsLauncherRoot(filepath.Join(home, "project")) {
		t.Fatal("IsLauncherRoot(project dir) = true, want false")
	}
}
