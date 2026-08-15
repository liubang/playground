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
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
)

const testToken = "test-token-0123456789abcdef"

// newTestService builds a SessionService over a real SQLite store with a
// fake model.
func newTestService(t *testing.T, model domain.Model) *app.SessionService {
	t.Helper()
	return newTestServiceWithRecorder(t, model, nil)
}

// newTestServiceWithRecorder is newTestService with an optional trace
// recorder wired into the process runtime (nil = tracing disabled).
func newTestServiceWithRecorder(t *testing.T, model domain.Model, rec trace.Recorder) *app.SessionService {
	t.Helper()
	return newTestServiceFull(t, model, rec, app.SessionServiceConfig{})
}

// newTestServiceFull is newTestServiceWithRecorder with an explicit
// SessionServiceConfig (e.g. wiring the share-endpoint controller).
func newTestServiceFull(t *testing.T, model domain.Model, rec trace.Recorder, svcCfg app.SessionServiceConfig) *app.SessionService {
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
		// A random loopback port keeps share-endpoint tests free of
		// conflicts with any real listener.
		Share: config.ResolvedShare{Listen: "127.0.0.1:0"},
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	// The artifact store is process-level; a workspace assembled on demand
	// (registering a new workspace builds its runtime) needs a real one.
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), resolved.Limits.MaxArtifactBytes)
	if err != nil {
		t.Fatalf("open artifact store: %v", err)
	}
	proc := &app.ProcessRuntime{
		Current:  resolved.Default,
		Store:    store,
		Artifact: artStore,
		Recorder: rec,
		// A workspace assembled on demand (registering a new one) wires the
		// ask_user tool, which requires a non-nil questioner.
		Questioner: domain.AutonomousQuestioner{},
	}
	proc.SwapResolved(resolved)
	svc := app.NewSingletonWorkspaceService(&app.Bootstrap{
		ProcessRuntime: proc,
		Registry:       agent.NewToolRegistry(),
	}, broker, svcCfg)
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	return svc
}

// newTestServer builds a SessionService over a real SQLite store with a
// fake model, and an httptest server running the adapter against it.
func newTestServer(t *testing.T, model domain.Model) (*httptest.Server, *app.SessionService) {
	t.Helper()
	return newTestServerWithRecorder(t, model, nil)
}

