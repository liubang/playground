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
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

const testToken = "test-token-0123456789abcdef"

// newTestService builds a SessionService over a real SQLite store with a
// fake model.
func newTestService(t *testing.T, model domain.Model) *app.SessionService {
	t.Helper()
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	resolved := &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:         "test",
			Model:        model,
			Models:       []config.Model{{Name: "model-a", ContextWindow: 128000}},
			DefaultModel: "model-a",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "model-a"},
		Limits:  domain.DefaultLimits(),
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSessionService(&app.Bootstrap{
		Resolved: resolved,
		Current:  resolved.Default,
		Store:    store,
		Registry: agent.NewToolRegistry(),
	}, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	return svc
}

// newTestServer builds a SessionService over a real SQLite store with a
// fake model, and an httptest server running the adapter against it.
func newTestServer(t *testing.T, model domain.Model) (*httptest.Server, *app.SessionService) {
	t.Helper()
	svc := newTestService(t, model)
	srv, err := New(Config{
		Token:   testToken,
		Version: "test",
		Service: svc,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.http.Handler)
	t.Cleanup(ts.Close)
	return ts, svc
}

func authed(t *testing.T, req *http.Request) {
	t.Helper()
	req.Header.Set("Authorization", "Bearer "+testToken)
}

func doJSON(t *testing.T, client *http.Client, method, url string, body string) (int, map[string]any) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	authed(t, req)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("%s %s: %v", method, url, err)
	}
	defer resp.Body.Close()
	var decoded map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
		t.Fatalf("%s %s: decode response: %v", method, url, err)
	}
	return resp.StatusCode, decoded
}

func createTestSession(t *testing.T, ts *httptest.Server) string {
	t.Helper()
	status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", "")
	if status != http.StatusCreated {
		t.Fatalf("POST /v1/sessions status = %d, want 201 (%v)", status, body)
	}
	id, _ := body["session_id"].(string)
	if id == "" {
		t.Fatalf("POST /v1/sessions: no session_id in %v", body)
	}
	return id
}

func waitIdle(t *testing.T, ts *httptest.Server, id string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		_, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id+"/snapshot", "")
		if body["state"] == "idle" {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("session %s did not become idle", id)
}

func TestAuthRequired(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())

	// Health routes are exempt.
	resp, err := ts.Client().Get(ts.URL + "/healthz")
	if err != nil {
		t.Fatalf("GET /healthz: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /healthz status = %d, want 200", resp.StatusCode)
	}

	// /v1/* requires a token.
	resp, err = ts.Client().Get(ts.URL + "/v1/sessions")
	if err != nil {
		t.Fatalf("GET /v1/sessions: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("GET /v1/sessions without token status = %d, want 401", resp.StatusCode)
	}

	// Wrong token.
	req, _ := http.NewRequest("GET", ts.URL+"/v1/sessions", nil)
	req.Header.Set("Authorization", "Bearer wrong")
	resp, err = ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET /v1/sessions (wrong token): %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("GET /v1/sessions with wrong token status = %d, want 401", resp.StatusCode)
	}
}

func TestMetaVersion(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	_, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/meta/version", "")
	if body["protocol"] != float64(1) {
		t.Fatalf("protocol = %v, want 1", body["protocol"])
	}
	if body["instance"] == "" {
		t.Fatalf("instance missing in %v", body)
	}
}

// TestMetaModels: the catalog endpoint exposes every configured model plus
// the process default, so the SPA model picker has its data source.
func TestMetaModels(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	_, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/meta/models", "")
	def, _ := body["default"].(string)
	if def == "" {
		t.Fatalf("default missing in %v", body)
	}
	models, _ := body["models"].([]any)
	if len(models) == 0 {
		t.Fatalf("models empty in %v", body)
	}
	first, _ := models[0].(map[string]any)
	if first["provider"] == "" || first["name"] == "" {
		t.Fatalf("model entry malformed: %v", first)
	}
}

