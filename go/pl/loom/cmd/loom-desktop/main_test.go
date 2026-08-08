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

package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func TestResolveListenPort(t *testing.T) {
	// Concrete ports pass through untouched.
	if got, err := resolveListenPort("127.0.0.1:7680"); err != nil || got != "127.0.0.1:7680" {
		t.Fatalf("concrete port = %q, %v", got, err)
	}
	// :0 resolves to a concrete, immediately re-bindable port.
	got, err := resolveListenPort("127.0.0.1:0")
	if err != nil {
		t.Fatalf("resolve :0: %v", err)
	}
	if strings.HasSuffix(got, ":0") {
		t.Fatalf("resolve :0 = %q, port still zero", got)
	}
	if _, err := resolveListenPort("not-an-addr"); err == nil {
		t.Fatal("malformed addr: want error")
	}
}

func TestDerivePublicBase(t *testing.T) {
	cases := []struct {
		name      string
		advertise string
		bound     string
		want      string
	}{
		{"explicit advertise wins", "http://example.com:9000", "127.0.0.1:7680", "http://example.com:9000"},
		{"loopback", "", "127.0.0.1:7680", "http://127.0.0.1:7680"},
		{"localhost name", "", "localhost:7680", "http://127.0.0.1:7680"},
		{"specific LAN addr", "", "192.168.1.5:7680", "http://192.168.1.5:7680"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := derivePublicBase(tc.advertise, tc.bound)
			if err != nil {
				t.Fatalf("derivePublicBase: %v", err)
			}
			if got != tc.want {
				t.Fatalf("derivePublicBase(%q, %q) = %q, want %q", tc.advertise, tc.bound, got, tc.want)
			}
		})
	}
	// Unspecified bind: derived from the outbound interface (or an error
	// when no route exists); never empty-without-error on a routed host.
	got, err := derivePublicBase("", "0.0.0.0:7680")
	if err == nil && !strings.HasPrefix(got, "http://") {
		t.Fatalf("unspecified bind base = %q, want http:// URL", got)
	}
	if _, err := derivePublicBase("", "bad"); err == nil {
		t.Fatal("malformed bound addr: want error")
	}
}

// TestBootstrapHandler locks the desktop start page contract
// (docs/DESKTOP_DESIGN.md §2.3): a meta-refresh redirect to the loopback UI
// carrying the in-process token in the URL fragment.
func TestBootstrapHandler(t *testing.T) {
	target := "http://127.0.0.1:54321/#token=deadbeef"
	handler := bootstrapHandler(target)

	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	body := rec.Body.String()
	if !strings.Contains(body, `<meta http-equiv="refresh" content="0;url=`+target+`">`) {
		t.Fatalf("bootstrap page missing meta refresh: %s", body)
	}
	if rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("Cache-Control = %q, want no-store", rec.Header().Get("Cache-Control"))
	}

	// Non-GET is meaningless for the bootstrap page.
	rec = httptest.NewRecorder()
	handler.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST status = %d, want 405", rec.Code)
	}
}

func TestAppleScriptString(t *testing.T) {
	if got := appleScriptString(`plain`); got != `"plain"` {
		t.Fatalf("plain = %s", got)
	}
	got := appleScriptString(`a"b\\c`)
	if got != `"a\"b\\\\c"` {
		t.Fatalf("escaped = %s", got)
	}
}

func TestTruncateRunes(t *testing.T) {
	if got := truncateRunes("short", 10); got != "short" {
		t.Fatalf("short = %q", got)
	}
	if got := truncateRunes(strings.Repeat("x", 200), 120); len([]rune(got)) != 120 {
		t.Fatalf("truncated rune len = %d, want 120", len([]rune(got)))
	}
	// Multibyte content must be truncated by runes, not bytes.
	got := truncateRunes(strings.Repeat("中", 50), 10)
	if len([]rune(got)) != 10 || !strings.HasSuffix(got, "…") {
		t.Fatalf("multibyte = %q", got)
	}
}