// newTestServerWithRecorder is newTestServer with an optional trace recorder.
func newTestServerWithRecorder(t *testing.T, model domain.Model, rec trace.Recorder) (*httptest.Server, *app.SessionService) {
	t.Helper()
	svc := newTestServiceWithRecorder(t, model, rec)
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

// TestMetaEnvironment: the environment endpoint serves the cached
// toolchain report (produced by the startup PATH augmentation), so the
// settings card can render the effective PATH without any probing of its
// own.
func TestMetaEnvironment(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	status, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/meta/environment", "")
	if status == http.StatusServiceUnavailable {
		// The test runtime may skip PATH augmentation; the 503 contract is
		// part of the API surface.
		return
	}
	if _, ok := body["effective_path"].(string); !ok {
		t.Fatalf("effective_path missing in %v", body)
	}
	if _, ok := body["dirs"].([]any); !ok {
		t.Fatalf("dirs missing in %v", body)
	}
	if _, ok := body["tools"].([]any); !ok {
		t.Fatalf("tools missing in %v", body)
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

// TestSessionListLiveState locks that a busy session's live controller state
// is projected into the session listing — sidebar status dots and the
// per-workspace attention badge depend on non-idle states showing up here.
func TestSessionListLiveState(t *testing.T) {
	release := make(chan struct{})
	model := &gateModel{inner: fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn},
	), release: release}
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"first"}`); status != http.StatusAccepted {
		t.Fatalf("submit = (%d, %v)", status, body)
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		_, listing := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
		sessions, _ := listing["sessions"].([]any)
		if len(sessions) != 1 {
			t.Fatalf("sessions = %v, want exactly 1", listing)
		}
		entry, _ := sessions[0].(map[string]any)
		if entry["state"] == "running" {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("state = %v, want running (busy live session)", entry["state"])
		}
		time.Sleep(20 * time.Millisecond)
	}
	close(release)
	waitIdle(t, ts, id)
}

// TestSessionListAwaitingApproval locks the approval-state projection the
// per-workspace attention badge counts: a live session in awaiting_approval
// must show up as such in the listing, and flip back once the last pending
// card resolves. The controller setters stand in for the publishing store
// hooks that fire when permission events persist (covered in app tests).
func TestSessionListAwaitingApproval(t *testing.T) {
	release := make(chan struct{})
	model := &gateModel{inner: fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn},
	), release: release}
	ts, svc := newTestServer(t, model)
	id := createTestSession(t, ts)

	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"first"}`); status != http.StatusAccepted {
		t.Fatalf("submit = (%d, %v)", status, body)
	}
	// Wait for the turn to hold the controller in running state.
	deadline := time.Now().Add(5 * time.Second)
	for {
		_, listing := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
		sessions, _ := listing["sessions"].([]any)
		entry, _ := sessions[0].(map[string]any)
		if entry["state"] == "running" {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("state = %v, want running first", entry["state"])
		}
		time.Sleep(20 * time.Millisecond)
	}

	sid, err := domain.ParseSessionID(id)
	if err != nil {
		t.Fatalf("ParseSessionID: %v", err)
	}
	h, ok := svc.Get(sid)
	if !ok {
		t.Fatalf("session %s is not live", id)
	}
	listState := func() any {
		t.Helper()
		_, listing := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
		sessions, _ := listing["sessions"].([]any)
		if len(sessions) != 1 {
			t.Fatalf("sessions = %v, want exactly 1", listing)
		}
		entry, _ := sessions[0].(map[string]any)
		return entry["state"]
	}

	// A pending approval must surface in the listing — this row is exactly
	// what the workspace attention badge counts.
	h.Controller.SetAwaitingApproval()
	if state := listState(); state != "awaiting_approval" {
		t.Fatalf("state = %v, want awaiting_approval", state)
	}

	// Resolving the last pending card (pendingCards is empty here) flips the
	// controller back to running, and the listing follows — the badge must
	// clear instead of lingering on a stale count.
	h.Controller.SetRunning()
	if state := listState(); state != "running" {
		t.Fatalf("state = %v, want running after resolve", state)
	}

	close(release)
	waitIdle(t, ts, id)
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