func TestSessionLifecycleEndpoints(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)

	id := createTestSession(t, ts)

	// Snapshot reflects the fresh session.
	_, snap := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id+"/snapshot", "")
	if snap["state"] != "idle" || snap["session_id"] != id {
		t.Fatalf("snapshot = %v", snap)
	}

	// Submit a prompt; the fake model answers.
	status, submitted := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`)
	if status != http.StatusAccepted || submitted["turn"] != float64(1) {
		t.Fatalf("submit = (%d, %v), want (202, turn 1)", status, submitted)
	}
	waitIdle(t, ts, id)

	// The transcript carries the exchange.
	_, page := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id+"/transcript", "")
	messages, _ := page["messages"].([]any)
	if len(messages) < 2 {
		t.Fatalf("transcript messages = %v, want the user+assistant pair", messages)
	}

	// Session listing and inspection cover the live session.
	_, listing := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
	if sessions, _ := listing["sessions"].([]any); len(sessions) != 1 {
		t.Fatalf("sessions = %v, want exactly 1", listing)
	}
	_, inspection := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+id, "")
	if inspection["events"] != nil {
		t.Fatalf("inspection must not include events: %v", inspection)
	}

	// Resuming the live session is a no-op 200.
	status, _ = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", fmt.Sprintf(`{"resume":%q}`, id))
	if status != http.StatusOK {
		t.Fatalf("resume live session status = %d, want 200", status)
	}
}

func TestSubmitPromptIdempotencyHeader(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	post := func() (int, map[string]any) {
		req, _ := http.NewRequest("POST", ts.URL+"/v1/sessions/"+id+"/prompts", strings.NewReader(`{"prompt":"hi"}`))
		authed(t, req)
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Idempotency-Key", "k-1")
		resp, err := ts.Client().Do(req)
		if err != nil {
			t.Fatalf("POST prompts: %v", err)
		}
		defer resp.Body.Close()
		var decoded map[string]any
		if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
			t.Fatalf("decode: %v", err)
		}
		return resp.StatusCode, decoded
	}

	status, first := post()
	if status != http.StatusAccepted || first["deduplicated"] == true {
		t.Fatalf("first submit = (%d, %v)", status, first)
	}
	waitIdle(t, ts, id)
	status, second := post()
	if status != http.StatusOK || second["deduplicated"] != true {
		t.Fatalf("repeat submit = (%d, %v), want (200, deduplicated)", status, second)
	}
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls = %d, want 1", calls)
	}
}

func TestPromptNotIdleAndSteer(t *testing.T) {
	// A gated model keeps the first turn busy while we steer.
	release := make(chan struct{})
	model := &gateModel{inner: fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second", StopReason: domain.StopEndTurn},
	), release: release}
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"first"}`)
	if status != http.StatusAccepted {
		t.Fatalf("first submit status = %d", status)
	}
	// The turn is busy: the next submission steers instead of failing.
	status, steered := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"second"}`)
	if status != http.StatusAccepted || steered["steered"] != true {
		t.Fatalf("steer submit = (%d, %v), want (202, steered:true)", status, steered)
	}
	close(release)
	waitIdle(t, ts, id)
}

func TestResolveApprovalBindingMismatch(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)
	status, body := doJSON(t, ts.Client(), "POST",
		ts.URL+"/v1/sessions/"+id+"/approvals/evt_00000000000000000000000000000000",
		`{"call_id":"tc_00000000000000000000000000000000","args_hash":"x","decision":"allow"}`)
	if status != http.StatusConflict {
		t.Fatalf("resolve with bogus binding = (%d, %v), want 409", status, body)
	}
	// The state gate fires before the binding check: an idle session
	// reports not_idle (both are 409 in the wire model).
	if errObj, _ := body["error"].(map[string]any); errObj["code"] != "not_idle" {
		t.Fatalf("error body = %v, want not_idle", body)
	}
}

func TestSSEEventStream(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "streamed", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", ts.URL+"/v1/sessions/"+id+"/events", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET events: %v", err)
	}
	defer resp.Body.Close()
	if ct := resp.Header.Get("Content-Type"); ct != "text/event-stream" {
		t.Fatalf("Content-Type = %q, want text/event-stream", ct)
	}

	lines := make(chan string, 64)
	go func() {
		scanner := bufio.NewScanner(resp.Body)
		for scanner.Scan() {
			lines <- scanner.Text()
		}
	}()

	// First frame: the connected comment carrying the instance ID.
	first := <-lines
	if !strings.HasPrefix(first, ": connected, instance=") {
		t.Fatalf("first SSE line = %q, want the connected comment", first)
	}

	// Drive a turn; the stream must carry turn.started with a sequence id.
	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`); status != http.StatusAccepted {
		t.Fatalf("POST prompts = (%d, %v)", status, body)
	}
	var sawID, sawTurnStarted bool
	deadline := time.After(5 * time.Second)
	for !(sawID && sawTurnStarted) {
		select {
		case line := <-lines:
			if strings.HasPrefix(line, "id: ") {
				sawID = true
			}
			if line == "event: turn.started" {
				sawTurnStarted = true
			}
		case <-deadline:
			t.Fatalf("timed out waiting for turn.started (id=%v started=%v)", sawID, sawTurnStarted)
		}
	}
}

func TestSSEInvalidCursorResync(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", ts.URL+"/v1/sessions/"+id+"/events?after=99999999", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET events: %v", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	if !strings.Contains(string(body), "event: server.resync") {
		t.Fatalf("body = %q, want a server.resync frame", body)
	}
}

func TestDrainingRejectsNewWork(t *testing.T) {
	ts, svc := newTestServer(t, fakes.NewFakeModel())
	if err := svc.Shutdown(context.Background()); err != nil {
		t.Fatalf("service shutdown: %v", err)
	}
	status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", "")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("create session while draining = (%d, %v), want 503", status, body)
	}
}

