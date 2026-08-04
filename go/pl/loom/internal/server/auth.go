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
// header only — never in cookies, so browsers cannot attach it implicitly
// and CSRF is a non-issue (docs/SERVE_DESIGN.md §5.2/§6).
func (s *Server) authorized(r *http.Request) bool {
	const prefix = "Bearer "
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	token := strings.TrimPrefix(header, prefix)
	if len(token) != len(s.cfg.Token) {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(token), []byte(s.cfg.Token)) == 1
}
