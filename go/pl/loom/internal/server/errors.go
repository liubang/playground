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
	"encoding/json"
	"errors"
	"net/http"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// apiError is the unified wire error model (docs/SERVE_DESIGN.md §5.3).
type apiError struct {
	Code    string `json:"code"`
	Message string `json:"message,omitempty"`
	State   string `json:"state,omitempty"`
}

type apiErrorBody struct {
	Error apiError `json:"error"`
}

var errUnauthorized = &statusError{status: http.StatusUnauthorized, code: "unauthenticated", message: "bearer token required"}

// statusError couples an HTTP status with a wire error code.
type statusError struct {
	status  int
	code    string
	message string
}

func (e *statusError) Error() string { return e.message }

func invalidInput(msg string) *statusError {
	return &statusError{status: http.StatusBadRequest, code: "invalid_input", message: msg}
}

// writeJSON serializes v as the response body.
func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(v)
}

// writeError renders an error in the unified model. It accepts *statusError
// directly or maps application-layer errors via mapError.
func writeError(w http.ResponseWriter, err error) {
	var se *statusError
	if !errors.As(err, &se) {
		se = mapError(err)
	}
	writeJSON(w, se.status, apiErrorBody{Error: apiError{Code: se.code, Message: se.message}})
}

// mapError converts application-layer errors into the wire error model
// (docs/SERVE_DESIGN.md §5.3 状态码映射). Typed sentinel errors map
// structurally; controller errors (plain strings today) map by stable
// message prefixes until the application layer exports typed errors for
// them (F10 in the design review log).
func mapError(err error) *statusError {
	switch {
	case errors.Is(err, app.ErrSessionNotFound):
		return &statusError{status: http.StatusNotFound, code: "not_found", message: err.Error()}
	case errors.Is(err, app.ErrDraining):
		return &statusError{status: http.StatusServiceUnavailable, code: "draining", message: err.Error()}
	case errors.Is(err, app.ErrCursorInvalid):
		return &statusError{status: http.StatusConflict, code: "cursor_invalid", message: err.Error()}
	case errors.Is(err, app.ErrTooManySessions), errors.Is(err, app.ErrTooManyTurns):
		return &statusError{status: http.StatusTooManyRequests, code: "rate_limited", message: err.Error()}
	}
	msg := err.Error()
	switch {
	case strings.Contains(msg, "cannot submit prompt in state"),
		strings.Contains(msg, "cannot cancel in state"),
		strings.Contains(msg, "cannot create new session in state"),
		strings.Contains(msg, "not awaiting approval in state"):
		return &statusError{status: http.StatusConflict, code: "not_idle", message: msg}
	case strings.Contains(msg, "approval binding does not match"),
		strings.Contains(msg, "unknown or already resolved question"):
		return &statusError{status: http.StatusConflict, code: "binding_mismatch", message: msg}
	case strings.Contains(msg, "session not found"),
		strings.Contains(msg, "no checkpoint"),
		strings.Contains(msg, "no active session"):
		return &statusError{status: http.StatusNotFound, code: "not_found", message: msg}
	case strings.Contains(msg, "parse session ID"),
		strings.Contains(msg, "parse event ID"),
		strings.Contains(msg, "reasoning must be"),
		strings.Contains(msg, "unknown model"),
		strings.Contains(msg, "unknown provider"),
		strings.Contains(msg, "invalid"):
		return &statusError{status: http.StatusBadRequest, code: "invalid_input", message: msg}
	case strings.Contains(msg, "is closed"):
		return &statusError{status: http.StatusGone, code: "session_closed", message: msg}
	}
	return &statusError{status: http.StatusInternalServerError, code: "internal", message: msg}
}

// parseSessionParam validates a {id} path value as a session ID.
func parseSessionParam(r *http.Request) (domain.SessionID, error) {
	raw := r.PathValue("id")
	id, err := domain.ParseSessionID(raw)
	if err != nil || !domain.HasPrefix(id, "sess_") {
		return domain.SessionID{}, invalidInput("invalid session id")
	}
	return id, nil
}

// parseEventParam validates an {approvalID}/{questionID} path value.
func parseEventParam(r *http.Request, name string) (domain.EventID, error) {
	raw := r.PathValue(name)
	id, err := domain.ParseEventID(raw)
	if err != nil {
		return domain.EventID{}, invalidInput("invalid " + name)
	}
	return id, nil
}
