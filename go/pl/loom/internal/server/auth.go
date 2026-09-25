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
// Created: 2026/08/04

package server

import (
	"crypto/subtle"
	"net/http"
	"strings"
)

// authorized reports whether the request carries the configured bearer
// token (constant-time comparison). The token travels in the Authorization
// header — never in cookies, so browsers cannot attach it implicitly and
// CSRF is a non-issue (docs/SERVE_DESIGN.md §5.2/§6). The single exception
// is the event stream's WebSocket upgrade: the browser WebSocket API
// cannot set headers, so that handshake — and ONLY it, scoped to the
// events route — may carry the token as a query parameter. A loopback WS
// URL is no more exposed than the desktop bootstrap fragment that already
// hands the token to the webview, and it never lands in cookies or history.
func (s *Server) authorized(r *http.Request) bool {
	const prefix = "Bearer "
	if header := r.Header.Get("Authorization"); strings.HasPrefix(header, prefix) {
		return subtle.ConstantTimeCompare([]byte(strings.TrimPrefix(header, prefix)), []byte(s.cfg.Token)) == 1
	}
	if isWebSocketUpgrade(r) && isEventsRoute(r.URL.Path) {
		return subtle.ConstantTimeCompare([]byte(r.URL.Query().Get("token")), []byte(s.cfg.Token)) == 1
	}
	return false
}

// isEventsRoute reports whether the path is the per-session event stream
// endpoint (GET /v1/sessions/{id}/events) — the only route that upgrades
// to WebSocket, and therefore the only one allowed query-token auth.
func isEventsRoute(path string) bool {
	return strings.HasPrefix(path, "/v1/sessions/") && strings.HasSuffix(path, "/events")
}
