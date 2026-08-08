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
// Created: 2026/08/08

package server

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// TestMiddlewarePanicRecovery locks the panic-containment contract
// (REVIEW M24): a handler panic is logged and answered with a 500 in the
// unified error model instead of killing the connection, and the panic
// value never leaks into the response.
func TestMiddlewarePanicRecovery(t *testing.T) {
	var logBuf bytes.Buffer
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{
		Token:   testToken,
		Version: "test",
		Service: svc,
		Logger:  slog.New(slog.NewTextHandler(&logBuf, nil)),
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	handler := srv.withMiddleware(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		panic("boom")
	}))

	rec := httptest.NewRecorder()
	// A static path skips the auth gate, so the request reaches the
	// panicking handler without a token.
	req := httptest.NewRequest("GET", "/", nil)
	handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", rec.Code)
	}
	var body map[string]any
	if err := json.NewDecoder(rec.Body).Decode(&body); err != nil {
		t.Fatalf("decode error body: %v", err)
	}
	errObj, _ := body["error"].(map[string]any)
	if errObj["code"] != "internal" {
		t.Fatalf("error body = %v, want code internal", body)
	}
	if msg, _ := errObj["message"].(string); strings.Contains(msg, "boom") {
		t.Fatalf("panic value leaked into response: %v", body)
	}
	if log := logBuf.String(); !strings.Contains(log, "http handler panic") || !strings.Contains(log, "boom") {
		t.Fatalf("panic not logged: %q", log)
	}
}

// TestCORSPreflight locks the preflight contract (REVIEW M24): with
// --allow-origin configured, a bare OPTIONS from the whitelisted origin is
// answered 204 with allow headers before the auth gate; OPTIONS without a
// whitelisted origin keeps the previous behavior (401 on /v1 routes).
func TestCORSPreflight(t *testing.T) {
	const origin = "https://ui.example.com"
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{
		Token:       testToken,
		Version:     "test",
		Service:     svc,
		AllowOrigin: origin,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)

	options := func(t *testing.T, originHeader string) *http.Response {
		t.Helper()
		req, err := http.NewRequest(http.MethodOptions, ts.URL+"/v1/sessions", nil)
		if err != nil {
			t.Fatalf("NewRequest: %v", err)
		}
		if originHeader != "" {
			req.Header.Set("Origin", originHeader)
		}
		resp, err := ts.Client().Do(req)
		if err != nil {
			t.Fatalf("OPTIONS: %v", err)
		}
		return resp
	}

	// Whitelisted origin: preflight short-circuits before auth (no token).
	resp := options(t, origin)
	if resp.StatusCode != http.StatusNoContent {
		t.Fatalf("preflight status = %d, want 204", resp.StatusCode)
	}
	if got := resp.Header.Get("Access-Control-Allow-Origin"); got != origin {
		t.Fatalf("ACAO = %q, want %q", got, origin)
	}
	methods := resp.Header.Get("Access-Control-Allow-Methods")
	for _, m := range []string{"GET", "POST", "DELETE", "PATCH"} {
		if !strings.Contains(methods, m) {
			t.Fatalf("Allow-Methods = %q, missing %s", methods, m)
		}
	}
	headers := resp.Header.Get("Access-Control-Allow-Headers")
	if !strings.Contains(headers, "Authorization") || !strings.Contains(headers, "Content-Type") {
		t.Fatalf("Allow-Headers = %q, want Authorization and Content-Type", headers)
	}
	resp.Body.Close()

	// No Origin: OPTIONS falls through to the auth gate (unchanged).
	resp = options(t, "")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("OPTIONS without Origin status = %d, want 401", resp.StatusCode)
	}
	resp.Body.Close()

	// A different origin gets no preflight grant either.
	resp = options(t, "https://evil.example.com")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("OPTIONS with foreign Origin status = %d, want 401", resp.StatusCode)
	}
	if got := resp.Header.Get("Access-Control-Allow-Origin"); got != "" {
		t.Fatalf("foreign origin got ACAO = %q, want none", got)
	}
	resp.Body.Close()

	// The actual credentialed request from the whitelisted origin works
	// and carries the ACAO header.
	req, err := http.NewRequest("GET", ts.URL+"/v1/meta/version", nil)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.Header.Set("Origin", origin)
	authed(t, req)
	resp, err = ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET version: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET version status = %d, want 200", resp.StatusCode)
	}
	if got := resp.Header.Get("Access-Control-Allow-Origin"); got != origin {
		t.Fatalf("GET ACAO = %q, want %q", got, origin)
	}
}
