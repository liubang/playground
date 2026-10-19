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

package app

import (
	"context"
	"io"
	"log/slog"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// stubStreamModel plays a fixed event sequence — fakes.FakeModel only
// produces successful streams, so mid-stream failures need a custom stub.
type stubStreamModel struct {
	events []domain.ModelEvent
}

func (m stubStreamModel) Stream(context.Context, domain.ModelRequest) (domain.ModelStream, error) {
	return &stubStream{events: m.events}, nil
}

type stubStream struct {
	events []domain.ModelEvent
	next   int
}

func (s *stubStream) Recv() (domain.ModelEvent, error) {
	if s.next >= len(s.events) {
		return domain.ModelEvent{}, io.EOF
	}
	e := s.events[s.next]
	s.next++
	return e, nil
}

func (s *stubStream) Close() error { return nil }

func TestCleanGeneratedTitle(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		want string
	}{
		{"plain", "Fix the flaky SST test", "Fix the flaky SST test"},
		{"quoted", `"Fix the flaky SST test"`, "Fix the flaky SST test"},
		{"backticked", "`Fix the flaky SST test`", "Fix the flaky SST test"},
		{"first line only", "Fix the flaky SST test\nHere is why...", "Fix the flaky SST test"},
		{"trailing punctuation", "修复偶发测试失败。", "修复偶发测试失败"},
		{"whitespace collapsed", "  fix   the\n  flaky test  ", "fix the"},
		{"smart quotes", "“Fix the flaky SST test”", "Fix the flaky SST test"},
		{"markdown bold", "**Fix the flaky SST test**", "Fix the flaky SST test"},
		{"decorations only", `""`, ""},
		{"empty", "", ""},
		{"long title truncated", strings.Repeat("a", 60), strings.Repeat("a", 49) + "…"},
	}
	for _, tc := range cases {
		if got := cleanGeneratedTitle(tc.raw); got != tc.want {
			t.Errorf("%s: cleanGeneratedTitle(%q) = %q, want %q", tc.name, tc.raw, got, tc.want)
		}
	}
}

func TestRunTitleCall(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text: "\"Fix the flaky SST test\"\n", StopReason: domain.StopEndTurn,
	})
	title, err := runTitleCall(context.Background(), model, "model-a", "fix the flaky test in sstv2")
	if err != nil {
		t.Fatalf("runTitleCall: %v", err)
	}
	if title != "Fix the flaky SST test" {
		t.Fatalf("title = %q", title)
	}
	// The sidecar request is minimal: no tools, capped output, the raw
	// prompt as the user message.
	if calls := model.Calls(); len(calls) != 1 {
		t.Fatalf("model calls = %d, want 1", len(calls))
	} else {
		req := calls[0]
		if len(req.Tools) != 0 || req.MaxTokens != titleGenMaxTokens {
			t.Fatalf("request = %+v, want tool-less and capped", req)
		}
		if got := req.Messages[1].TextParts()[0]; got != "fix the flaky test in sstv2" {
			t.Fatalf("user message = %q, want the raw prompt", got)
		}
	}

	// An empty model answer is an error — the caller keeps the fallback.
	empty := fakes.NewFakeModel(fakes.ScriptEntry{Text: " \n", StopReason: domain.StopEndTurn})
	if _, err := runTitleCall(context.Background(), empty, "model-a", "prompt"); err == nil {
		t.Fatal("runTitleCall with empty output must fail")
	}

	// A mid-stream failure arrives as a stream_error EVENT (providers
	// report it in-band; Recv still ends on io.EOF): the partial text
	// collected before the break must not become a title.
	broken := stubStreamModel{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: "partial"},
		{Kind: domain.ModelEventStreamError, Error: "connection reset", Retryable: true},
	}}
	if _, err := runTitleCall(context.Background(), broken, "model-a", "prompt"); err == nil {
		t.Fatal("runTitleCall must fail on a stream_error event")
	}
}

// newTitleTestRuntime assembles the minimal process runtime the title
// sidecar needs: a real SQLite store and a one-provider resolved config
// backed by the fake model.
func newTitleTestRuntime(t *testing.T, model domain.Model) (*ProcessRuntime, *session.SQLiteStore) {
	t.Helper()
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })
	ref := config.ProviderModelRef{Provider: "fake", Model: "model-a"}
	proc := &ProcessRuntime{Store: store, Logger: slog.Default(), Current: ref}
	proc.resolved.Store(&config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:   "fake",
			Model:  model,
			Models: []config.Model{{Name: "model-a"}},
		}},
		Default: ref,
	})
	proc.TitleSidecar = NewTitleSidecar(store, proc.Resolved, nil)
	return proc, store
}

