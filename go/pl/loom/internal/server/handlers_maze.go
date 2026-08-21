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

// handlers_maze.go — trace-maze endpoints: GET /v1/sessions/{id}/maze
// (authenticated) and GET /v1/shared/{token}/maze (public; the token is the
// capability). Both return the same MazeData (internal/app/maze.go),
// feeding the webui's trace tab / compare view and the share page's trace
// view respectively.

package server

import (
	"net/http"
)

// handleSessionMaze serves GET /v1/sessions/{id}/maze — the session's
// trace-maze payload built from its persisted event log.
func (s *Server) handleSessionMaze(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	data, err := s.svc.Maze(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, data)
}

// handleSharedMaze serves GET /v1/shared/{token}/maze — the public
// read-only maze for a shared session. No bearer token: the unguessable
// share token in the URL is the capability (same trust level as
// handleSharedView — the maze is a timing/verdict projection of the
// transcript that view already exposes).
func (s *Server) handleSharedMaze(w http.ResponseWriter, r *http.Request) {
	token, err := parseShareToken(r)
	if err != nil {
		writeError(w, err)
		return
	}
	data, err := s.svc.SharedMaze(r.Context(), token)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, data)
}
