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
// Created: 2026/08/07

package server

import (
	"errors"
	"net/http"
	"os"
	"regexp"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// shareTokenPattern constrains share tokens to the 128-bit hex shape the
// store generates — anything else is a client error, not a lookup.
var shareTokenPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// parseShareToken validates the {token} path value.
func parseShareToken(r *http.Request) (string, error) {
	token := r.PathValue("token")
	if !shareTokenPattern.MatchString(token) {
		return "", &statusError{status: http.StatusNotFound, code: "share_not_found", message: "share not found"}
	}
	return token, nil
}

// handleShareSession creates (or returns the existing) public share link
// for the session. Bearer-gated: only the owner can mint a link.
func (s *Server) handleShareSession(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	token, err := s.svc.ShareSession(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("share.create", id)
	resp := map[string]string{
		"token": token,
		"path":  "/share/" + token,
	}
	// When the LAN share listener is up, include the absolute link so
	// clients can hand out a URL reachable beyond this machine — the
	// webview talks to the loopback UI, so the base must come from the
	// live listener, not this server's own address
	// (docs/DESKTOP_DESIGN.md §5.2).
	if s.cfg.Share != nil {
		if base := s.cfg.Share.PublicBase(); base != "" {
			resp["url"] = base + "/share/" + token
		}
	}
	writeJSON(w, http.StatusOK, resp)
}

// handleGetShareEndpoint reports the runtime state of the LAN share
// listener (docs/DESKTOP_DESIGN.md §5). Servers without a ShareManager
// (`loom serve`, where --listen is the whole story) answer 404 so the
// frontend hides the toggle.
func (s *Server) handleGetShareEndpoint(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.cfg.Share.State())
}

// handleSetShareEndpoint persists the share.enabled preference into the
// config file and hot-applies it — the same write-through pattern as
// skills.disabled, so the on/off state never diverges from the file:
// the listener starts/stops via the hot-apply reconcile, and the next
// launch sees the same state. The bind address stays config-driven
// (share.listen is edited in settings, not here).
func (s *Server) handleSetShareEndpoint(w http.ResponseWriter, r *http.Request) {
	if s.cfg.ConfigPath == "" {
		writeError(w, errConfigUnavailable)
		return
	}
	var body struct {
		Enabled bool `json:"enabled"`
	}
	if err := decodeBody(w, r, &body); err != nil {
		writeError(w, err)
		return
	}

	// Read-modify-write against the on-disk file; writeAndApplyConfig
	// re-reads it under the config lock and rejects on revision skew, so
	// this pre-read only seeds the mutation, never the trust decision.
	raw, err := os.ReadFile(s.cfg.ConfigPath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		writeError(w, err)
		return
	}
	incoming := &config.File{}
	if raw != nil {
		incoming, err = config.ParseFile(raw)
		if err != nil {
			writeError(w, &statusError{
				status:  http.StatusInternalServerError,
				code:    "config_invalid",
				message: "config file cannot be parsed (fix it by hand): " + err.Error(),
			})
			return
		}
	}
	enabled := body.Enabled
	incoming.Share.Enabled = &enabled
	revision := ""
	if raw != nil {
		revision = config.RevisionOf(raw)
	}
	merged, report, err := s.writeAndApplyConfig(r, incoming, revision)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("share.endpoint", domain.SessionID{}, "enabled", body.Enabled)
	writeJSON(w, http.StatusOK, map[string]any{
		"revision": config.RevisionOf(merged),
		"applied":  report,
		"endpoint": s.cfg.Share.State(),
	})
}

// handleRevokeShare deletes the session's share link; existing links stop
// resolving immediately.
func (s *Server) handleRevokeShare(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	if err := s.svc.RevokeShare(r.Context(), id); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("share.revoke", id)
	w.WriteHeader(http.StatusNoContent)
}

// handleSharedView serves the public read-only transcript view. No bearer
// token: the share token in the URL is the capability.
func (s *Server) handleSharedView(w http.ResponseWriter, r *http.Request) {
	token, err := parseShareToken(r)
	if err != nil {
		writeError(w, err)
		return
	}
	view, err := s.svc.SharedView(r.Context(), token)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, view)
}

// handleSharedArtifact serves artifact bytes through a share link, limited
// to blobs the shared session actually referenced (images it rendered).
func (s *Server) handleSharedArtifact(w http.ResponseWriter, r *http.Request) {
	token, err := parseShareToken(r)
	if err != nil {
		writeError(w, err)
		return
	}
	ref, err := parseArtifactRefParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	data, err := s.svc.ReadSharedArtifact(r.Context(), token, ref)
	if err != nil {
		writeError(w, err)
		return
	}
	serveArtifactBytes(w, data)
}
