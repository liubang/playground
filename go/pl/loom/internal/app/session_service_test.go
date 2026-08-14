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

package app

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

func newTestService(t *testing.T, model domain.Model) (*SessionService, *runtimeevent.Broker) {
	t.Helper()
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := NewSingletonWorkspaceService(testBootstrap(store, model), broker, SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	return svc, broker
}

func nextEvent(t *testing.T, ch <-chan runtimeevent.RuntimeEvent) runtimeevent.RuntimeEvent {
	t.Helper()
	select {
	case evt, ok := <-ch:
		if !ok {
			t.Fatalf("event channel closed unexpectedly")
		}
		return evt
	case <-time.After(2 * time.Second):
		t.Fatalf("timed out waiting for event")
		return runtimeevent.RuntimeEvent{}
	}
}

// TestSessionServicePreferenceInheritance: a manual model/reasoning switch
// becomes the process-level preference — sessions created afterwards start
// from it, and the model catalog's default follows.
func TestSessionServicePreferenceInheritance(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	snap, err := svc.Snapshot(ctx, h.ID)
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	if snap.ModelName != "test-model" {
		t.Fatalf("initial model = %q, want configured default test-model", snap.ModelName)
	}

	if _, err := svc.SetModel(ctx, h.ID, "test/new-model"); err != nil {
		t.Fatalf("SetModel: %v", err)
	}
	if _, err := svc.SetReasoning(ctx, h.ID, "high"); err != nil {
		t.Fatalf("SetReasoning: %v", err)
	}

	h2, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession 2: %v", err)
	}
	snap2, err := svc.Snapshot(ctx, h2.ID)
	if err != nil {
		t.Fatalf("Snapshot 2: %v", err)
	}
	if snap2.ModelName != "new-model" {
		t.Fatalf("new session model = %q, want inherited new-model", snap2.ModelName)
	}
	if snap2.ReasoningEffort != "high" || !snap2.ReasoningOverridden {
		t.Fatalf("new session reasoning = %q overridden=%v, want high/true",
			snap2.ReasoningEffort, snap2.ReasoningOverridden)
	}
	if cat := svc.ModelCatalog(); cat.Default != "test/new-model" {
		t.Fatalf("catalog default = %q, want test/new-model", cat.Default)
	}
}

// TestModelCatalogModalities: the picker wire shape carries each model's
// declared modalities so the frontend can badge vision-capable models and
// gate image attachments; undeclared models stay empty (text-only).
func TestModelCatalogModalities(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	cat := svc.ModelCatalog()
	got := map[string][]string{}
	for _, m := range cat.Models {
		got[m.Provider+"/"+m.Name] = m.Modalities
	}
	if mods := got["test/new-model"]; len(mods) != 2 || mods[0] != "text" || mods[1] != "image" {
		t.Fatalf("new-model modalities = %v, want [text image]", mods)
	}
	if mods := got["test/test-model"]; len(mods) != 0 {
		t.Fatalf("test-model modalities = %v, want empty (text-only)", mods)
	}
}

// TestProcessRuntimeLoadPrefs: persisted preferences are restored over the
// configured defaults; unresolvable values (e.g. a model removed from the
// config) are ignored without breaking startup.
func TestProcessRuntimeLoadPrefs(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	resolved := testResolvedConfig(fakes.NewFakeModel())

	proc := &ProcessRuntime{Current: resolved.Default, Store: store}
	proc.SwapResolved(resolved)
	if got := proc.CurrentModel(); got != resolved.Default {
		t.Fatalf("CurrentModel before prefs = %v, want configured default", got)
	}

	if err := store.SetPref(ctx, "model", "test/third-model"); err != nil {
		t.Fatalf("SetPref model: %v", err)
	}
	if err := store.SetPref(ctx, "reasoning", "low"); err != nil {
		t.Fatalf("SetPref reasoning: %v", err)
	}
	proc.loadPrefs(ctx)
	if got := proc.CurrentModel(); got.String() != "test/third-model" {
		t.Fatalf("CurrentModel after load = %v, want test/third-model", got)
	}
	if got := proc.ReasoningPreference(); got != "low" {
		t.Fatalf("ReasoningPreference after load = %q, want low", got)
	}

	// An unresolvable persisted model leaves the configured default intact.
	if err := store.SetPref(ctx, "model", "ghost/removed-model"); err != nil {
		t.Fatalf("SetPref ghost: %v", err)
	}
	proc2 := &ProcessRuntime{Current: resolved.Default, Store: store}
	proc2.SwapResolved(resolved)
	proc2.loadPrefs(ctx)
	if got := proc2.CurrentModel(); got != resolved.Default {
		t.Fatalf("CurrentModel with ghost pref = %v, want configured default", got)
	}
}

