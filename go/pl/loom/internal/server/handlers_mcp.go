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
	"net/http"

	"github.com/liubang/playground/go/pl/loom/internal/mcp"
)

// The MCP endpoints expose the process-level MCP manager's live state for
// the settings UI: server connection status and manual reconnect.

// handleListMCPServers serves the status of every configured MCP server.
func (s *Server) handleListMCPServers(w http.ResponseWriter, _ *http.Request) {
	servers := s.svc.MCPServers()
	if servers == nil {
		servers = []mcp.ServerStatus{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"servers": servers})
}

// handleReconnectMCPServer drops and re-establishes one server's
// connection from the live config; the response is the fresh status
// (Connected=false with Error when the reconnect failed).
func (s *Server) handleReconnectMCPServer(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if name == "" {
		writeError(w, invalidInput("server name is required"))
		return
	}
	status, err := s.svc.ReconnectMCPServer(r.Context(), name)
	if err != nil {
		writeError(w, invalidInput(err.Error()))
		return
	}
	writeJSON(w, http.StatusOK, status)
}
