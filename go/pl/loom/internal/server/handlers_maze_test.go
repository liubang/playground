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

package server

import (
	"encoding/json"
	"net/http"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// TestSessionMazeEndpoint: the authenticated maze endpoint projects the
// session's event log into one lane with a settled answer step.
func TestSessionMazeEndpoint(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`)
	if status != http.StatusAccepted {
		t.Fatalf("submit status = %d, want 202", status)
	}
	waitIdle(t, ts, id)

	status, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id+"/maze", "")
	if status != http.StatusOK {
		t.Fatalf("GET maze status = %d, want 200 (%v)", status, body)
	}
	lanes, _ := body["lanes"].([]any)
	if len(lanes) != 1 {
		t.Fatalf("maze lanes = %d, want 1 (%v)", len(lanes), body)
	}
	lane, _ := lanes[0].(map[string]any)
	if lane["model"] != "model-a" {
		t.Fatalf("lane model = %v, want model-a", lane["model"])
	}
	stats, _ := lane["stats"].(map[string]any)
	if steps, _ := stats["steps"].(float64); steps < 1 {
		t.Fatalf("lane stats.steps = %v, want >= 1", stats)
	}
	main, _ := lane["main"].([]any)
	if len(main) != 1 {
		t.Fatalf("main = %d, want 1 answer step", len(main))
	}
	step, _ := main[0].(map[string]any)
	if step["v"] != "answer" || step["turn"] != float64(1) {
		t.Fatalf("step v/turn = %v/%v, want answer/1", step["v"], step["turn"])
	}
	// The per-request usage truth persisted on response_completed flows
	// through to the maze (the persistence contract).
	if step["out_tok"] == nil {
		t.Fatalf("step missing per-request usage: %v", step)
	}
	if tmax, _ := body["tmax"].(float64); tmax < 60 {
		t.Fatalf("tmax = %v, want >= 60 (minimum axis window)", tmax)
	}
}

// TestSessionMazeEndpointEmpty: a session with no activity yields an empty
// but well-formed lane (the frontend shows the empty hint).
func TestSessionMazeEndpointEmpty(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)
	status, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id+"/maze", "")
	if status != http.StatusOK {
		t.Fatalf("GET maze status = %d, want 200 (%v)", status, body)
	}
	lanes, _ := body["lanes"].([]any)
	if len(lanes) != 1 {
		t.Fatalf("lanes = %d, want 1 (%v)", len(lanes), body)
	}
	lane, _ := lanes[0].(map[string]any)
	stats, _ := lane["stats"].(map[string]any)
	if steps, _ := stats["steps"].(float64); steps != 0 {
		t.Fatalf("steps = %v, want 0", stats)
	}
	// Wire contract: main/detours serialize as empty arrays, never null —
	// the frontend iterates them without nil guards.
	if lane["main"] == nil || lane["detours"] == nil {
		t.Fatalf("main/detours must be [] not null: %v", lane)
	}
}

// TestSharedMazeEndpoint: the public maze rides the share-token capability —
// reachable without credentials while shared, 404 after revoke.
func TestSharedMazeEndpoint(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "shared answer", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`)
	if status != http.StatusAccepted {
		t.Fatalf("submit status = %d, want 202", status)
	}
	waitIdle(t, ts, id)

	status, share := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	if status != http.StatusOK {
		t.Fatalf("POST share status = %d, want 200 (%v)", status, share)
	}
	token, _ := share["token"].(string)

	status, body := getPublic(t, ts.URL+"/v1/shared/"+token+"/maze")
	if status != http.StatusOK {
		t.Fatalf("GET shared maze status = %d, want 200 (%s)", status, body)
	}
	var maze map[string]any
	if err := json.Unmarshal(body, &maze); err != nil {
		t.Fatalf("decode shared maze: %v", err)
	}
	if lanes, _ := maze["lanes"].([]any); len(lanes) != 1 {
		t.Fatalf("shared maze lanes = %v, want 1", maze["lanes"])
	}

	// Malformed token shape → 404 (existence is not revealed).
	status, _ = getPublic(t, ts.URL+"/v1/shared/not-a-token/maze")
	if status != http.StatusNotFound {
		t.Fatalf("bad token status = %d, want 404", status)
	}

	// Revoking invalidates the link immediately (DELETE answers 204 with an
	// empty body, so use a raw request).
	req, _ := http.NewRequest("DELETE", ts.URL+"/v1/sessions/"+id+"/share", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("DELETE share: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent && resp.StatusCode != http.StatusOK {
		t.Fatalf("revoke status = %d", resp.StatusCode)
	}
	status, _ = getPublic(t, ts.URL+"/v1/shared/"+token+"/maze")
	if status != http.StatusNotFound {
		t.Fatalf("after revoke status = %d, want 404", status)
	}
}