// TestWorkspaceEndpoints locks the workspace REST contract
// (docs/WORKSPACE_DESIGN.md §8.1): register (idempotent by canonical root),
// list, get-by-id, workspace-scoped session create and list filtering.
func TestWorkspaceEndpoints(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	// Roots are stored canonicalized (EvalSymlinks resolves macOS /var →
	// /private/var), so compare against canonicalized temp dirs.
	rootA := canonicalTempDir(t)
	rootB := canonicalTempDir(t)

	status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/workspaces", fmt.Sprintf(`{"root_path":%q,"name":"alpha"}`, rootA))
	if status != http.StatusOK {
		t.Fatalf("register A = (%d, %v)", status, body)
	}
	wsA, _ := body["workspace"].(map[string]any)
	idA, _ := wsA["id"].(string)
	if idA == "" || wsA["name"] != "alpha" || wsA["root_path"] != rootA {
		t.Fatalf("bad workspace A: %v", wsA)
	}

	// Re-registering the same canonical root reuses the workspace ID.
	_, reuse := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/workspaces", fmt.Sprintf(`{"root_path":%q}`, rootA))
	if got, _ := reuse["workspace"].(map[string]any)["id"].(string); got != idA {
		t.Fatalf("re-register id = %s, want reuse %s", got, idA)
	}

	_, bodyB := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/workspaces", fmt.Sprintf(`{"root_path":%q,"name":"beta"}`, rootB))
	idB, _ := bodyB["workspace"].(map[string]any)["id"].(string)

	_, list := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/workspaces", "")
	names := map[string]string{}
	for _, w := range list["workspaces"].([]any) {
		m := w.(map[string]any)
		names[m["id"].(string)], _ = m["name"].(string)
	}
	if names[idA] != "alpha" || names[idB] != "beta" {
		t.Fatalf("list missing workspaces: %v", names)
	}

	_, one := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/workspaces/"+idA, "")
	if got, _ := one["workspace"].(map[string]any)["root_path"].(string); got != rootA {
		t.Fatalf("get by id root = %q, want %q", got, rootA)
	}

	// Create a session in workspace A; the response carries workspace_id.
	status, created := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", fmt.Sprintf(`{"workspace_id":%q}`, idA))
	if status != http.StatusCreated {
		t.Fatalf("create in A = (%d, %v)", status, created)
	}
	sessA, _ := created["session_id"].(string)
	if created["workspace_id"] != idA {
		t.Fatalf("session workspace_id = %v, want %s", created["workspace_id"], idA)
	}
	if _, cB := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", fmt.Sprintf(`{"workspace_id":%q}`, idB)); cB["workspace_id"] != idB {
		t.Fatalf("session in B workspace_id = %v", cB["workspace_id"])
	}

	// List filtered by workspace A returns only A's session.
	_, filtered := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions?workspace_id="+idA, "")
	var ids []string
	for _, s := range filtered["sessions"].([]any) {
		ids = append(ids, s.(map[string]any)["id"].(string))
	}
	if len(ids) != 1 || ids[0] != sessA {
		t.Fatalf("filter A ids = %v, want [%s]", ids, sessA)
	}

	// Unknown workspace id → 404; malformed → 400.
	if status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", `{"workspace_id":"ws_00000000000000000000000000000000"}`); status != http.StatusNotFound {
		t.Fatalf("unknown workspace = (%d, %v), want 404", status, body)
	}
	if status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", `{"workspace_id":"bogus"}`); status != http.StatusBadRequest {
		t.Fatalf("invalid workspace id = %d, want 400", status)
	}
}

// doDelete issues a bare DELETE and returns the status code (204 responses
// carry no body, so doJSON's decode step does not apply).
func doDelete(t *testing.T, client *http.Client, url string) int {
	t.Helper()
	req, err := http.NewRequest(http.MethodDelete, url, nil)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	authed(t, req)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("DELETE %s: %v", url, err)
	}
	defer resp.Body.Close()
	return resp.StatusCode
}