// countReapLoopGoroutines reports how many exsession reaper goroutines
// are currently running. The reaper exits only via Manager.Close, so its
// presence is the observable signal for the M22 failure-cleanup path.
func countReapLoopGoroutines() int {
	buf := make([]byte, 4<<20)
	n := runtime.Stack(buf, true)
	return bytes.Count(buf[:n], []byte("exsession.(*Manager).reapLoop"))
}

// TestNewWorkspaceBootstrapFailureClosesSessionManager is the M22
// regression lock: a sub-agent assembly failure must release the
// already-created exec-session manager — before the fix those paths
// returned without Close, leaking the manager's reaper goroutine for the
// process lifetime.
func TestNewWorkspaceBootstrapFailureClosesSessionManager(t *testing.T) {
	ctx := context.Background()
	resolved := testResolvedConfig(fakes.NewFakeModel())
	resolved.Storage = config.ResolvedStorage{BaseDir: t.TempDir()}
	resolved.Subagent.Enabled = true
	if err := os.MkdirAll(resolved.Storage.SessionsDir(), 0o755); err != nil {
		t.Fatalf("mkdir sessions dir: %v", err)
	}
	proc, err := NewProcessRuntime(ctx, resolved, ProcessRuntimeConfig{ArtifactDir: filepath.Join(t.TempDir(), "artifacts")})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	t.Cleanup(proc.Close)

	baseline := countReapLoopGoroutines()
	bootstrapSubagentFailpoint = func() error { return errors.New("injected sub-agent failure") }
	t.Cleanup(func() { bootstrapSubagentFailpoint = nil })

	if _, err := NewWorkspaceBootstrap(ctx, proc, BootstrapConfig{WorkspaceRoot: t.TempDir()}); err == nil ||
		!strings.Contains(err.Error(), "injected sub-agent failure") {
		t.Fatalf("NewWorkspaceBootstrap error = %v, want injected failure", err)
	}

	// The reaper exits asynchronously once Close lands; give it a moment.
	deadline := time.Now().Add(2 * time.Second)
	for countReapLoopGoroutines() != baseline && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if got := countReapLoopGoroutines(); got != baseline {
		t.Fatalf("reaper goroutines = %d, want baseline %d (session manager leaked)", got, baseline)
	}
}

func TestSessionServiceCreateAndSingleton(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if h.ID.IsZero() {
		t.Fatalf("handle ID is zero after CreateSession")
	}
	if got, ok := svc.Get(h.ID); !ok || got != h {
		t.Fatalf("Get(%s) = (%p, %v), want the same handle", h.ID, got, ok)
	}
	// Resuming a live session returns the same handle (one Controller per
	// SessionID process-wide).
	again, err := svc.ResumeSession(ctx, h.ID)
	if err != nil {
		t.Fatalf("ResumeSession(live): %v", err)
	}
	if again != h {
		t.Fatalf("ResumeSession(live) returned a different handle")
	}
}

func TestSessionServiceSubmitPromptAndIdempotency(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	svc, _ := newTestService(t, model)
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	result, dedup, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "key-1", false)
	if err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	if dedup {
		t.Fatalf("first submit deduplicated = true, want false")
	}
	if result.Turn != 1 || result.Steered {
		t.Fatalf("SubmitResult = %+v, want {Turn:1}", result)
	}
	waitForIdle(t, h.Controller)

	again, dedup, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "key-1", false)
	if err != nil {
		t.Fatalf("SubmitPrompt(repeat): %v", err)
	}
	if !dedup {
		t.Fatalf("repeat submit deduplicated = false, want true")
	}
	if again != result {
		t.Fatalf("deduplicated result = %+v, want %+v", again, result)
	}
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls = %d, want 1 (idempotent retry did not re-run the turn)", calls)
	}
}

func TestSessionServiceIsolatesSessionState(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h1, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(1): %v", err)
	}
	h2, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(2): %v", err)
	}
	if h1.Runtime.GoalCell == h2.Runtime.GoalCell ||
		h1.Runtime.PlanCell == h2.Runtime.PlanCell ||
		h1.Runtime.SteerCell == h2.Runtime.SteerCell ||
		h1.Runtime.Questioner == h2.Runtime.Questioner {
		t.Fatalf("sessions share mutable state cells/questioner")
	}
	if h1.Runtime.Registry == h2.Runtime.Registry {
		t.Fatalf("sessions share the same overlay registry")
	}
}

