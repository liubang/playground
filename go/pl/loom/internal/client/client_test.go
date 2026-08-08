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
// Created: 2026/08/03

package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestClientTypesAreJSONSerializable is the interface hard-constraint guard
// (docs/SERVE_DESIGN.md §17.5): every request/response type crossing the
// Client boundary must survive a JSON roundtrip unchanged, so the inproc
// and http implementations can never drift apart in shape.
func TestClientTypesAreJSONSerializable(t *testing.T) {
	approvalID := domain.NewEventID()
	samples := map[string]any{
		"Snapshot": Snapshot{
			State:         ControllerStateRunning,
			SessionID:     domain.NewSessionID(),
			RunID:         domain.NewRunID(),
			ModelName:     "gpt-5",
			TurnCount:     2,
			Usage:         domain.Usage{InputTokens: 10, OutputTokens: 20},
			EventSeq:      42,
			Timestamp:     time.Now().UTC(),
			PendingSteers: []string{"steer note"},
			Messages: []domain.Message{{
				ID:        domain.NewMessageID(),
				Role:      domain.RoleUser,
				Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}},
				CreatedAt: time.Now().UTC(),
			}},
			PendingRequests: []PendingRequest{
				{
					Kind: PendingRequestApproval,
					ID:   approvalID,
					Approval: &runtimeevent.ApprovalRequestedPayload{
						ApprovalID:  approvalID,
						CallID:      domain.NewToolCallID(),
						ToolName:    "edit",
						Risk:        domain.R2,
						Description: "edit file",
						ArgsHash:    "abc123",
						Diff:        "- a\n+ b",
						Arguments:   json.RawMessage(`{"path":"x.go"}`),
					},
				},
				{
					Kind: PendingRequestQuestion,
					ID:   domain.NewEventID(),
					Question: &domain.Question{
						ID:      domain.NewEventID(),
						Text:    "which one?",
						Options: []domain.QuestionOption{{Label: "a"}, {Label: "b", Description: "the b one"}},
					},
				},
			},
		},
		"SubmitResult":            SubmitResult{Steered: true, QueueLen: 2, Turn: 3},
		"ApprovalBinding":         ApprovalBinding{ApprovalID: approvalID, CallID: domain.NewToolCallID(), ArgsHash: "hash"},
		"AnswerQuestionResult":    AnswerQuestionResult{Resolved: true},
		"SetReasoningResult":      SetReasoningResult{},
		"RequestCompactionResult": RequestCompactionResult{AlreadyPending: true},
		"SessionSummary":          SessionSummary{ID: domain.NewSessionID(), Version: 7, CreatedAt: time.Now().UTC(), UpdatedAt: time.Now().UTC()},
		"RuleSet":                 &permission.RuleSet{},
		"SubagentView":            SubagentView{SessionID: domain.NewSessionID(), Active: true},
		"SkillsListing":           SkillsListing{Skills: []SkillInfo{{Name: "s", Description: "d", Scope: "user", Path: "/p"}}, Issues: []string{"i"}},
		"MCPServerInfo":           MCPServerInfo{Name: "m", Connected: true, Tools: []string{"t1"}},
		"SetModelResult":          SetModelResult{Prev: config.ProviderModelRef{Provider: "p", Model: "m1"}, Cur: config.ProviderModelRef{Provider: "p", Model: "m2"}},
		"CheckpointInfo":          CheckpointInfo{Sequence: 3, CreatedAt: time.Now().UTC(), Label: "l", Turns: 2},
		"RewindOutcome":           RewindOutcome{Checkpoint: CheckpointInfo{Sequence: 1}, Restored: []string{"a.go"}},
	}
	for name, sample := range samples {
		t.Run(name, func(t *testing.T) {
			data, err := json.Marshal(sample)
			if err != nil {
				t.Fatalf("marshal: %v", err)
			}
			typ := reflect.TypeOf(sample)
			if typ.Kind() == reflect.Ptr {
				typ = typ.Elem()
			}
			fresh := reflect.New(typ)
			if err := json.Unmarshal(data, fresh.Interface()); err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			again, err := json.Marshal(fresh.Interface())
			if err != nil {
				t.Fatalf("re-marshal: %v", err)
			}
			if !bytes.Equal(data, again) {
				t.Fatalf("roundtrip mismatch:\nfirst:  %s\nsecond: %s", data, again)
			}
		})
	}
}

