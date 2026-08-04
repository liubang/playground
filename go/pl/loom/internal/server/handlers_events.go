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
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// sseHeartbeatInterval is the keepalive comment cadence (docs/SERVE_DESIGN.md
// §5.4): proxies routinely kill silent long connections at 30-60s.
const sseHeartbeatInterval = 15 * time.Second

// handleSessionEvents serves GET /v1/sessions/{id}/events — the SSE event
// channel. It is a pure formatting layer: catch-up + live stitching,
// cursor validation, and slow-consumer policy all live in SessionService.
//
// Frames: `id:` = global sequence, `event:` = RuntimeEvent.Kind,
// `data:` = full RuntimeEvent JSON. The first frame is a comment carrying
// the server instance ID (clients resync when it changes between
// connections). Special server events (server.resync / server.draining)
// are not runtime events and never enter the replay log.
func (s *Server) handleSessionEvents(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, &statusError{status: http.StatusInternalServerError, code: "internal", message: "streaming unsupported"})
		return
	}

	after, err := parseCursor(r)
	if err != nil {
		writeError(w, err)
		return
	}
	if !s.acquireSSE(id.String()) {
		writeError(w, &statusError{status: http.StatusTooManyRequests, code: "rate_limited", message: "too many event streams for this session"})
		return
	}
	defer s.releaseSSE(id.String())

	events, err := s.svc.SubscribeEvents(r.Context(), id, after)
	if errors.Is(err, app.ErrCursorInvalid) {
		// The cursor can no longer be honored: instruct the client to
		// rebuild from a snapshot and close (docs/SERVE_DESIGN.md §5.4).
		s.writeSSEHeaders(w)
		fmt.Fprintf(w, ": connected, instance=%s\n\n", s.instance)
		writeSSEEvent(w, "server.resync", map[string]string{"reason": "cursor_invalid"})
		flusher.Flush()
		return
	}
	if err != nil {
		writeError(w, err)
		return
	}

	s.writeSSEHeaders(w)
	fmt.Fprintf(w, ": connected, instance=%s\n\n", s.instance)
	flusher.Flush()

	heartbeat := time.NewTicker(sseHeartbeatInterval)
	defer heartbeat.Stop()
	for {
		select {
		case evt, ok := <-events:
			if !ok {
				// The stream ended: slow-consumer drop, idle reclaim, pump
				// resync, or service shutdown. Draining gets its named
				// signal so clients stop reconnecting; everything else is
				// an implicit "resync and come back".
				if s.draining() {
					writeSSEEvent(w, "server.draining", map[string]string{"reason": "shutdown"})
					flusher.Flush()
				}
				return
			}
			writeSSEFrame(w, evt)
			flusher.Flush()
		case <-heartbeat.C:
			fmt.Fprintf(w, ": hb %d\n\n", time.Now().Unix())
			flusher.Flush()
		case <-s.shuttingDown():
			// Server-level drain: http.Server.Shutdown does not cancel
			// in-flight request contexts, so without this branch a live
			// SSE client would stall the graceful stop until its
			// deadline. Emit the named signal so clients stop
			// reconnecting, then return.
			writeSSEEvent(w, "server.draining", map[string]string{"reason": "shutdown"})
			flusher.Flush()
			return
		case <-r.Context().Done():
			return
		}
	}
}

// parseCursor reads the resume cursor: the `after` query parameter wins
// over the standard Last-Event-ID header (docs/SERVE_DESIGN.md §5.4).
func parseCursor(r *http.Request) (uint64, error) {
	raw := r.URL.Query().Get("after")
	if raw == "" {
		raw = r.Header.Get("Last-Event-ID")
	}
	if raw == "" {
		return 0, nil
	}
	cursor, err := strconv.ParseUint(raw, 10, 64)
	if err != nil {
		return 0, invalidInput("invalid event cursor")
	}
	return cursor, nil
}

func (s *Server) writeSSEHeaders(w http.ResponseWriter) {
	header := w.Header()
	header.Set("Content-Type", "text/event-stream")
	header.Set("Cache-Control", "no-cache")
	header.Set("Connection", "keep-alive")
	header.Set("X-Accel-Buffering", "no")
}

// writeSSEFrame writes one runtime event as an SSE frame.
func writeSSEFrame(w http.ResponseWriter, evt runtimeevent.RuntimeEvent) {
	data, err := json.Marshal(evt)
	if err != nil {
		return
	}
	fmt.Fprintf(w, "id: %d\n", evt.Sequence)
	fmt.Fprintf(w, "event: %s\n", evt.Kind)
	fmt.Fprintf(w, "data: %s\n\n", data)
}

// writeSSEEvent writes a named server event (server.resync /
// server.draining) — never a runtime event, never replayed.
func writeSSEEvent(w http.ResponseWriter, name string, payload any) {
	data, err := json.Marshal(payload)
	if err != nil {
		return
	}
	fmt.Fprintf(w, "event: %s\n", name)
	fmt.Fprintf(w, "data: %s\n\n", data)
}
