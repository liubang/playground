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
// Created: 2026/08/11

// Rule pack endpoints: GET lists the embedded packs with installation
// state; PUT installs one (writes the standard rule file to the user
// rules dir and hot-reloads policy); DELETE uninstalls it. These are the
// transport for the settings panel's "rule packs" cards.
package server

import (
	"net/http"

	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// handleListRulePacks serves GET /v1/rules/packs.
func (s *Server) handleListRulePacks(w http.ResponseWriter, r *http.Request) {
	packs, err := s.svc.ListRulePacks(r.Context())
	if err != nil {
		writeError(w, &statusError{
			status:  http.StatusInternalServerError,
			code:    "packs_unavailable",
			message: "cannot list rule packs: " + err.Error(),
		})
		return
	}
	if packs == nil {
		packs = []permission.PackInfo{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"packs": packs})
}

// handleInstallRulePack serves PUT /v1/rules/packs/{id}/install.
func (s *Server) handleInstallRulePack(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	info, err := s.svc.InstallRulePack(r.Context(), id)
	if err != nil {
		writeError(w, &statusError{
			status:  http.StatusBadRequest,
			code:    "pack_not_found",
			message: err.Error(),
		})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"pack": info})
}

// handleUninstallRulePack serves DELETE /v1/rules/packs/{id}.
func (s *Server) handleUninstallRulePack(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := s.svc.UninstallRulePack(r.Context(), id); err != nil {
		writeError(w, &statusError{
			status:  http.StatusBadRequest,
			code:    "pack_not_found",
			message: err.Error(),
		})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"uninstalled": id})
}
