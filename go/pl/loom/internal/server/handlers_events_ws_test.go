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
// Created: 2026/09/25

package server

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// wsURL rewrites the httptest base URL into a ws:// events URL carrying the
// token as a query parameter, exactly like the browser client (the
// WebSocket API cannot set headers).
func wsURL(ts *httptest.Server, sessionID, query string) string {
	base := "ws" + strings.TrimPrefix(ts.URL, "http")
	url := base + "/v1/sessions/" + sessionID + "/events"
	if query != "" {
		url += "?" + query
	}
	return url
}

// dialWS dials the events endpoint with a WebSocket upgrade and returns the
// connection plus the handshake response (nil on success).
func dialWS(t *testing.T, url string, header http.Header) (*websocket.Conn, *http.Response, error) {
	t.Helper()
	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	return dialer.Dial(url, header)
}

// readWSMessage reads one text message with a deadline so a stuck stream
// fails the test instead of hanging it.
func readWSMessage(t *testing.T, conn *websocket.Conn) string {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	typ, msg, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read ws message: %v", err)
	}
	if typ != websocket.TextMessage {
		t.Fatalf("ws message type = %d, want text", typ)
	}
	return string(msg)
}

func TestWebSocketEventStream(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "streamed", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	// Authorization-header auth, the native-client (Swift) style.
	header := http.Header{"Authorization": []string{"Bearer " + testToken}}
	conn, _, err := dialWS(t, wsURL(ts, id, ""), header)
	if err != nil {
		t.Fatalf("ws dial: %v", err)
	}
	defer conn.Close()

	// First message: the connected banner carrying the instance ID — the
	// exact same frame the SSE transport emits first.
	if banner := readWSMessage(t, conn); !strings.HasPrefix(banner, ": connected, instance=") {
		t.Fatalf("first ws message = %q, want the connected banner", banner)
	}

	// Drive a turn; the stream must carry turn.started with a sequence id,
	// packed as one SSE frame per WS message.
	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`); status != http.StatusAccepted {
		t.Fatalf("POST prompts = (%d, %v)", status, body)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		msg := readWSMessage(t, conn)
		if strings.Contains(msg, "event: turn.started") {
			if !strings.Contains(msg, "id: ") {
				t.Fatalf("turn.started frame lacks a sequence id: %q", msg)
			}
			return
		}
	}
	t.Fatal("timed out waiting for turn.started over websocket")
}

// Subscription failures must answer BEFORE the 101 upgrade as ordinary
// HTTP errors: a browser WS client cannot read a failed handshake's
// status, so it falls back to the SSE transport, which reproduces the
// same status and drives the precise handling (resync/resume/backoff).
func TestWebSocketInvalidCursorResync(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)

	_, resp, err := dialWS(t, wsURL(ts, id, "after=99999999&token="+testToken), nil)
	if err == nil {
		t.Fatal("ws dial with an invalid cursor succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusConflict {
		t.Fatalf("ws dial with an invalid cursor: status = %v, want 409 cursor_invalid", resp)
	}
}

func TestWebSocketSessionNotLiveReturns404(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())

	_, resp, err := dialWS(t, wsURL(ts, "sess_00000000000000000000000000000000", "token="+testToken), nil)
	if err == nil {
		t.Fatal("ws dial for a dead session succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusNotFound {
		t.Fatalf("ws dial for a dead session: status = %v, want 404", resp)
	}
}

func TestWebSocketQueryTokenAuth(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)

	// Wrong query token must fail the handshake with the middleware's 401,
	// before any upgrade happens.
	_, resp, err := dialWS(t, wsURL(ts, id, "token=wrong"), nil)
	if err == nil {
		t.Fatal("ws dial with a wrong token succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("ws dial with a wrong token: status = %v, want 401", resp)
	}

	// The query-token exception is scoped to WebSocket upgrades: a plain
	// SSE request without the Authorization header still gets 401 even
	// with the correct token in the query.
	req, _ := http.NewRequest("GET", ts.URL+"/v1/sessions/"+id+"/events?token="+testToken, nil)
	sseResp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET events: %v", err)
	}
	sseResp.Body.Close()
	if sseResp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("plain GET with only a query token: status = %d, want 401 (query token must not authenticate non-WS requests)", sseResp.StatusCode)
	}
}

func TestWebSocketOriginCheck(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)

	header := http.Header{"Origin": []string{"http://evil.example"}}
	_, resp, err := dialWS(t, wsURL(ts, id, "token="+testToken), header)
	if err == nil {
		t.Fatal("ws dial with a foreign Origin succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusForbidden {
		t.Fatalf("ws dial with a foreign Origin: status = %v, want 403", resp)
	}
}