func TestSessionServiceSubscribeEventsReplayAndLive(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	svc, _ := newTestService(t, model)
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, _, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, h.Controller)

	// Subscribe from the beginning: the replayed tail must contain
	// turn.started for this session.
	ch, err := svc.SubscribeEvents(ctx, h.ID, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	var sawTurnStarted bool
	var lastSeq uint64
	for i := 0; i < 16; i++ {
		select {
		case evt, ok := <-ch:
			if !ok {
				i = 16
				continue
			}
			if evt.SessionID != h.ID {
				t.Fatalf("event for foreign session %s leaked into subscription", evt.SessionID)
			}
			if evt.Kind == runtimeevent.KindTurnStarted {
				sawTurnStarted = true
			}
			lastSeq = evt.Sequence
		case <-time.After(500 * time.Millisecond):
			i = 16
		}
	}
	if !sawTurnStarted {
		t.Fatalf("replayed events did not contain turn.started")
	}
	// A cursor at the last seen sequence is honorably served (no dup/loss).
	if _, err := svc.SubscribeEvents(ctx, h.ID, lastSeq); err != nil {
		t.Fatalf("SubscribeEvents(after=%d): %v", lastSeq, err)
	}
}

func TestSessionServiceSubscribeCursorInvalid(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, err := svc.SubscribeEvents(ctx, h.ID, 1<<40); !errors.Is(err, ErrCursorInvalid) {
		t.Fatalf("SubscribeEvents(future cursor) error = %v, want ErrCursorInvalid", err)
	}
	if _, err := svc.SubscribeEvents(ctx, domain.NewSessionID(), 0); !errors.Is(err, ErrSessionNotFound) {
		t.Fatalf("SubscribeEvents(unknown session) error = %v, want ErrSessionNotFound", err)
	}
}

func TestSessionServiceResumeFromStore(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	svc, _ := newTestService(t, model)
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, _, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, h.Controller)
	sessionID := h.ID

	// Simulate a fresh process attaching to the persisted session: the
	// service has no live handle, ResumeSession rebuilds one.
	resumed, err := svc.ResumeSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("ResumeSession: %v", err)
	}
	if resumed != h {
		t.Fatalf("ResumeSession returned a new handle for a live session")
	}
	snap, err := svc.Snapshot(ctx, sessionID)
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	if snap.TurnCount != 1 {
		t.Fatalf("snapshot TurnCount = %d, want 1", snap.TurnCount)
	}
}

// TestSessionServiceSnapshotWatermarkHandoff is the H2 regression lock:
// Snapshot.EventSeq is a GLOBAL watermark that routinely exceeds a quiet
// session's own replay max — subscribing with it must succeed.
func TestSessionServiceSnapshotWatermarkHandoff(t *testing.T) {
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "busy done", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "quiet done", StopReason: domain.StopEndTurn},
	)
	svc, broker := newTestService(t, model)
	ctx := context.Background()

	busy, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(busy): %v", err)
	}
	quiet, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession(quiet): %v", err)
	}
	// The busy session pushes the global sequence forward; the quiet
	// session's replay ring stays empty (it has no events of its own).
	if _, _, err := svc.SubmitPrompt(ctx, busy.ID, "work", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(busy): %v", err)
	}
	waitForIdle(t, busy.Controller)

	// Simulate a legitimate interleaving: the quiet controller samples its
	// projection watermark AFTER the busy session's events were published —
	// the watermark then exceeds everything in the quiet ring.
	quiet.Controller.mu.Lock()
	quiet.Controller.appliedSeq = broker.Sequence()
	quiet.Controller.mu.Unlock()

	snap, err := svc.Snapshot(ctx, quiet.ID)
	if err != nil {
		t.Fatalf("Snapshot(quiet): %v", err)
	}
	if snap.EventSeq <= quiet.Replay.MaxSeen() {
		t.Fatalf("test setup: watermark %d does not exceed quiet replay max %d", snap.EventSeq, quiet.Replay.MaxSeen())
	}
	ch, err := svc.SubscribeEvents(ctx, quiet.ID, snap.EventSeq)
	if err != nil {
		t.Fatalf("SubscribeEvents(quiet, watermark=%d): %v (must accept global watermarks)", snap.EventSeq, err)
	}
	// Live events still flow after the watermark handoff.
	if _, _, err := svc.SubmitPrompt(ctx, quiet.ID, "hello", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt(quiet): %v", err)
	}
	evt := nextEvent(t, ch)
	if evt.SessionID != quiet.ID {
		t.Fatalf("live event session = %s, want %s", evt.SessionID, quiet.ID)
	}
	waitForIdle(t, quiet.Controller)
}