func TestLoadWindowState(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "desktop-window.json")

	if _, ok := loadWindowState(path); ok {
		t.Fatal("missing file: want ok=false")
	}
	write := func(s string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(s), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	write(`{"width":1400,"height":900,"x":10,"y":20}`)
	ws, ok := loadWindowState(path)
	if !ok || ws.Width != 1400 || ws.Height != 900 || ws.X != 10 || ws.Y != 20 {
		t.Fatalf("valid = %+v, ok=%v", ws, ok)
	}
	write(`{"width":10,"height":10}`)
	if _, ok := loadWindowState(path); ok {
		t.Fatal("below minimum: want ok=false")
	}
	write(`not json`)
	if _, ok := loadWindowState(path); ok {
		t.Fatal("corrupt: want ok=false")
	}
}

func TestWriteWindowStateRoundtrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "desktop-window.json")
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	want := windowState{Width: 1440, Height: 900, X: 30, Y: 40}
	writeWindowState(path, want, logger)
	got, ok := loadWindowState(path)
	if !ok || got != want {
		t.Fatalf("roundtrip = %+v, ok=%v, want %+v", got, ok, want)
	}
}

// captureNotifications swaps notifyFunc for a recorder and restores it.
func captureNotifications(t *testing.T) *struct {
	mu    sync.Mutex
	calls []string
} {
	t.Helper()
	rec := &struct {
		mu    sync.Mutex
		calls []string
	}{}
	orig := notifyFunc
	notifyFunc = func(title, body string) {
		rec.mu.Lock()
		defer rec.mu.Unlock()
		rec.calls = append(rec.calls, title+"|"+body)
	}
	t.Cleanup(func() { notifyFunc = orig })
	return rec
}

func TestNotifyForEvent(t *testing.T) {
	rec := captureNotifications(t)
	payload := func(v any) json.RawMessage {
		t.Helper()
		raw, err := json.Marshal(v)
		if err != nil {
			t.Fatal(err)
		}
		return raw
	}

	// Approval carries tool + description.
	if err := notifyForEvent(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindApprovalRequested,
		Payload: payload(runtimeevent.ApprovalRequestedPayload{ToolName: "shell", Description: "run tests"}),
	}); err != nil {
		t.Fatal(err)
	}
	// Turn failure surfaces the error; clean finish is a plain banner.
	_ = notifyForEvent(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindTurnFinished,
		Payload: payload(runtimeevent.TurnFinishedPayload{Error: "boom"}),
	})
	_ = notifyForEvent(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindTurnFinished,
		Payload: payload(runtimeevent.TurnFinishedPayload{}),
	})
	_ = notifyForEvent(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindQuestionAsked,
		Payload: payload(runtimeevent.QuestionAskedPayload{Text: "which option?"}),
	})
	// Noise kinds never notify.
	_ = notifyForEvent(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindModelTextDelta,
		Payload: payload(runtimeevent.ModelTextDeltaPayload{Delta: "hi"}),
	})

	rec.mu.Lock()
	defer rec.mu.Unlock()
	want := []string{
		"Approval needed|shell: run tests",
		"Turn failed|boom",
		"Turn finished|Loom finished the current turn.",
		"Loom has a question|which option?",
	}
	if len(rec.calls) != len(want) {
		t.Fatalf("calls = %v, want %v", rec.calls, want)
	}
	for i := range want {
		if rec.calls[i] != want[i] {
			t.Fatalf("call[%d] = %q, want %q", i, rec.calls[i], want[i])
		}
	}
}

// TestGenerateToken: 32-hex shape, unique across calls.
func TestGenerateToken(t *testing.T) {
	a, err := generateToken()
	if err != nil {
		t.Fatalf("generateToken: %v", err)
	}
	b, _ := generateToken()
	if len(a) != 64 {
		t.Fatalf("token len = %d, want 64 hex chars (32 bytes)", len(a))
	}
	if a == b {
		t.Fatal("two tokens identical")
	}
}
