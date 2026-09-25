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
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/gorilla/websocket"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
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
	// Browsers upgrade the stream to WebSocket in place when possible:
	// every SSE stream pins one HTTP/1.1 connection for its lifetime, and
	// browser stacks cap at ~6 connections per host — half a dozen live
	// session streams starve the pool, after which ordinary API calls
	// (prompt submit included) queue client-side until they time out,
	// never reaching the server at all. WebSocket connections do not
	// count against that pool. Same URL, same frame format: each WS text
	// message carries exactly one SSE frame (docs/SERVE_DESIGN.md §5.4).
	if isWebSocketUpgrade(r) {
		s.handleSessionEventsWS(w, r, id)
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
		s.logger.Warn("sse stream rejected: too many streams", "session_id", id.String(), "max", maxSSEPerSession)
		writeError(w, &statusError{status: http.StatusTooManyRequests, code: "rate_limited", message: "too many event streams for this session"})
		return
	}
	defer s.releaseSSE(id.String())

	events, err := s.svc.SubscribeEvents(r.Context(), id, after)
	if errors.Is(err, app.ErrCursorInvalid) {
		// The cursor can no longer be honored: instruct the client to
		// rebuild from a snapshot and close (docs/SERVE_DESIGN.md §5.4).
		s.logger.Info("sse cursor invalid; client instructed to resync", "session_id", id.String(), "after", after)
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
	s.logger.Info("sse stream attached", "session_id", id.String(), "after", after)
	defer func() {
		// 覆盖所有退出路径（慢消费者断流、pump 重同步、客户端断开、drain）：
		// SSE 建断连此前无任何日志，断流重连类问题只能靠反推。
		s.logger.Info("sse stream detached", "session_id", id.String(), "draining", s.draining())
	}()

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
func writeSSEFrame(w io.Writer, evt runtimeevent.RuntimeEvent) {
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
func writeSSEEvent(w io.Writer, name string, payload any) {
	data, err := json.Marshal(payload)
	if err != nil {
		return
	}
	fmt.Fprintf(w, "event: %s\n", name)
	fmt.Fprintf(w, "data: %s\n\n", data)
}

// --- WebSocket transport ---

// wsWriteTimeout bounds a single frame write so a half-dead client (OS
// suspension, App Nap) cannot park the stream goroutine forever.
const wsWriteTimeout = 10 * time.Second

// wsUpgrader performs the RFC 6455 handshake. The bearer token gates the
// request regardless of origin, so cross-site WebSocket hijacking is
// already impossible; still, when a browser sends an Origin header it
// must name this very host.
var wsUpgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 8192,
	CheckOrigin: func(r *http.Request) bool {
		origin := r.Header.Get("Origin")
		if origin == "" {
			return true
		}
		u, err := url.Parse(origin)
		if err != nil {
			return false
		}
		return strings.EqualFold(u.Host, r.Host)
	},
}

// isWebSocketUpgrade reports whether the request asks to upgrade to a
// WebSocket (RFC 6455 §4.2.1): "Upgrade: websocket" plus a Connection
// header carrying the "upgrade" token.
func isWebSocketUpgrade(r *http.Request) bool {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		return false
	}
	for _, field := range strings.Split(r.Header.Get("Connection"), ",") {
		if strings.EqualFold(strings.TrimSpace(field), "upgrade") {
			return true
		}
	}
	return false
}

// writeWSMessage sends one WS text message with a bounded write deadline.
func writeWSMessage(conn *websocket.Conn, p []byte) error {
	_ = conn.SetWriteDeadline(time.Now().Add(wsWriteTimeout))
	return conn.WriteMessage(websocket.TextMessage, p)
}

// handleSessionEventsWS is handleSessionEvents over a WebSocket transport.
// Every text message carries exactly one SSE frame, so the client's frame
// parser (and the catch-up/cursor/slow-consumer semantics in
// SessionService) is shared verbatim with the SSE path.
func (s *Server) handleSessionEventsWS(w http.ResponseWriter, r *http.Request, id domain.SessionID) {
	after, err := parseCursor(r)
	if err != nil {
		writeError(w, err)
		return
	}
	if !s.acquireSSE(id.String()) {
		s.logger.Warn("event stream rejected: too many streams", "session_id", id.String(), "max", maxSSEPerSession, "transport", "websocket")
		writeError(w, &statusError{status: http.StatusTooManyRequests, code: "rate_limited", message: "too many event streams for this session"})
		return
	}
	defer s.releaseSSE(id.String())

	// Subscribe BEFORE the upgrade: every failure (session not live → 404,
	// cursor invalid → 409, draining → 503) must go out as an ordinary
	// HTTP error, because a browser WebSocket client cannot read the status
	// of a failed handshake — it falls back to the SSE transport, which
	// then reproduces the same status and drives the precise handling
	// (resume / resync / backoff). Answering 101 first and dying silently
	// would trap the client in a reconnect loop (review finding).
	// The subscription is ctx-scoped, so a failed upgrade afterwards still
	// cleans it up when the handler returns.
	events, err := s.svc.SubscribeEvents(r.Context(), id, after)
	if err != nil {
		if errors.Is(err, app.ErrCursorInvalid) {
			s.logger.Info("ws cursor invalid; client instructed to resync", "session_id", id.String(), "after", after)
		}
		writeError(w, err)
		return
	}

	conn, err := wsUpgrader.Upgrade(w, r, nil)
	if err != nil {
		// The upgrader has already written the failure response.
		return
	}
	defer conn.Close()

	if err := writeWSMessage(conn, []byte(fmt.Sprintf(": connected, instance=%s\n\n", s.instance))); err != nil {
		return
	}
	s.logger.Info("ws stream attached", "session_id", id.String(), "after", after)
	defer func() {
		// SSE 建断连日志的 WS 对照：断流重连类问题只能靠日志反推。
		s.logger.Info("ws stream detached", "session_id", id.String(), "draining", s.draining())
	}()

	// A read pump runs even though clients never send data: it processes
	// close frames and surfaces a vanished peer promptly (gorilla allows
	// exactly one concurrent reader and one concurrent writer).
	closed := make(chan struct{})
	go func() {
		defer close(closed)
		conn.SetReadLimit(4096)
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	}()

	heartbeat := time.NewTicker(sseHeartbeatInterval)
	defer heartbeat.Stop()
	for {
		select {
		case evt, ok := <-events:
			if !ok {
				if s.draining() {
					var buf bytes.Buffer
					writeSSEEvent(&buf, "server.draining", map[string]string{"reason": "shutdown"})
					_ = writeWSMessage(conn, buf.Bytes())
				}
				return
			}
			var buf bytes.Buffer
			writeSSEFrame(&buf, evt)
			if err := writeWSMessage(conn, buf.Bytes()); err != nil {
				return
			}
		case <-heartbeat.C:
			if err := writeWSMessage(conn, []byte(fmt.Sprintf(": hb %d\n\n", time.Now().Unix()))); err != nil {
				return
			}
		case <-s.shuttingDown():
			var buf bytes.Buffer
			writeSSEEvent(&buf, "server.draining", map[string]string{"reason": "shutdown"})
			_ = writeWSMessage(conn, buf.Bytes())
			return
		case <-closed:
			return
		case <-r.Context().Done():
			return
		}
	}
}
