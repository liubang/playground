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
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// workspaceEntry is the wire shape of one workspace, with its session count
// and a default-workspace marker (the default cannot be deleted; frontends
// use it to hide the delete affordance).
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

	// The browser is confined to $HOME (see the comment above): the request
	// path itself must resolve inside it, not just the returned parent link.
	homeResolved := home
	if h, herr := filepath.EvalSymlinks(home); herr == nil {
		homeResolved = h
	}
	if resolved != homeResolved && !strings.HasPrefix(resolved, homeResolved+string(filepath.Separator)) {
		writeError(w, invalidInput("path is outside the home directory"))
		return
	}

	entries, err := os.ReadDir(resolved)
	if err != nil {
		writeError(w, invalidInput("cannot read directory"))
		return
	}
	dirs := make([]dirEntry, 0)
	for _, e := range entries {
		if !e.IsDir() || hiddenDir.MatchString(e.Name()) {
			continue
		}
		dirs = append(dirs, dirEntry{Name: e.Name(), Path: filepath.Join(resolved, e.Name())})
	}
	// Stable, human-friendly ordering by name.
	sort.Slice(dirs, func(i, j int) bool { return dirs[i].Name < dirs[j].Name })

	// Parent is browseable unless it would climb above $HOME.
	parent := ""
	if resolved != homeResolved {
		p := filepath.Dir(resolved)
		if p != resolved && (p == homeResolved || strings.HasPrefix(p, homeResolved+string(filepath.Separator))) {
			parent = p
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"path":    resolved,
		"parent":  parent,
		"home":    home,
		"entries": dirs,
	})
}
