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

import "net/http"

// The skills endpoint serves the aggregated skill catalog (every
// registered workspace plus the shared user scope) for the settings UI.
// It is read-only: skill content editing stays in the editor, and the
// runtime toggles live in the config (skills.enabled / extra_roots).

// handleListSkills serves GET /v1/skills.
func (s *Server) handleListSkills(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.svc.SkillsOverview(r.Context()))
}
