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

package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestServeRealModelE2E is the M1' acceptance suite against a REAL LLM
// provider (the user's own ~/.loom/config.yaml). It is skipped unless
// LOOM_E2E_LLM=1, so CI never pays for or depends on a live model.
//
// Acceptance coverage (docs/SERVE_DESIGN.md §12 M1' 验收):
//  1. full turn with a real tool loop (read_file), verified end-to-end by
//     a code word the model can only know by reading the file;
//  2. runtime events flow through SessionService's pump/subscription;
//  3. snapshot watermark handoff (SubscribeEvents at Snapshot.EventSeq);
//  4. busy-turn steering;
//  5. idempotent submission;
//  6. session resume via a fresh client;
//  7. approval/question requests are auto-resolved to keep turns moving
//     (and thereby exercised when the policy requires them).
func TestServeRealModelE2E(t *testing.T) {
	if os.Getenv("LOOM_E2E_LLM") != "1" {
		t.Skip("set LOOM_E2E_LLM=1 to run the real-model acceptance suite")
	}

	ctx := context.Background()
	configPath := os.Getenv("LOOM_CONFIG")
	if configPath == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			t.Skipf("no home dir: %v", err)
		}
		configPath = filepath.Join(home, ".loom", "config.yaml")
	}
	if _, err := os.Stat(configPath); err != nil {
		t.Skipf("loom config not found at %s", configPath)
	}
	resolved, err := config.Load(configPath, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
	if err != nil {
		t.Skipf("load loom config: %v", err)
	}

	// Isolate all writable state in a temp dir: the user's session store
	// and workspace are never touched.
	tmp := t.TempDir()
	resolved.Storage.SessionDB = filepath.Join(tmp, "sessions", "sessions.db")
	if err := os.MkdirAll(filepath.Dir(resolved.Storage.SessionDB), 0o700); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	workspace := filepath.Join(tmp, "ws")
	if err := os.MkdirAll(workspace, 0o755); err != nil {
		t.Fatalf("mkdir ws: %v", err)
	}
	const codeWord = "loom-e2e-m1-biplane-42"
	if err := os.WriteFile(filepath.Join(workspace, "marker.txt"), []byte("口令是："+codeWord+"\n"), 0o600); err != nil {
		t.Fatalf("write marker: %v", err)
	}

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	bootstrap, err := app.NewBootstrap(ctx, resolved, app.BootstrapConfig{
		WorkspaceRoot: workspace,
		ArtifactDir:   filepath.Join(tmp, "artifacts"),
		Version:       "e2e",
		Logger:        discard,
	})
	if err != nil {
		t.Fatalf("NewBootstrap: %v", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := app.NewSessionService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	}()

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// --- 1+2. full turn with a real tool loop ---
	prompt := fmt.Sprintf("用 read_file 工具读取 %s 这个文件，然后只回答文件里的口令本身，不要提问。", filepath.Join(workspace, "marker.txt"))
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, prompt, nil); err != nil {
		t.Fatalf("SubmitPrompt(turn1): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	finalText := lastAssistantText(snap.Messages)
	if !strings.Contains(finalText, codeWord) {
		t.Fatalf("final answer %q does not contain the code word %q (tool loop broken?)", finalText, codeWord)
	}
	if snap.TurnCount != 1 {
		t.Fatalf("TurnCount = %d, want 1", snap.TurnCount)
	}
	if snap.EventSeq == 0 {
		t.Fatalf("snapshot EventSeq = 0, want a live watermark")
	}
	collector.assertSaw(t,
		runtimeevent.KindTurnStarted,
		runtimeevent.KindModelResponseCompleted,
		runtimeevent.KindTurnFinished,
	)
	collector.assertSawAny(t, runtimeevent.KindToolStarted, runtimeevent.KindToolPrepared)
	t.Logf("turn1 ok: code word verified via tool loop, %d events", collector.count())

	// --- 3. snapshot watermark handoff ---
	live, err := c.SubscribeEvents(ctx, snap.EventSeq)
	if err != nil {
		t.Fatalf("SubscribeEvents(EventSeq=%d): %v (watermark handoff must succeed)", snap.EventSeq, err)
	}
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, "用一个字回答：好", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn2): %v", err)
	}
	waitForAnyEvent(t, live, sessionID, 2*time.Minute)
	collector.waitTurn(t, turns+1, 3*time.Minute)
	t.Log("watermark handoff ok: subscribe at EventSeq, live events flow")

	// --- 4. busy-turn steering ---
	if _, err := c.SubmitPrompt(ctx, "慢慢数：用中文从一数到二十，每个数字单独一行。", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn3): %v", err)
	}
	// The real model latency gives a natural busy window; retry briefly to
	// avoid racing a suspiciously fast first token.
	var steered bool
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		result, err := c.SubmitPrompt(ctx, "不用数了，直接说：停。", nil)
		if err != nil {
			t.Fatalf("SubmitPrompt(steer): %v", err)
		}
		if result.Steered {
			steered = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !steered {
		t.Fatalf("second submission during an active turn was not steered")
	}
	// The steered message either drains mid-turn or relays as the next
	// turn; both paths end with the session idle and the message in the
	// transcript.
	steerDeadline := time.Now().Add(3 * time.Minute)
	for {
		snap, err = c.RequestSnapshot(ctx)
		if err != nil {
			t.Fatalf("RequestSnapshot(after steer): %v", err)
		}
		if snap.State == app.ControllerStateIdle && transcriptContains(snap.Messages, "停") {
			break
		}
		if time.Now().After(steerDeadline) {
			t.Fatalf("steered message never reached the transcript (state=%s)", snap.State)
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Log("steer ok: busy submission queued and drained")

	// --- 5. idempotent submission ---
	turnsBefore := snap.TurnCount
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, "用一个字回答：嗯", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn4): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	// Repeat the exact submission through the service's idempotency channel.
	turns = collector.turnsDone()
	res, dedup, err := svc.SubmitPrompt(ctx, sessionID, "用一个字回答：嗯", nil, "idem-e2e-1")
	if err != nil {
		t.Fatalf("SubmitPrompt(idem first): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	res2, dedup2, err := svc.SubmitPrompt(ctx, sessionID, "用一个字回答：嗯", nil, "idem-e2e-1")
	if err != nil {
		t.Fatalf("SubmitPrompt(idem repeat): %v", err)
	}
	if !dedup2 || res2 != res {
		t.Fatalf("idempotent repeat = (%+v, %v), want (%+v, true)", res2, dedup2, res)
	}
	if dedup {
		t.Fatalf("first idempotent submit marked deduplicated")
	}
	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(after idem): %v", err)
	}
	if snap.TurnCount != turnsBefore+2 {
		t.Fatalf("TurnCount = %d, want %d (repeat must not add a turn)", snap.TurnCount, turnsBefore+2)
	}
	t.Log("idempotency ok")

	// --- 6. resume via a fresh client ---
	c2 := client.NewInProc(svc)
	if err := c2.ResumeSession(ctx, sessionID); err != nil {
		t.Fatalf("ResumeSession: %v", err)
	}
	snap2, err := c2.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(resumed): %v", err)
	}
	if snap2.TurnCount != snap.TurnCount {
		t.Fatalf("resumed TurnCount = %d, want %d", snap2.TurnCount, snap.TurnCount)
	}
	if !transcriptContains(snap2.Messages, codeWord) {
		t.Fatalf("resumed transcript lost the code-word exchange")
	}
	t.Logf("resume ok: %d turns, transcript intact", snap2.TurnCount)

	t.Log("ACCEPTANCE PASS: real-model serve-path e2e complete")
}

// eventCollector drains a subscription and auto-resolves any
// approval/question requests so real-model turns never wedge.
type eventCollector struct {
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu   sync.Mutex
	seen map[runtimeevent.RuntimeEventKind]int
	turns int
}

func (c *eventCollector) run() {
	c.seen = make(map[runtimeevent.RuntimeEventKind]int)
	for evt := range c.ch {
		c.mu.Lock()
		c.seen[evt.Kind]++
		if evt.Kind == runtimeevent.KindTurnFinished {
			c.turns++
		}
		c.mu.Unlock()
		switch evt.Kind {
		case runtimeevent.KindApprovalRequested:
			var p runtimeevent.ApprovalRequestedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.ResolveApproval(context.Background(), app.ApprovalBinding{
					ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
				}, domain.DecisionAllow, nil)
			}
		case runtimeevent.KindQuestionAsked:
			var p runtimeevent.QuestionAskedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.AnswerQuestion(context.Background(), p.QuestionID, domain.QuestionAnswer{Skipped: true})
			}
		}
	}
}