// TestDeleteWorkspaceEndpoint locks the workspace-deletion contract
// (docs/WORKSPACE_DESIGN.md §16.1): deletion cascades to the workspace's
// sessions — live sessions are shut down and persisted history is removed
// with the workspace; the default workspace is refused (409); unknown IDs
// 404; the on-disk root directory is never touched.
func TestDeleteWorkspaceEndpoint(t *testing.T) {
	ts, registry, store := newWorkspaceScopedServer(t, fakes.NewFakeModel())
	ctx := context.Background()

	defWs, err := registry.RegisterDefault(ctx, canonicalTempDir(t))
	if err != nil {
		t.Fatalf("RegisterDefault: %v", err)
	}
	wsB, err := registry.Register(ctx, canonicalTempDir(t), "beta")
	if err != nil {
		t.Fatalf("Register beta: %v", err)
	}
	idB := wsB.WorkspaceID.String()

	// The list endpoint marks the default workspace (frontends hide its
	// delete affordance; the server refuses its deletion regardless).
	_, list := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/workspaces", "")
	for _, w := range list["workspaces"].([]any) {
		m := w.(map[string]any)
		isDefault, _ := m["is_default"].(bool)
		if want := m["id"] == defWs.WorkspaceID.String(); isDefault != want {
			t.Fatalf("is_default for %v = %v, want %v", m["id"], isDefault, want)
		}
	}

	// A historical (persisted but not live) session in workspace B: the
	// deletion must cascade to it.
	histID := domain.NewSessionID()
	if err := store.CreateSession(ctx, histID, wsB.WorkspaceID); err != nil {
		t.Fatalf("CreateSession historical: %v", err)
	}

	// A live session in B does not block deletion: it is shut down and
	// cascaded away with the workspace.
	_, bodyB := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", fmt.Sprintf(`{"workspace_id":%q}`, idB))
	sessB, _ := bodyB["session_id"].(string)

	// The default workspace is never deletable (legacy clients fall back to it).
	status, body := doJSON(t, ts.Client(), "DELETE", ts.URL+"/v1/workspaces/"+defWs.WorkspaceID.String(), "")
	if status != http.StatusConflict {
		t.Fatalf("delete default = (%d, %v), want 409", status, body)
	}
	if code, _ := body["error"].(map[string]any)["code"].(string); code != "workspace_in_use" {
		t.Fatalf("error code = %q, want workspace_in_use", code)
	}

	// Malformed / unknown IDs: 400 and 404 respectively.
	if status := doDelete(t, ts.Client(), ts.URL+"/v1/workspaces/bogus"); status != http.StatusBadRequest {
		t.Fatalf("delete malformed id = %d, want 400", status)
	}
	status, _ = doJSON(t, ts.Client(), "DELETE", ts.URL+"/v1/workspaces/ws_00000000000000000000000000000000", "")
	if status != http.StatusNotFound {
		t.Fatalf("delete unknown id = %d, want 404", status)
	}

	// Deletion succeeds without touching the live session first: the cascade
	// shuts it down (204, empty body).
	if status := doDelete(t, ts.Client(), ts.URL+"/v1/workspaces/"+idB); status != http.StatusNoContent {
		t.Fatalf("delete workspace = %d, want 204", status)
	}
	status, body = doJSON(t, ts.Client(), "GET", ts.URL+"/v1/workspaces/"+idB, "")
	if status != http.StatusNotFound {
		t.Fatalf("get after delete = (%d, %v), want 404", status, body)
	}
	// A second delete is not idempotent: not-found.
	status, _ = doJSON(t, ts.Client(), "DELETE", ts.URL+"/v1/workspaces/"+idB, "")
	if status != http.StatusNotFound {
		t.Fatalf("re-delete = %d, want 404", status)
	}

	// Both sessions of the workspace are gone — the historical one and the
	// previously live one: neither is listed nor resolvable in the store.
	_, allList := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions?workspace_id=all", "")
	for _, s := range allList["sessions"].([]any) {
		m := s.(map[string]any)
		if m["id"] == histID.String() || m["id"] == sessB {
			t.Fatalf("session %v must be cascaded away with its workspace", m["id"])
		}
	}
	if _, err := store.SessionWorkspace(ctx, histID); err == nil {
		t.Fatal("historical session must be cascaded away with its workspace")
	}
	liveID, err := domain.ParseSessionID(sessB)
	if err != nil {
		t.Fatalf("parse live session id: %v", err)
	}
	if _, err := store.SessionWorkspace(ctx, liveID); err == nil {
		t.Fatal("live session must be cascaded away with its workspace")
	}

	// The on-disk root directory is never touched by the deletion.
	if info, err := os.Stat(wsB.WorkspaceRoot); err != nil || !info.IsDir() {
		t.Fatalf("workspace root must survive deletion: %v", err)
	}
}

// canonicalTempDir returns t.TempDir() with symlinks resolved (the store
// persists canonical workspace roots).
func canonicalTempDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if resolved, err := filepath.EvalSymlinks(dir); err == nil {
		return resolved
	}
	return dir
}

