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
	"net/http"
	"regexp"
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
	writeJSON(w, http.StatusOK, map[string]string{
		"token": token,
		"path":  "/share/" + token,
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