func (c *eventCollector) turnsDone() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.turns
}

func (c *eventCollector) waitTurn(t *testing.T, n int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if c.turnsDone() >= n {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for turn %d to finish (done=%d)", n, c.turnsDone())
}

func (c *eventCollector) assertSaw(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] == 0 {
			t.Fatalf("never saw event %q", k)
		}
	}
}

func (c *eventCollector) assertSawAny(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] > 0 {
			return
		}
	}
	t.Fatalf("never saw any of %v", kinds)
}

func (c *eventCollector) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, v := range c.seen {
		n += v
	}
	return n
}

func waitForAnyEvent(t *testing.T, ch <-chan runtimeevent.RuntimeEvent, want domain.SessionID, timeout time.Duration) {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case evt, ok := <-ch:
			if !ok {
				t.Fatalf("live channel closed before any event")
			}
			if evt.SessionID == want {
				return
			}
		case <-deadline:
			t.Fatalf("timed out waiting for a live event after watermark handoff")
		}
	}
}

func lastAssistantText(messages []domain.Message) string {
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == domain.RoleAssistant {
			return strings.Join(messages[i].TextParts(), "")
		}
	}
	return ""
}

func transcriptContains(messages []domain.Message, needle string) bool {
	for _, m := range messages {
		for _, part := range m.Parts {
			if part.Kind == domain.PartText && strings.Contains(part.Text, needle) {
				return true
			}
		}
	}
	return false
}