// TestBrowseDirectories locks the directory-browse contract used by the
// workspace picker (docs/WORKSPACE_DESIGN.md §11.3): hidden dirs excluded,
// home-relative default, non-existent path rejected.
func TestBrowseDirectories(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no home dir")
	}

	// Default path resolves to $HOME.
	_, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/files/browse", "")
	if body["home"] != home || body["path"] != home {
		t.Fatalf("default browse = %v, want home %s", body, home)
	}

	// A temp dir (created inside $HOME to stay within the browse confinement)
	// with a visible and a hidden subdirectory.
	tmp, err := os.MkdirTemp(home, "loom-browse-test-*")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(tmp)
	if err := os.Mkdir(filepath.Join(tmp, "sub"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(tmp, ".secret"), 0o755); err != nil {
		t.Fatal(err)
	}
	_, b2 := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/files/browse?path="+tmp, "")
	sawSub := false
	for _, e := range b2["entries"].([]any) {
		name := e.(map[string]any)["name"].(string)
		if name == "sub" {
			sawSub = true
		}
		if name == ".secret" {
			t.Fatal("hidden directory must be excluded")
		}
	}
	if !sawSub {
		t.Fatalf("sub not listed in %v", b2["entries"])
	}

	// A symlink to a directory is listed and traversable, even when its
	// target sits outside $HOME (the link is a door the user built under
	// their home). Navigation must stay on the symlink view.
	link := filepath.Join(home, "loom-browse-link-*")
	home2, _ := os.MkdirTemp("", "loom-browse-out-*")
	defer os.RemoveAll(home2)
	if err := os.Mkdir(filepath.Join(home2, "inner"), 0o755); err != nil {
		t.Fatal(err)
	}
	realLink, err := os.MkdirTemp(home, "loom-browse-src-*")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(realLink)
	link = filepath.Join(realLink, "alias")
	if err := os.Symlink(home2, link); err != nil {
		t.Skipf("symlink: %v", err)
	}
	defer os.Remove(link)
	status, b3 := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/files/browse?path="+link, "")
	if status != http.StatusOK {
		t.Fatalf("symlinked dir browse = %d, want 200", status)
	}
	if b3["path"] != link {
		t.Fatalf("symlink browse path = %v, want the symlink form %s", b3["path"], link)
	}
	sawInner := false
	for _, e := range b3["entries"].([]any) {
		if e.(map[string]any)["name"].(string) == "inner" {
			sawInner = true
		}
	}
	if !sawInner {
		t.Fatalf("inner not listed through symlink in %v", b3["entries"])
	}
	// The parent link climbs the symlink form, not the resolved target.
	if b3["parent"] != realLink {
		t.Fatalf("symlink parent = %v, want %s", b3["parent"], realLink)
	}

	// Paths outside $HOME are rejected: the browser never lists beyond it
	// (REVIEW H11). /etc resolves (through a symlink on macOS) outside any
	// realistic home directory.
	if status, _ := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/files/browse?path=/etc", ""); status != http.StatusBadRequest {
		t.Fatalf("outside-home path = %d, want 400", status)
	}

	// Non-existent path → 400.
	if status, _ := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/files/browse?path=/no/such/dir/xyz", ""); status != http.StatusBadRequest {
		t.Fatalf("missing path = %d, want 400", status)
	}
}

// newWorkspaceScopedServer builds a server backed by a real WorkspaceRegistry
// (RegisterDefault yields a real default workspace ID), so the workspace_id
// default-scope behavior is testable end to end.
func newWorkspaceScopedServer(t *testing.T, model domain.Model) (*httptest.Server, *app.WorkspaceRegistry, *session.SQLiteStore) {
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
		Storage: config.ResolvedStorage{BaseDir: t.TempDir()},
	}
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), resolved.Limits.MaxArtifactBytes)
	if err != nil {
		t.Fatalf("open artifact store: %v", err)
	}
	proc := &app.ProcessRuntime{
		Current:    resolved.Default,
		Store:      store,
		Artifact:   artStore,
		Questioner: domain.AutonomousQuestioner{},
	}
	proc.SwapResolved(resolved)
	registry, err := app.NewWorkspaceRegistry(proc)
	if err != nil {
		t.Fatalf("NewWorkspaceRegistry: %v", err)
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.http.Handler)
	t.Cleanup(ts.Close)
	return ts, registry, store
}

func sessionIDs(body map[string]any) []string {
	var ids []string
	for _, s := range body["sessions"].([]any) {
		ids = append(ids, s.(map[string]any)["id"].(string))
	}
	return ids
}

