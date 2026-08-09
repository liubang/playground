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
// Created: 2026/08/08

package server

import (
	"errors"
	"net/http"
	"os"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
)

// The skills endpoints back the settings UI's skill management: an
// aggregated read-only catalog (every registered workspace plus the shared
// user scope), a per-name disable toggle persisted into the config file
// (skills.disabled, hot-applied to every assembled loader), and a delete
// that removes the on-disk skill directory after proving it lives under
// one of the discovery roots. Skill content editing stays in the editor;
// the remaining runtime toggles live in the config (skills.enabled /
// extra_roots).

// handleListSkills serves GET /v1/skills.
func (s *Server) handleListSkills(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.svc.SkillsOverview(r.Context()))
}

// skillDisabledUpdate is the PUT /v1/skills/{name}/disabled request model.
type skillDisabledUpdate struct {
	Disabled bool `json:"disabled"`
}

// handleSetSkillDisabled persists one skill name into (or out of) the
// config's skills.disabled list and hot-applies the result. Disable is by
// name — the catalog's identity key — so it spans scopes and workspaces.
func (s *Server) handleSetSkillDisabled(w http.ResponseWriter, r *http.Request) {
	if s.cfg.ConfigPath == "" {
		writeError(w, errConfigUnavailable)
		return
	}
	name := r.PathValue("name")
	if name == "" || len([]rune(name)) > skill.MaxNameLen {
		writeError(w, invalidInput("invalid skill name"))
		return
	}
	var req skillDisabledUpdate
	if err := decodeBody(w, r, &req); err != nil {
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
	incoming.Skills.Disabled = updateDisabledList(incoming.Skills.Disabled, name, req.Disabled)
	revision := ""
	if raw != nil {
		revision = config.RevisionOf(raw)
	}
	merged, report, err := s.writeAndApplyConfig(r, incoming, revision)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("skill.set_disabled", domain.SessionID{}, "name", name, "disabled", req.Disabled)
	disabled := incoming.Skills.Disabled
	if disabled == nil {
		disabled = []string{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"revision": config.RevisionOf(merged),
		"disabled": disabled,
		"applied":  report,
	})
}

// updateDisabledList adds name to (disabled=true) or removes it from
// (disabled=false) the list, keeping the existing order and deduping.
func updateDisabledList(list []string, name string, disabled bool) []string {
	out := make([]string, 0, len(list)+1)
	found := false
	for _, n := range list {
		if n == name {
			found = true
			if !disabled {
				continue
			}
		}
		out = append(out, n)
	}
	if disabled && !found {
		out = append(out, name)
	}
	return out
}

// skillDeleteRequest is the DELETE /v1/skills request model: the SKILL.md
// path exactly as reported by GET /v1/skills.
type skillDeleteRequest struct {
	Path string `json:"path"`
}

// handleDeleteSkill removes one discovered skill from disk. The service
// confines the deletion to the discovery roots (see DeleteSkill): anything
// a scan would not load cannot be deleted through this endpoint.
func (s *Server) handleDeleteSkill(w http.ResponseWriter, r *http.Request) {
	var req skillDeleteRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	if strings.TrimSpace(req.Path) == "" {
		writeError(w, invalidInput("path is required"))
		return
	}
	if err := s.svc.DeleteSkill(r.Context(), req.Path); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("skill.delete", domain.SessionID{}, "path", req.Path)
	w.WriteHeader(http.StatusNoContent)
}