// TestShutdownSignalsSSEDraining locks in the graceful-stop contract
// (docs/SERVE_DESIGN.md §7.3): Shutdown must push a server.draining frame
// to live SSE streams and let their handlers return promptly —
// http.Server.Shutdown alone never cancels in-flight long connections.
func TestShutdownSignalsSSEDraining(t *testing.T) {
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	id := createTestSession(t, ts)

	sseCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	req, _ := http.NewRequestWithContext(sseCtx, "GET", ts.URL+"/v1/sessions/"+id+"/events", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET events: %v", err)
	}
	defer resp.Body.Close()
	lines := make(chan string, 64)
	go func() {
		scanner := bufio.NewScanner(resp.Body)
		for scanner.Scan() {
			lines <- scanner.Text()
		}
	}()
	if first := <-lines; !strings.HasPrefix(first, ": connected, instance=") {
		t.Fatalf("first SSE line = %q, want the connected comment", first)
	}

	shutdownDone := make(chan error, 1)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		shutdownDone <- srv.Shutdown(ctx)
	}()

	deadline := time.After(5 * time.Second)
	for {
		select {
		case line := <-lines:
			if line == "event: server.draining" {
				goto drained
			}
		case <-deadline:
			t.Fatalf("no server.draining frame on shutdown")
		}
	}
drained:
	select {
	case err := <-shutdownDone:
		if err != nil {
			t.Fatalf("Shutdown: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatalf("Shutdown did not return promptly with a live SSE stream")
	}
}

// TestWebStaticAndSecurityHeaders covers the embedded SPA mount and the
// uniform security headers (docs/WEB_DESIGN.md §7.1/§7.2).
func TestWebStaticAndSecurityHeaders(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())

	// Static assets are reachable without a token (the token gate must be
	// anonymous), and every response carries the security headers.
	resp, err := ts.Client().Get(ts.URL + "/")
	if err != nil {
		t.Fatalf("GET /: %v", err)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET / status = %d", resp.StatusCode)
	}
	if !strings.Contains(string(body), `id="gate"`) {
		t.Fatalf("index missing the token gate")
	}
	for header, want := range map[string]string{
		"Content-Security-Policy": "default-src 'self'",
		"X-Content-Type-Options":  "nosniff",
		"X-Frame-Options":         "DENY",
	} {
		if got := resp.Header.Get(header); !strings.Contains(got, want) {
			t.Fatalf("%s = %q, want substring %q", header, got, want)
		}
	}

	// API responses carry the headers too (uniform middleware).
	req, _ := http.NewRequest("GET", ts.URL+"/v1/meta/version", nil)
	authed(t, req)
	apiResp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET /v1/meta/version: %v", err)
	}
	apiResp.Body.Close()
	if apiResp.Header.Get("X-Frame-Options") != "DENY" {
		t.Fatalf("API response missing X-Frame-Options")
	}
}

// TestNoWebDisablesSPA: --no-web serves pure API (docs/WEB_DESIGN.md §7.1).
func TestNoWebDisablesSPA(t *testing.T) {
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc, NoWeb: true})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	resp, err := ts.Client().Get(ts.URL + "/")
	if err != nil {
		t.Fatalf("GET /: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("GET / with NoWeb status = %d, want 404", resp.StatusCode)
	}
}

// TestSessionListEnrichment locks the enriched SessionSummary wire shape
// (docs/WEB_DESIGN.md §7.6/§7.7): snake_case keys, live state/model, title
// from the first user prompt.
func TestSessionListEnrichment(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"fix the flaky test in sstv2"}`); status != http.StatusAccepted {
		t.Fatalf("submit = (%d, %v)", status, body)
	}
	waitIdle(t, ts, id)

	_, listing := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
	sessions, _ := listing["sessions"].([]any)
	if len(sessions) != 1 {
		t.Fatalf("sessions = %v, want exactly 1", listing)
	}
	entry, _ := sessions[0].(map[string]any)
	if entry["id"] != id {
		t.Fatalf("entry id = %v (snake_case key check)", entry)
	}
	if entry["state"] != "idle" {
		t.Fatalf("state = %v, want idle (live session)", entry["state"])
	}
	if entry["model_name"] != "model-a" {
		t.Fatalf("model_name = %v", entry["model_name"])
	}
	if entry["turn_count"] != float64(1) {
		t.Fatalf("turn_count = %v", entry["turn_count"])
	}
	if entry["title"] != "fix the flaky test in sstv2" {
		t.Fatalf("title = %v", entry["title"])
	}
	if _, hasCapitalized := entry["ID"]; hasCapitalized {
		t.Fatalf("legacy capitalized keys must be gone: %v", entry)
	}
}

// gateModel blocks its first Stream call until release closes, giving the
// test a deterministic busy window.
type gateModel struct {
	inner   *fakes.FakeModel
	release chan struct{}
	once    bool
}

func (m *gateModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	if !m.once {
		m.once = true
		select {
		case <-m.release:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
	return m.inner.Stream(ctx, req)
}
