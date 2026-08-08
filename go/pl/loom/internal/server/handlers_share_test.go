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
// Created: 2026/08/07

package server

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// getPublic performs an unauthenticated GET and returns status + body.
func getPublic(t *testing.T, url string) (int, []byte) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("GET %s: read body: %v", url, err)
	}
	return resp.StatusCode, body
}

func TestShareLinkLifecycle(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "hello world", StopReason: domain.StopEndTurn})
	ts, _ := newTestServer(t, model)
	id := createTestSession(t, ts)

	// One exchange so the shared view has content and a title.
	status, _ := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/prompts", `{"prompt":"hi"}`)
	if status != http.StatusAccepted {
		t.Fatalf("submit status = %d, want 202", status)
	}
	waitIdle(t, ts, id)

	// Creating a share is an owner operation: no bearer token → 401.
	req, _ := http.NewRequest("POST", ts.URL+"/v1/sessions/"+id+"/share", nil)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("POST share (anon): %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("POST share without token status = %d, want 401", resp.StatusCode)
	}

	// Create the share; repeated creation is idempotent (same token).
	status, share := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	if status != http.StatusOK {
		t.Fatalf("POST share status = %d, want 200 (%v)", status, share)
	}
	token, _ := share["token"].(string)
	if !regexp.MustCompile(`^[0-9a-f]{32}$`).MatchString(token) {
		t.Fatalf("share token = %q, want 32 lowercase hex chars", token)
	}
	if path, _ := share["path"].(string); path != "/share/"+token {
		t.Fatalf("share path = %v, want /share/<token>", share["path"])
	}
	// No PublicBaseURL configured: the absolute url field must be absent so
	// clients fall back to their own origin (docs/DESKTOP_DESIGN.md §5.2).
	if _, ok := share["url"]; ok {
		t.Fatalf("share response unexpectedly carries url: %v", share["url"])
	}
	_, again := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	if again["token"] != token {
		t.Fatalf("repeat share token = %v, want idempotent %q", again["token"], token)
	}

	// The public view is reachable without any credentials.
	status, body := getPublic(t, ts.URL+"/v1/shared/"+token)
	if status != http.StatusOK {
		t.Fatalf("GET shared view status = %d, want 200 (%s)", status, body)
	}
	var view map[string]any
	if err := json.Unmarshal(body, &view); err != nil {
		t.Fatalf("decode shared view: %v", err)
	}
	if view["session_id"] != id {
		t.Fatalf("shared view session_id = %v, want %s", view["session_id"], id)
	}
	if title, _ := view["title"].(string); title != "hi" {
		t.Fatalf("shared view title = %q, want %q", title, "hi")
	}
	if messages, _ := view["messages"].([]any); len(messages) < 2 {
		t.Fatalf("shared view messages = %d, want the user+assistant pair", len(messages))
	}

	// The share page itself is a public static asset.
	status, body = getPublic(t, ts.URL+"/share/"+token)
	if status != http.StatusOK || !strings.Contains(string(body), "share.js") {
		t.Fatalf("GET share page = (%d, %.80s), want 200 HTML", status, body)
	}

	// Shared artifacts are gated by session references: an unreferenced
	// (but well-formed) artifact id must not be readable through the link.
	artID := "art_" + strings.Repeat("0", 64)
	status, _ = getPublic(t, ts.URL+"/v1/shared/"+token+"/artifacts/"+artID+"?size=1")
	if status != http.StatusNotFound {
		t.Fatalf("GET unreferenced shared artifact status = %d, want 404", status)
	}

	// Malformed and unknown tokens both read as 404 (no oracle).
	status, _ = getPublic(t, ts.URL+"/v1/shared/not-a-token")
	if status != http.StatusNotFound {
		t.Fatalf("GET malformed share token status = %d, want 404", status)
	}
	status, _ = getPublic(t, ts.URL+"/v1/shared/"+strings.Repeat("1", 32))
	if status != http.StatusNotFound {
		t.Fatalf("GET unknown share token status = %d, want 404", status)
	}

	// Revoke: the link dies immediately, and a re-share mints a new token.
	req, _ = http.NewRequest("DELETE", ts.URL+"/v1/sessions/"+id+"/share", nil)
	authed(t, req)
	resp, err = ts.Client().Do(req)
	if err != nil {
		t.Fatalf("DELETE share: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Fatalf("DELETE share status = %d, want 204", resp.StatusCode)
	}
	status, _ = getPublic(t, ts.URL+"/v1/shared/"+token)
	if status != http.StatusNotFound {
		t.Fatalf("GET revoked share status = %d, want 404", status)
	}
	_, reshared := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	if reshared["token"] == token {
		t.Fatalf("re-share after revoke returned the revoked token %q", token)
	}
}