// --- http transport regression tests (REVIEW.md round 2) ---
//
// newWireStub stands up a minimal loom wire endpoint on httptest: it
// answers session creation with a fresh id and lets each test install
// its own events/snapshot handlers, so SSE framing edge cases can be
// tested byte-for-byte without the full server stack.
func newWireStub(t *testing.T, events, snapshot http.HandlerFunc) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/sessions", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"session_id": %q}`, domain.NewSessionID().String())
	})
	if events != nil {
		mux.HandleFunc("GET /v1/sessions/{id}/events", events)
	}
	if snapshot != nil {
		mux.HandleFunc("GET /v1/sessions/{id}/snapshot", snapshot)
	}
	ts := httptest.NewServer(mux)
	t.Cleanup(ts.Close)
	return ts
}

func newBoundHTTPClient(t *testing.T, ts *httptest.Server) *httpClient {
	t.Helper()
	c := NewHTTP(ts.URL, "test-token").(*httpClient)
	if err := c.NewSession(context.Background()); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	t.Cleanup(c.Close)
	return c
}

// waitStreamClosed drains ch and fails unless it closes in time.
func waitStreamClosed(t *testing.T, ch <-chan runtimeevent.RuntimeEvent) {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		select {
		case _, ok := <-ch:
			if !ok {
				return
			}
		case <-deadline:
			t.Fatal("event stream did not close in time")
		}
	}
}

func testWireEvent(t *testing.T, seq uint64) runtimeevent.RuntimeEvent {
	t.Helper()
	return runtimeevent.RuntimeEvent{
		Version:   runtimeevent.RuntimeEventVersion,
		Sequence:  seq,
		SessionID: domain.NewSessionID(),
		Kind:      runtimeevent.KindTurnStarted,
		Time:      time.Now().UTC(),
		Durable:   true,
	}
}

// TestHTTPServerResyncTerminatesStream (M18): a server.resync frame must
// close the subscription promptly — even while the server holds the
// connection open — and must NOT be mistaken for a drain.
func TestHTTPServerResyncTerminatesStream(t *testing.T) {
	ts := newWireStub(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, ": connected, instance=test\n\n")
		fmt.Fprint(w, "event: server.resync\ndata: {\"reason\":\"cursor_invalid\"}\n\n")
		w.(http.Flusher).Flush()
		// Hold the connection open: the client must terminate on the
		// frame itself, not on connection close.
		<-r.Context().Done()
	}, nil)
	c := newBoundHTTPClient(t, ts)
	events, err := c.SubscribeEvents(context.Background(), 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	waitStreamClosed(t, events)
	select {
	case <-c.Done():
		t.Fatal("server.resync must not close Done — only draining may")
	default:
	}
}

// TestHTTPServerDrainingTerminatesStreamAndDone (M18/M19): a
// server.draining frame closes the subscription AND the client's Done
// channel, so callers stop reconnecting instead of storming a dying
// server (the same contract as the web SPA's drained flag).
func TestHTTPServerDrainingTerminatesStreamAndDone(t *testing.T) {
	ts := newWireStub(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "event: server.draining\ndata: {\"reason\":\"shutdown\"}\n\n")
		w.(http.Flusher).Flush()
		<-r.Context().Done()
	}, nil)
	c := newBoundHTTPClient(t, ts)
	events, err := c.SubscribeEvents(context.Background(), 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	waitStreamClosed(t, events)
	select {
	case <-c.Done():
	case <-time.After(5 * time.Second):
		t.Fatal("Done not closed after server.draining")
	}
}