// TestListSessionsDefaultScope locks the TUI-facing default (the reported
// bug): GET /v1/sessions with no workspace_id returns only the default
// workspace's sessions (the single-workspace picker view), while
// workspace_id=all spans every workspace (the tree view).
func TestListSessionsDefaultScope(t *testing.T) {
	ts, registry, _ := newWorkspaceScopedServer(t, fakes.NewFakeModel())
	ctx := context.Background()

	defWs, err := registry.RegisterDefault(ctx, canonicalTempDir(t))
	if err != nil {
		t.Fatalf("RegisterDefault: %v", err)
	}
	wsB, err := registry.Register(ctx, canonicalTempDir(t), "beta")
	if err != nil {
		t.Fatalf("Register beta: %v", err)
	}

	// One session in the default workspace (empty body), one in workspace B.
	_, bodyA := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", "")
	if bodyA["workspace_id"] != defWs.WorkspaceID.String() {
		t.Fatalf("default create workspace_id = %v, want %s", bodyA["workspace_id"], defWs.WorkspaceID)
	}
	sessA, _ := bodyA["session_id"].(string)
	_, bodyB := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions", fmt.Sprintf(`{"workspace_id":%q}`, wsB.WorkspaceID.String()))
	if bodyB["workspace_id"] != wsB.WorkspaceID.String() {
		t.Fatalf("B create workspace_id = %v", bodyB["workspace_id"])
	}

	// Default scope: only the default workspace's session.
	_, defList := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions", "")
	if ids := sessionIDs(defList); len(ids) != 1 || ids[0] != sessA {
		t.Fatalf("default-scope ids = %v, want [%s]", ids, sessA)
	}
	// all scope: both.
	_, allList := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions?workspace_id=all", "")
	if ids := sessionIDs(allList); len(ids) != 2 {
		t.Fatalf("all-scope ids = %v, want 2 sessions", ids)
	}
}

// --- feedback ---

// feedbackRecorder is a fake trace.Recorder: every run gets a deterministic
// trace id ("trace-<runID>") and ScoreTrace captures submissions.
type feedbackRecorder struct {
	mu      sync.Mutex
	scores  []capturedScore
	deliver bool
}

type capturedScore struct {
	traceID string
	name    string
	value   float64
	comment string
}

func (f *feedbackRecorder) StartRun(ctx context.Context, meta trace.RunMeta) (context.Context, trace.RunHandle) {
	return ctx, feedbackRunHandle{traceID: "trace-" + meta.RunID}
}

func (f *feedbackRecorder) ScoreTrace(_ context.Context, traceID, name string, value float64, comment string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	if !f.deliver {
		return false
	}
	f.scores = append(f.scores, capturedScore{traceID: traceID, name: name, value: value, comment: comment})
	return true
}

// setDeliver flips delivery mid-test (e.g. simulating a backend outage);
// it goes through the mutex because ScoreTrace runs on handler goroutines.
func (f *feedbackRecorder) setDeliver(v bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.deliver = v
}

func (f *feedbackRecorder) captured() []capturedScore {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]capturedScore(nil), f.scores...)
}

type feedbackRunHandle struct{ traceID string }

func (h feedbackRunHandle) RecordGeneration(context.Context, trace.GenerationRecord) {}
func (h feedbackRunHandle) RecordTool(context.Context, trace.ToolRecord)             {}
func (h feedbackRunHandle) RecordEvent(context.Context, string, map[string]string)   {}
func (h feedbackRunHandle) Score(context.Context, string, float64, string)           {}
func (h feedbackRunHandle) TraceID() string                                          { return h.traceID }
func (h feedbackRunHandle) End(trace.RunResult)                                      {}

