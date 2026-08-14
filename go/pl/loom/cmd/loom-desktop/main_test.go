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
	"context"
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

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestLastActiveWorkspaceRootNoHistory: with no session store there is
// nothing to derive from — the caller must get "" so it can ask once.
func TestLastActiveWorkspaceRootNoHistory(t *testing.T) {
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: t.TempDir()}}
	if got := lastActiveWorkspaceRoot(context.Background(), resolved); got != "" {
		t.Fatalf("lastActiveWorkspaceRoot without store = %q, want empty", got)
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

// TestLoadConfigFirstRunBootstrapsDefaultPath: a fresh install has no
// config at all; the desktop must write the starter template at the
// default path and keep booting (the settings UI collects the API key),
// instead of refusing to start.
func TestLoadConfigFirstRunBootstrapsDefaultPath(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv(config.HomeEnv, "")

	resolved, err := loadConfig()
	if err != nil {
		t.Fatalf("desktop loadConfig() first run: %v", err)
	}
	if len(resolved.Providers) == 0 {
		t.Fatal("template providers = 0, want the deepseek starter provider")
	}
	def, derr := config.DefaultHomeDir()
	if derr != nil {
		t.Fatal(derr)
	}
	if _, serr := os.Stat(filepath.Join(def, config.FileName)); serr != nil {
		t.Fatalf("starter config not created: %v", serr)
	}
}

// TestLoadConfigExplicitHomeStaysHardError: LOOM_HOME names a directory
// whose config.yaml should exist; a missing config there is a user error
// and must not be papered over with an auto-created template.
func TestLoadConfigExplicitHomeStaysHardError(t *testing.T) {
	explicit := filepath.Join(t.TempDir(), "custom")
	t.Setenv(config.HomeEnv, explicit)

	_, err := loadConfig()
	if err == nil || !strings.Contains(err.Error(), "not found") {
		t.Fatalf("desktop loadConfig() error = %v, want not-found", err)
	}
	if _, serr := os.Stat(filepath.Join(explicit, "config.yaml")); !os.IsNotExist(serr) {
		t.Fatalf("explicit home's config was auto-created: %v", serr)
	}
}