// TestSessionServiceSubscribeLatestAfterInvalidate is the M6 regression
// lock: after a pump-resync invalidation, pre-gap cursors fail loudly and
// the snapshot+SubscribeLatest resync path works.
func TestSessionServiceSubscribeLatestAfterInvalidate(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	svc, _ := newTestService(t, model)
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, _, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "", false); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, h.Controller)

	snap, err := svc.Snapshot(ctx, h.ID)
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	// Simulate the pump losing events after the snapshot watermark.
	svc.invalidateAll()
	if _, err := svc.SubscribeEvents(ctx, h.ID, snap.EventSeq); !errors.Is(err, ErrCursorInvalid) {
		t.Fatalf("SubscribeEvents(pre-gap cursor) error = %v, want ErrCursorInvalid", err)
	}
	// Resync path: snapshot is complete, live-only re-attach succeeds.
	ch, err := svc.SubscribeLatest(ctx, h.ID)
	if err != nil {
		t.Fatalf("SubscribeLatest: %v", err)
	}
	select {
	case evt, ok := <-ch:
		t.Fatalf("live-only subscription delivered unexpected event %v (ok=%v)", evt.Kind, ok)
	case <-time.After(200 * time.Millisecond):
	}
}

// TestSessionServiceConcurrentIdempotentSubmit is the M7 regression lock:
// concurrent retries with the same idempotency key share one execution.
func TestSessionServiceConcurrentIdempotentSubmit(t *testing.T) {
	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	svc, _ := newTestService(t, model)
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	const retries = 8
	results := make(chan SubmitResult, retries)
	dedups := make(chan bool, retries)
	errs := make(chan error, retries)
	var wg sync.WaitGroup
	for i := 0; i < retries; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			res, dedup, err := svc.SubmitPrompt(ctx, h.ID, "question", nil, "same-key", false)
			results <- res
			dedups <- dedup
			errs <- err
		}()
	}
	wg.Wait()
	waitForIdle(t, h.Controller)
	first := <-results
	for i := 1; i < retries; i++ {
		if got := <-results; got != first {
			t.Fatalf("concurrent retry result = %+v, want %+v", got, first)
		}
	}
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("model calls = %d, want 1 (single-flight)", calls)
	}
	if err := <-errs; err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
}

func TestSessionServiceDraining(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := svc.Shutdown(ctx); err != nil {
		t.Fatalf("Shutdown: %v", err)
	}
	if _, err := svc.CreateSession(ctx, domain.WorkspaceID{}); !errors.Is(err, ErrDraining) {
		t.Fatalf("CreateSession after shutdown error = %v, want ErrDraining", err)
	}
	if _, _, err := svc.SubmitPrompt(ctx, h.ID, "q", nil, "", false); !errors.Is(err, ErrDraining) {
		t.Fatalf("SubmitPrompt after shutdown error = %v, want ErrDraining", err)
	}
}

// TestSessionServiceSubscribeAfterShutdownDrains is the M20 regression
// lock: subscriptions registered while the service is draining are
// rejected, not attached to a forward goroutine with no lifecycle
// guarantee.
func TestSessionServiceSubscribeAfterShutdownDrains(t *testing.T) {
	svc, _ := newTestService(t, fakes.NewFakeModel())
	ctx := context.Background()

	h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := svc.Shutdown(ctx); err != nil {
		t.Fatalf("Shutdown: %v", err)
	}
	if _, err := svc.SubscribeEvents(ctx, h.ID, 0); !errors.Is(err, ErrDraining) {
		t.Fatalf("SubscribeEvents after shutdown error = %v, want ErrDraining", err)
	}
	if _, err := svc.SubscribeLatest(ctx, h.ID); !errors.Is(err, ErrDraining) {
		t.Fatalf("SubscribeLatest after shutdown error = %v, want ErrDraining", err)
	}
}

// TestSessionServiceSubscribeShutdownRace hammers subscriptions against
// Shutdown. Before M20 the wg.Add in subscribeLocked could land after
// Shutdown's wg.Wait began (WaitGroup misuse, a fatal panic) — with the
// fix every subscription either wins a tracked goroutine (its channel
// closes when the service goes down) or is rejected with ErrDraining.
func TestSessionServiceSubscribeShutdownRace(t *testing.T) {
	for i := 0; i < 20; i++ {
		svc, _ := newTestService(t, fakes.NewFakeModel())
		ctx := context.Background()
		h, err := svc.CreateSession(ctx, domain.WorkspaceID{})
		if err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
		var wg sync.WaitGroup
		for j := 0; j < 8; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				ch, err := svc.SubscribeLatest(ctx, h.ID)
				if errors.Is(err, ErrDraining) {
					return
				}
				if err != nil {
					t.Errorf("SubscribeLatest: %v", err)
					return
				}
				// A successful subscription must terminate: Shutdown
				// cancels the service context and drops live subscribers.
				for range ch {
				}
			}()
		}
		// Shutdown may report "controller is closed": s.cancel() stops the
		// controller's Run loop before the per-handle shutdown command
		// arrives — immaterial here, the subscriptions are what matters.
		_ = svc.Shutdown(ctx)
		wg.Wait()
	}
}