func awaitTitle(t *testing.T, store *session.SQLiteStore, sessionID domain.SessionID, want string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		title, err := store.SessionTitle(context.Background(), sessionID)
		if err == nil && title == want {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("title = %q, %v; want %q persisted", title, err, want)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestTitleSidecarGenerate(t *testing.T) {
	ctx := context.Background()
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text: "Fix the flaky SST test", StopReason: domain.StopEndTurn,
	})
	proc, store := newTitleTestRuntime(t, model)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	ref := config.ProviderModelRef{Provider: "fake", Model: "model-a"}

	titled := make(chan string, 1)
	proc.TitleSidecar.Generate(sessionID, ref, "fix the flaky test in sstv2", func(title string) { titled <- title })
	awaitTitle(t, store, sessionID, "Fix the flaky SST test")
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls = %d, want exactly one sidecar call", calls)
	}
	// The completion callback carries the persisted title (it drives the
	// session.titled event).
	select {
	case title := <-titled:
		if title != "Fix the flaky SST test" {
			t.Fatalf("onDone = %q", title)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("onDone was not called")
	}

	// A second run finds the persisted title, skips the model call and
	// still reports completion ("" = no new title written).
	skipped := make(chan string, 1)
	proc.TitleSidecar.Generate(sessionID, ref, "fix the flaky test in sstv2", func(title string) { skipped <- title })
	select {
	case title := <-skipped:
		if title != "" {
			t.Fatalf("onDone for skipped run = %q, want empty", title)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("onDone was not called for the skipped run")
	}
	// A nil sidecar and a blank prompt are no-ops that still invoke onDone
	// — callers rely on it to clear their title-pending marker.
	var nilSidecar *TitleSidecar
	noops := 0
	nilSidecar.Generate(sessionID, ref, "prompt", func(string) { noops++ })
	proc.TitleSidecar.Generate(sessionID, ref, "   ", func(string) { noops++ })
	if noops != 2 {
		t.Fatalf("onDone invocations on no-op paths = %d, want 2", noops)
	}
	// The skipped paths return before spawning work or right after a cheap
	// title read; give any stray goroutine time to surface.
	time.Sleep(200 * time.Millisecond)
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls after skips = %d, want still 1", calls)
	}
}

// TestTitleSidecarStreamFailure locks that a mid-stream failure neither
// persists the partial text (write-once would make it permanent) nor
// leaves the caller's title-pending marker stuck.
func TestTitleSidecarStreamFailure(t *testing.T) {
	ctx := context.Background()
	broken := stubStreamModel{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: "partial title"},
		{Kind: domain.ModelEventStreamError, Error: "connection reset"},
	}}
	proc, store := newTitleTestRuntime(t, broken)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	done := make(chan string, 1)
	proc.TitleSidecar.Generate(sessionID, config.ProviderModelRef{Provider: "fake", Model: "model-a"}, "prompt", func(title string) { done <- title })
	select {
	case title := <-done:
		if title != "" {
			t.Fatalf("onDone = %q, want empty on stream failure", title)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("onDone was not called after the stream failure")
	}
	if title, err := store.SessionTitle(ctx, sessionID); err != nil || title != "" {
		t.Fatalf("title = %q, %v; partial text must not persist", title, err)
	}
}

// flakyTitleModel fails the first failTimes calls with a retryable
// rate-limited stream error, then plays a success — the
// rate-limited-then-recovers gateway shape.
type flakyTitleModel struct {
	failTimes int
	calls     int
}

func (m *flakyTitleModel) Stream(context.Context, domain.ModelRequest) (domain.ModelStream, error) {
	m.calls++
	if m.calls <= m.failTimes {
		return &stubStream{events: []domain.ModelEvent{
			{Kind: domain.ModelEventStreamError, Error: "rate limited", Retryable: true, ErrorCode: domain.ErrRateLimited},
			{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopProviderError},
		}}, nil
	}
	return &stubStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextDelta, TextDelta: "Recovered Title"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}, nil
}

