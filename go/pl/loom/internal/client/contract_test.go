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

package client

import (
	"context"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/server"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// The client contract suite (docs/SERVE_DESIGN.md §10): the SAME scenarios
// run against every Client implementation, so inproc and http can never
// drift apart. Cover the wire-supported method set; unsupported methods
// (checkpoint/rewind/subagent/skills/MCP/rules) are out of scope until M3.

type contractFactory func(t *testing.T, model domain.Model) Client

func testBootstrapForContract(t *testing.T, model domain.Model) (*app.Bootstrap, domain.SessionStore) {
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
	return &app.Bootstrap{
		ProcessRuntime: &app.ProcessRuntime{
			Resolved: resolved,
			Current:  resolved.Default,
			Store:    store,
		},
		Registry: agent.NewToolRegistry(),
	}, store
}

func inprocFactory(t *testing.T, model domain.Model) Client {
	t.Helper()
	bootstrap, _ := testBootstrapForContract(t, model)
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	return NewInProc(svc)
}

func httpFactory(t *testing.T, model domain.Model) Client {
	t.Helper()
	bootstrap, _ := testBootstrapForContract(t, model)
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	srv, err := server.New(server.Config{Token: "contract-token", Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("server.New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return NewHTTP(ts.URL, "contract-token")
}

func TestClientContract(t *testing.T) {
	for name, factory := range map[string]contractFactory{
		"inproc": inprocFactory,
		"http":   httpFactory,
	} {
		t.Run(name, func(t *testing.T) {
			runClientContract(t, factory)
		})
	}
}

func runClientContract(t *testing.T, factory contractFactory) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// --- lifecycle + turn + snapshot ---
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "resumed answer", StopReason: domain.StopEndTurn},
	)
	c := factory(t, model)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if c.SessionID().IsZero() {
		t.Fatalf("SessionID is zero after NewSession")
	}
	if got := c.State(); got != ControllerStateIdle {
		t.Fatalf("State = %q, want idle", got)
	}

	result, err := c.SubmitPrompt(ctx, "first question", nil)
	if err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	if result.Turn != 1 || result.Steered {
		t.Fatalf("SubmitResult = %+v, want {Turn:1}", result)
	}
	waitContractIdle(t, c)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snap.TurnCount != 1 {
		t.Fatalf("TurnCount = %d, want 1", snap.TurnCount)
	}
	if !contractTranscriptContains(snap.Messages, "first answer") {
		t.Fatalf("snapshot transcript missing the model answer")
	}
	if snap.EventSeq == 0 {
		t.Fatalf("snapshot EventSeq = 0, want a live watermark")
	}

	// --- watermark handoff: subscribing at the snapshot's watermark works
	// and streams the next turn's events ---
	events, err := c.SubscribeEvents(ctx, snap.EventSeq)
	if err != nil {
		t.Fatalf("SubscribeEvents(EventSeq=%d): %v", snap.EventSeq, err)
	}
	if _, err := c.SubmitPrompt(ctx, "second question", nil); err != nil {
		t.Fatalf("SubmitPrompt(second): %v", err)
	}
	sawTurnStarted := false
	deadline := time.After(5 * time.Second)
	for !sawTurnStarted {
		select {
		case evt, ok := <-events:
			if !ok {
				t.Fatalf("event channel closed before turn.started")
			}
			if evt.Kind == runtimeevent.KindTurnStarted {
				sawTurnStarted = true
			}
		case <-deadline:
			t.Fatalf("timed out waiting for turn.started after watermark handoff")
		}
	}
	waitContractIdle(t, c)

	// --- resume on the same client preserves the transcript ---
	// Two turns have completed (first + second), so the resumed snapshot
	// must carry both.
	if err := c.ResumeSession(ctx, c.SessionID()); err != nil {
		t.Fatalf("ResumeSession: %v", err)
	}
	snap2, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(resumed): %v", err)
	}
	if snap2.TurnCount != 2 {
		t.Fatalf("resumed TurnCount = %d, want 2", snap2.TurnCount)
	}
	if !contractTranscriptContains(snap2.Messages, "first answer") {
		t.Fatalf("resumed transcript lost history")
	}

	// --- steer semantics: a busy-turn submission queues, never 409s ---
	// (Both implementations route through SessionService.SubmitPrompt,
	// whose steer behavior is covered in app tests; here we just assert
	// the wire shape round-trips.)
	result, err = c.SubmitPrompt(ctx, "third question", nil)
	if err != nil {
		t.Fatalf("SubmitPrompt(third): %v", err)
	}
	if result.Turn != 3 {
		t.Fatalf("SubmitResult.Turn = %d, want 3", result.Turn)
	}
	waitContractIdle(t, c)

	// --- drain the SSE subscription opened earlier ---
	// The events channel from the watermark handoff must be drained
	// before the test ends, otherwise the http server's Close blocks on
	// the in-flight SSE goroutine.
	cancel()

	// --- cancel on an idle session reports a conflict-style error ---
	// Use a fresh context since cancel() was called above to drain SSE.
	idleCtx, idleCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer idleCancel()
	if err := c.CancelTurn(idleCtx); err == nil {
		t.Fatalf("CancelTurn on idle session must fail")
	}
}

func waitContractIdle(t *testing.T, c Client) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if c.State() == ControllerStateIdle {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("client never became idle (state=%s)", c.State())
}

func contractTranscriptContains(messages []domain.Message, needle string) bool {
	for _, m := range messages {
		for _, part := range m.Parts {
			if part.Kind == domain.PartText && part.Text == needle {
				return true
			}
		}
	}
	return false
}