// TestHTTPSubscribeEventsTightPrefixAndEOFFlush (M18): the spec-legal
// `data:`/`event:` prefixes without a space must parse, and a final
// frame flushed by EOF (no trailing blank line) must not be lost.
func TestHTTPSubscribeEventsTightPrefixAndEOFFlush(t *testing.T) {
	want := testWireEvent(t, 7)
	data, err := json.Marshal(want)
	if err != nil {
		t.Fatalf("marshal event: %v", err)
	}
	ts := newWireStub(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		// No space after the colon, and no blank line before EOF: the
		// parser must flush the frame at stream end.
		fmt.Fprintf(w, "event:%s\ndata:%s", want.Kind, data)
	}, nil)
	c := newBoundHTTPClient(t, ts)
	events, err := c.SubscribeEvents(context.Background(), 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	select {
	case got, ok := <-events:
		if !ok {
			t.Fatal("stream closed before the EOF-flushed frame")
		}
		if got.Kind != want.Kind || got.Sequence != want.Sequence {
			t.Fatalf("event = {kind:%s seq:%d}, want {kind:%s seq:%d}", got.Kind, got.Sequence, want.Kind, want.Sequence)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for the tight-prefix frame")
	}
	waitStreamClosed(t, events)
}

// TestPumpSSESurfacesReadErrors (M18): a stream that fails mid-read must
// not look like a clean end — events already parsed are delivered, the
// error is logged, and the channel still closes.
func TestPumpSSESurfacesReadErrors(t *testing.T) {
	pr, pw := io.Pipe()
	c := NewHTTP("http://127.0.0.1", "test-token").(*httpClient)
	t.Cleanup(c.Close)
	out := make(chan runtimeevent.RuntimeEvent, 1)
	go c.pumpSSE(context.Background(), pr, out)

	data, err := json.Marshal(testWireEvent(t, 1))
	if err != nil {
		t.Fatalf("marshal event: %v", err)
	}
	if _, err := fmt.Fprintf(pw, "data: %s\n\n", data); err != nil {
		t.Fatalf("write frame: %v", err)
	}
	if err := pw.CloseWithError(errors.New("connection reset")); err != nil {
		t.Fatalf("CloseWithError: %v", err)
	}
	select {
	case got, ok := <-out:
		if !ok {
			t.Fatal("channel closed before delivering the parsed event")
		}
		if got.Sequence != 1 {
			t.Fatalf("event sequence = %d, want 1", got.Sequence)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for the parsed event")
	}
	waitStreamClosed(t, out)
}

// TestHTTPStateErrorMapping (M19): State must never report a dead
// session as booting. A 404 (session gone for good) maps to Closed;
// transient failures keep the last known state.
func TestHTTPStateErrorMapping(t *testing.T) {
	const (
		modeOK int32 = iota
		mode500
		mode404
	)
	var mode int32
	ts := newWireStub(t, nil, func(w http.ResponseWriter, r *http.Request) {
		switch atomic.LoadInt32(&mode) {
		case modeOK:
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprint(w, `{"state":"idle"}`)
		case mode500:
			w.WriteHeader(http.StatusInternalServerError)
			fmt.Fprint(w, `{"error":{"code":"internal","message":"boom"}}`)
		case mode404:
			w.WriteHeader(http.StatusNotFound)
			fmt.Fprint(w, `{"error":{"code":"not_found","message":"gone"}}`)
		}
	})
	fresh := NewHTTP(ts.URL, "test-token")
	if got := fresh.State(); got != ControllerStateBooting {
		t.Fatalf("State before bind = %q, want booting", got)
	}
	c := newBoundHTTPClient(t, ts)
	if got := c.State(); got != ControllerStateIdle {
		t.Fatalf("State = %q, want idle", got)
	}
	atomic.StoreInt32(&mode, mode500)
	if got := c.State(); got != ControllerStateIdle {
		t.Fatalf("State after transient 500 = %q, want last known state idle", got)
	}
	atomic.StoreInt32(&mode, mode404)
	if got := c.State(); got != ControllerStateClosed {
		t.Fatalf("State after 404 = %q, want closed", got)
	}
}

// TestMapWireError (D9): wire codes map back to the application-layer
// sentinels; unknown codes stay opaque but keep the server message.
func TestMapWireError(t *testing.T) {
	sentinels := map[string]error{
		"not_found":             app.ErrSessionNotFound,
		"workspace_not_found":   app.ErrWorkspaceNotFound,
		"workspace_unavailable": app.ErrWorkspaceUnavailable,
		"workspace_in_use":      app.ErrWorkspaceInUse,
		"draining":              app.ErrDraining,
		"cursor_invalid":        app.ErrCursorInvalid,
		"rate_limited":          app.ErrTooManyTurns,
	}
	for code, want := range sentinels {
		if err := mapWireError(code, "x"); !errors.Is(err, want) {
			t.Errorf("mapWireError(%q) = %v, want %v", code, err, want)
		}
	}
	if err := mapWireError("mystery", "boom"); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Errorf("mapWireError(unknown) = %v, want an error keeping the server message", err)
	}
}

// TestHTTPSubscribeEventsNonEnvelopeError (R15): an error body that is
// not the wire envelope (a proxy page, a truncated body) must still
// surface the HTTP status instead of an opaque decode failure.
func TestHTTPSubscribeEventsNonEnvelopeError(t *testing.T) {
	ts := newWireStub(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadGateway)
		fmt.Fprint(w, "<html>proxy exploded</html>")
	}, nil)
	c := newBoundHTTPClient(t, ts)
	_, err := c.SubscribeEvents(context.Background(), 0)
	if err == nil || !strings.Contains(err.Error(), "502") {
		t.Fatalf("SubscribeEvents error = %v, want a status-carrying error", err)
	}
}