// TestResumeSessionUnknownContentLength: the create-vs-resume decision must
// not depend on ContentLength — chunked clients (and in-process mounts like
// the Wails AssetServer) send ContentLength=-1 with a perfectly good body.
// Regression guard for docs/DESKTOP_DESIGN.md §9 R-B2.
func TestResumeSessionUnknownContentLength(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)

	req, err := http.NewRequest("POST", ts.URL+"/v1/sessions", strings.NewReader(`{"resume":"`+id+`"}`))
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.ContentLength = -1 // forces chunked transfer encoding
	authed(t, req)
	req.Header.Set("Content-Type", "application/json")
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("POST resume (chunked): %v", err)
	}
	defer resp.Body.Close()
	var body map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body["session_id"] != id {
		t.Fatalf("chunked resume returned session_id=%v, want the resumed %s (a new session means the body was ignored)", body["session_id"], id)
	}
}

// TestShareAbsoluteURL locks the PublicBaseURL contract
// (docs/DESKTOP_DESIGN.md §5.2): when an external base URL is configured,
// share responses carry an absolute "url" so clients on a non-HTTP origin
// (the desktop webview) can hand out working links.
func TestShareAbsoluteURL(t *testing.T) {
	svc := newTestServiceWithRecorder(t, fakes.NewFakeModel(), nil)
	srv, err := New(Config{
		Token:   testToken,
		Version: "test",
		Service: svc,
		// Trailing slash: New normalizes it away.
		PublicBaseURL: "http://192.168.1.5:7680/",
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.http.Handler)
	t.Cleanup(ts.Close)
	id := createTestSession(t, ts)

	status, share := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	if status != http.StatusOK {
		t.Fatalf("POST share status = %d, want 200 (%v)", status, share)
	}
	token, _ := share["token"].(string)
	want := "http://192.168.1.5:7680/share/" + token
	if share["url"] != want {
		t.Fatalf("share url = %v, want %q", share["url"], want)
	}
	if share["path"] != "/share/"+token {
		t.Fatalf("share path = %v, want /share/<token>", share["path"])
	}
}

// TestPublicBaseURLValidation: only http(s) base URLs are accepted —
// anything else would mint broken or surprising share links.
func TestPublicBaseURLValidation(t *testing.T) {
	svc := newTestServiceWithRecorder(t, fakes.NewFakeModel(), nil)
	for _, base := range []string{"ftp://x", "192.168.1.5:7680", "//x"} {
		if _, err := New(Config{
			Token: testToken, Version: "test", Service: svc, PublicBaseURL: base,
		}); err == nil {
			t.Fatalf("New(PublicBaseURL=%q): want validation error", base)
		}
	}
}

// TestSharedArtifactParamValidation locks the shared-helper contract
// (REVIEW R14): the share-link artifact endpoint applies the same id/size
// validation as the owner endpoint, before any store lookup.
func TestSharedArtifactParamValidation(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)
	_, share := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	token, _ := share["token"].(string)

	artID := "art_" + strings.Repeat("0", 64)
	for name, url := range map[string]string{
		"missing size":     ts.URL + "/v1/shared/" + token + "/artifacts/" + artID,
		"bad id":           ts.URL + "/v1/shared/" + token + "/artifacts/not-a-valid-id?size=1",
		"negative size":    ts.URL + "/v1/shared/" + token + "/artifacts/" + artID + "?size=-1",
		"non-numeric size": ts.URL + "/v1/shared/" + token + "/artifacts/" + artID + "?size=abc",
	} {
		status, _ := getPublic(t, url)
		if status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, status)
		}
	}
}

// TestShareViewSurvivesSessionDelete covers the FK cascade: deleting the
// session removes its share, so old links 404 instead of leaking data.
func TestShareViewSurvivesSessionDelete(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	id := createTestSession(t, ts)
	_, share := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/sessions/"+id+"/share", "")
	token, _ := share["token"].(string)

	req, _ := http.NewRequest("DELETE", ts.URL+"/v1/sessions/"+id, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("DELETE session: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Fatalf("DELETE session status = %d, want 204", resp.StatusCode)
	}
	status, _ := getPublic(t, ts.URL+"/v1/shared/"+token)
	if status != http.StatusNotFound {
		t.Fatalf("GET share of deleted session status = %d, want 404", status)
	}
}