// A transient (rate-limited) failure must not drop the title: the sidecar
// waits out the window and retries within its bounded budget.
func TestTitleSidecarRetriesTransientFailure(t *testing.T) {
	ctx := context.Background()
	model := &flakyTitleModel{failTimes: 2}
	proc, store := newTitleTestRuntime(t, model)
	proc.TitleSidecar.retryWait = time.Millisecond
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	done := make(chan string, 1)
	proc.TitleSidecar.Generate(sessionID, config.ProviderModelRef{Provider: "fake", Model: "model-a"}, "prompt", func(title string) { done <- title })
	awaitTitle(t, store, sessionID, "Recovered Title")
	if model.calls != 3 {
		t.Fatalf("model calls = %d, want 3 (2 transient failures + success)", model.calls)
	}
	select {
	case title := <-done:
		if title != "Recovered Title" {
			t.Fatalf("onDone = %q", title)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("onDone was not called")
	}
}

// A persistently failing provider exhausts the bounded retry budget and
// falls back to the truncated prompt — the failure stays silent.
func TestTitleSidecarExhaustsTransientRetries(t *testing.T) {
	ctx := context.Background()
	model := &flakyTitleModel{failTimes: 10} // never recovers
	proc, store := newTitleTestRuntime(t, model)
	proc.TitleSidecar.retryWait = time.Millisecond
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	done := make(chan string, 1)
	proc.TitleSidecar.Generate(sessionID, config.ProviderModelRef{Provider: "fake", Model: "model-a"}, "prompt", func(title string) { done <- title })
	select {
	case title := <-done:
		if title != "" {
			t.Fatalf("onDone = %q, want empty after exhausting retries", title)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("onDone was not called after exhausting retries")
	}
	if model.calls != titleGenAttempts {
		t.Fatalf("model calls = %d, want %d (bounded retries)", model.calls, titleGenAttempts)
	}
	if title, err := store.SessionTitle(ctx, sessionID); err != nil || title != "" {
		t.Fatalf("title = %q, %v; a failed sidecar must not persist anything", title, err)
	}
}

func TestMaybeGenerateSessionTitleGate(t *testing.T) {
	ctx := context.Background()
	// Three script entries: the fired session, the one-shot session and
	// the post-switch session each consume one sidecar call.
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "Generated Title", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "Generated Title", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "Generated Title", StopReason: domain.StopEndTurn},
	)
	proc, store := newTitleTestRuntime(t, model)
	newController := func(sessionID domain.SessionID, delegated bool) *Controller {
		return &Controller{
			bootstrap: &Bootstrap{ProcessRuntime: proc},
			logger:    slog.Default(),
			sessionID: sessionID,
			delegated: delegated,
		}
	}

	// Non-first turns and delegated sessions never reach the sidecar.
	untouched := domain.NewSessionID()
	if err := store.CreateSession(ctx, untouched, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	newController(untouched, false).maybeGenerateSessionTitle(2, "follow-up prompt", false)
	newController(untouched, true).maybeGenerateSessionTitle(1, "sub-agent task", false)
	time.Sleep(200 * time.Millisecond)
	if calls := len(model.Calls()); calls != 0 {
		t.Fatalf("model calls = %d, want 0 (gate must hold)", calls)
	}

	// The first turn of a regular session fires the sidecar.
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	newController(sessionID, false).maybeGenerateSessionTitle(1, "fix the flaky test", false)
	awaitTitle(t, store, sessionID, "Generated Title")

	// One-shot per controller: the turn-start attempt and the turn-end
	// recovery trigger must not duplicate a sidecar call.
	oneShotID := domain.NewSessionID()
	if err := store.CreateSession(ctx, oneShotID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	ctl := newController(oneShotID, false)
	ctl.maybeGenerateSessionTitle(1, "early trigger", false)
	ctl.maybeGenerateSessionTitle(1, "recovery trigger", false)
	awaitTitle(t, store, oneShotID, "Generated Title")
	if calls := len(model.Calls()); calls != 2 {
		t.Fatalf("model calls = %d, want 2 (one per session, one-shot held)", calls)
	}

	// A resumed conversation (transcript re-seeded) must NOT be titled
	// after its NEW first submission: the write-once title would describe
	// "继续" instead of the conversation. It keeps the truncated-prompt
	// fallback, which IS the historical first prompt.
	resumedID := domain.NewSessionID()
	if err := store.CreateSession(ctx, resumedID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	resumedCtl := newController(resumedID, false)
	resumedCtl.maybeGenerateSessionTitle(1, "继续", true)
	time.Sleep(200 * time.Millisecond)
	if calls := len(model.Calls()); calls != 2 {
		t.Fatalf("model calls = %d, want still 2 (history gate must hold)", calls)
	}

	// Session switch re-arms the one-shot: the next session on the same
	// controller gets its own sidecar run.
	switchCtl := newController(domain.NewSessionID(), false)
	switchCtl.state = ControllerStateIdle
	switchCtl.sessionCtx = ctx
	switchCtl.titleTriggered = true
	switchCtl.titlePending = true
	resCh := make(chan controllerResult, 1)
	switchCtl.handleNewSession(controllerCommand{ResultCh: resCh})
	if res := <-resCh; res.Err != nil {
		t.Fatalf("handleNewSession: %v", res.Err)
	}
	if switchCtl.titleTriggered || switchCtl.titlePending {
		t.Fatal("session switch must re-arm the title one-shot")
	}
	switchCtl.maybeGenerateSessionTitle(1, "a brand new session", false)
	awaitTitle(t, store, switchCtl.SessionID(), "Generated Title")
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want 3 (refire after session switch)", calls)
	}
}
