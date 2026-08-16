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

package server

import (
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// workspaceEntry is the wire shape of one workspace, with its session count
// and a default-workspace marker. The default workspace can be deleted;
// is_default is informational (e.g. for UI badges).
type workspaceEntry struct {
	domain.Workspace
	SessionCount int  `json:"session_count"`
	IsDefault    bool `json:"is_default"`
}

// handleListWorkspaces serves GET /v1/workspaces: every registered workspace
// with its session count (docs/WORKSPACE_DESIGN.md §8.1).
func (s *Server) handleListWorkspaces(w http.ResponseWriter, r *http.Request) {
	workspaces, err := s.svc.ListWorkspaces(r.Context())
	if err != nil {
		writeError(w, err)
		return
	}
	counts, err := s.svc.CountSessionsPerWorkspace(r.Context())
	if err != nil {
		// Counts are best-effort enrichment; a failure must not fail listing.
		counts = map[domain.WorkspaceID]int{}
	}
	defaultID := s.svc.DefaultWorkspaceID()
	out := make([]workspaceEntry, len(workspaces))
	for i, ws := range workspaces {
		out[i] = workspaceEntry{Workspace: ws, SessionCount: counts[ws.ID], IsDefault: ws.ID == defaultID}
	}
	writeJSON(w, http.StatusOK, map[string]any{"workspaces": out})
}

// registerWorkspaceRequest is the body of POST /v1/workspaces.
type registerWorkspaceRequest struct {
	RootPath string `json:"root_path"`
	Name     string `json:"name,omitempty"`
}

// handleRegisterWorkspace serves POST /v1/workspaces: register (or reuse by
// canonical root) a workspace. root_path must exist and be a directory.
func (s *Server) handleRegisterWorkspace(w http.ResponseWriter, r *http.Request) {
	var req registerWorkspaceRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	if strings.TrimSpace(req.RootPath) == "" {
		writeError(w, invalidInput("root_path is required"))
		return
	}
	ws, err := s.svc.RegisterWorkspace(r.Context(), req.RootPath, req.Name)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("workspace.register", domain.SessionID{}, "workspace_id", ws.ID.String(), "root_path", ws.RootPath)
	writeJSON(w, http.StatusOK, map[string]any{"workspace": ws})
}

// handleGetWorkspace serves GET /v1/workspaces/{id}.
func (s *Server) handleGetWorkspace(w http.ResponseWriter, r *http.Request) {
	id, err := parseWorkspaceIDParam(r.PathValue("id"))
	if err != nil {
		writeError(w, err)
		return
	}
	ws, err := s.svc.GetWorkspace(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"workspace": ws})
}

// handleDeleteWorkspace serves DELETE /v1/workspaces/{id}: removes the
// workspace entity and cascades to its sessions — live sessions are shut
// down, persisted session history is deleted with the workspace
// (docs/WORKSPACE_DESIGN.md §16.1). The on-disk root directory is never
// touched. The default workspace cannot be deleted (409 workspace_in_use).
func (s *Server) handleDeleteWorkspace(w http.ResponseWriter, r *http.Request) {
	id, err := parseWorkspaceIDParam(r.PathValue("id"))
	if err != nil {
		writeError(w, err)
		return
	}
	if id.IsZero() {
		writeError(w, invalidInput("invalid workspace id"))
		return
	}
	if err := s.svc.DeleteWorkspace(r.Context(), id); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("workspace.delete", domain.SessionID{}, "workspace_id", id.String())
	w.WriteHeader(http.StatusNoContent)
}

// --- directory browsing (workspace picker, docs/WORKSPACE_DESIGN.md §11.3) ---

// The browser is deliberately confined: it never lists above $HOME and never
// descends into hidden (dot-prefixed) directories, which carry credentials
// and VCS internals (.ssh/.git/...). It lists subdirectories only.

// hiddenDir matches dot-prefixed path components other than "." and "..".
var hiddenDir = regexp.MustCompile(`^\.[^.].*`)

// dirEntry is one browsable directory.
type dirEntry struct {
	Name string `json:"name"`
	Path string `json:"path"` // absolute path
}

// handleBrowseDirectories serves GET /v1/files/browse?path=...
//
// Query params: path (absolute or "~"-leading; empty or "~" = $HOME). The
// response carries the canonical absolute path, its parent (browseable unless
// above $HOME), and its visible subdirectories.
//
// Symlinks-to-directories are listed and traversable: os.ReadDir reports
// lstat semantics (a symlink entry is not IsDir), so link entries are
// followed with Stat to decide visibility. Navigation stays on the
// REQUESTED path form (symlink-bearing): responses echo it and parent
// links climb it, so a user entering ~/work keeps seeing ~/work/... even
// when the link resolves outside $HOME — the link itself is a door the
// user built under their home; workspace registration canonicalizes the
// root with EvalSymlinks anyway.
func (s *Server) handleBrowseDirectories(w http.ResponseWriter, r *http.Request) {
	home, err := os.UserHomeDir()
	if err != nil {
		writeError(w, invalidInput("user home unavailable"))
		return
	}
	raw := strings.TrimSpace(r.URL.Query().Get("path"))
	if raw == "" || raw == "~" {
		raw = home
	} else if strings.HasPrefix(raw, "~/") {
		raw = filepath.Join(home, raw[2:])
	}
	abs, err := filepath.Abs(raw)
	if err != nil {
		writeError(w, invalidInput("invalid path"))
		return
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		writeError(w, invalidInput("path does not exist"))
		return
	}
	info, err := os.Stat(resolved)
	if err != nil || !info.IsDir() {
		writeError(w, invalidInput("path is not a directory"))
		return
	}

	// The browser is anchored at $HOME (see the comment above): the
	// requested path — in its user-facing, symlink-bearing form — must
	// start under $HOME. Its resolved target may sit elsewhere (the user
	// followed their own symlink), but browsing can never be ENTERED
	// from outside $HOME.
	homeResolved := home
	if h, herr := filepath.EvalSymlinks(home); herr == nil {
		homeResolved = h
	}
	withinHome := func(p string) bool {
		return p == homeResolved || strings.HasPrefix(p, homeResolved+string(filepath.Separator))
	}
	if !withinHome(abs) && !withinHome(resolved) {
		writeError(w, invalidInput("path is outside the home directory"))
		return
	}

	// Entries are addressed on the requested path form so subsequent
	// clicks keep navigating through the symlink view.
	displayPath := abs
	entries, err := os.ReadDir(resolved)
	if err != nil {
		writeError(w, invalidInput("cannot read directory"))
		return
	}
	dirs := make([]dirEntry, 0)
	for _, e := range entries {
		if hiddenDir.MatchString(e.Name()) {
			continue
		}
		if !e.IsDir() {
			// ReadDir carries lstat semantics: a symlink to a
			// directory is not IsDir. Follow it before hiding.
			if e.Type()&fs.ModeSymlink == 0 {
				continue
			}
			target, err := os.Stat(filepath.Join(resolved, e.Name()))
			if err != nil || !target.IsDir() {
				continue
			}
		}
		dirs = append(dirs, dirEntry{Name: e.Name(), Path: filepath.Join(displayPath, e.Name())})
	}
	// Stable, human-friendly ordering by name.
	sort.Slice(dirs, func(i, j int) bool { return dirs[i].Name < dirs[j].Name })

	// Parent climbs the requested path form, browseable unless it would
	// climb above $HOME.
	parent := ""
	if p := filepath.Dir(displayPath); p != displayPath && withinHome(p) {
		parent = p
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"path":    displayPath,
		"parent":  parent,
		"home":    home,
		"entries": dirs,
	})
}