// snapshotAssistantMeta extracts (run_id, trace_id) from the last assistant
// message in a snapshot body. The session state flips to idle before the
// final message projection lands, so poll briefly instead of reading once.
func snapshotAssistantMeta(t *testing.T, ts *httptest.Server, sessionID string) (string, string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		_, body := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/sessions/"+sessionID+"/snapshot", "")
		msgs, _ := body["messages"].([]any)
		for i := len(msgs) - 1; i >= 0; i-- {
			m, _ := msgs[i].(map[string]any)
			if m["role"] != "assistant" {
				continue
			}
			meta, _ := m["metadata"].(map[string]any)
			runID, _ := meta["run_id"].(string)
			traceID, _ := meta["trace_id"].(string)
			if runID != "" {
				return runID, traceID
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("no assistant message with run_id metadata in snapshot")
	return "", ""
}

func TestSubmitFeedbackEndpoint(t *testing.T) {
	rec := &feedbackRecorder{deliver: true}
	ts, _ := newTestServerWithRecorder(t, fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn}), rec)

	id := createTestSession(t, ts)
	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hello"}`)
	if status != http.StatusAccepted {
		t.Fatalf("submit prompt status = %d, want 202", status)
	}
	waitIdle(t, ts, id)

	runID, traceID := snapshotAssistantMeta(t, ts, id)
	if traceID != "trace-"+runID {
		t.Fatalf("stamped trace_id = %q, want trace-%s", traceID, runID)
	}

	// Happy path: a vote lands on the run's trace as a user_feedback score.
	status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback",
		fmt.Sprintf(`{"run_id":%q,"value":1,"comment":"nice"}`, runID))
	if status != http.StatusOK || body["recorded"] != true {
		t.Fatalf("feedback status = %d body = %v, want 200 recorded=true", status, body)
	}
	scores := rec.captured()
	if len(scores) != 1 {
		t.Fatalf("captured scores = %v, want exactly 1", scores)
	}
	got := scores[0]
	if got.traceID != traceID || got.name != app.FeedbackScoreName || got.value != 1 || got.comment != "nice" {
		t.Fatalf("captured score = %+v, want trace=%s name=user_feedback value=1", got, traceID)
	}

	// Re-vote (down) overwrites: another submission, same trace.
	status, _ = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback",
		fmt.Sprintf(`{"run_id":%q,"value":0}`, runID))
	if status != http.StatusOK {
		t.Fatalf("re-vote status = %d, want 200", status)
	}
	if scores = rec.captured(); len(scores) != 2 || scores[1].value != 0 {
		t.Fatalf("captured scores after re-vote = %v", scores)
	}

	// Unknown run → 404 feedback_target_unknown.
	status, body = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback",
		fmt.Sprintf(`{"run_id":%q,"value":1}`, domain.NewRunID().String()))
	if status != http.StatusNotFound || body["error"].(map[string]any)["code"] != "feedback_target_unknown" {
		t.Fatalf("unknown run status = %d body = %v, want 404 feedback_target_unknown", status, body)
	}

	// Bad inputs → 400.
	status, _ = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback", `{"run_id":"bogus","value":1}`)
	if status != http.StatusBadRequest {
		t.Fatalf("bad run_id status = %d, want 400", status)
	}
	status, _ = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback",
		fmt.Sprintf(`{"run_id":%q,"value":2}`, runID))
	if status != http.StatusBadRequest {
		t.Fatalf("bad value status = %d, want 400", status)
	}
}

// TestSubmitFeedbackTracingDisabled: the turn ran with a trace id stamped,
// but the score backend refuses delivery → 503 tracing_disabled.
func TestSubmitFeedbackTracingDisabled(t *testing.T) {
	rec := &feedbackRecorder{deliver: true}
	ts, _ := newTestServerWithRecorder(t, fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn}), rec)

	id := createTestSession(t, ts)
	if status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hello"}`); status != http.StatusAccepted {
		t.Fatalf("submit prompt status = %d, want 202", status)
	}
	waitIdle(t, ts, id)
	runID, _ := snapshotAssistantMeta(t, ts, id)

	rec.setDeliver(false)
	status, body := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/feedback",
		fmt.Sprintf(`{"run_id":%q,"value":1}`, runID))
	if status != http.StatusServiceUnavailable || body["error"].(map[string]any)["code"] != "tracing_disabled" {
		t.Fatalf("status = %d body = %v, want 503 tracing_disabled", status, body)
	}
}
