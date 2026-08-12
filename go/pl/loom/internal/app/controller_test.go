// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

package app

import (
	"context"
	"encoding/json"
	"fmt"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// testResolvedConfig builds a resolved config with a single "test" provider
// backed by the given (fake) model and two selectable models, so /model
// switching has somewhere to go.
func testResolvedConfig(model domain.Model) *config.ResolvedConfig {
	return &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:  "test",
			Model: model,
			Models: []config.Model{
				{Name: "test-model", ContextWindow: 128000},
				{Name: "new-model", ContextWindow: 64000},
				{Name: "third-model"},
			},
			DefaultModel: "test-model",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "test-model"},
		Limits:  domain.DefaultLimits(),
	}
}

// testBootstrap assembles the minimal Bootstrap a controller test needs.
func testBootstrap(store domain.SessionStore, model domain.Model) *Bootstrap {
	resolved := testResolvedConfig(model)
	proc := &ProcessRuntime{
		Current: resolved.Default,
		Store:   store,
	}
	proc.SwapResolved(resolved)
	return &Bootstrap{
		ProcessRuntime: proc,
		Registry:       agent.NewToolRegistry(),
		SteerCell:      agent.NewSteerCell(),
	}
}

// TestControllerNewSessionReSeedsPreference: a controller is reused across
// /new; if another session moved the global preference in between, the new
// session must start from the CURRENT preference, not the stale value the
// controller was constructed (or previously diverged) with.
func TestControllerNewSessionReSeedsPreference(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	bs := testBootstrap(store, fakes.NewFakeModel())
	controller := NewController(ControllerConfig{
		Bootstrap: bs,
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	snap, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snap.ModelName != "test-model" {
		t.Fatalf("initial model = %q, want configured default test-model", snap.ModelName)
	}

	// Another session moves the global preference mid-flight.
	bs.SetModelPreference(ctx, config.ProviderModelRef{Provider: "test", Model: "new-model"})
	bs.SetReasoningPreference(ctx, "low")

	// Reusing the same controller for /new must re-seed from the preference.
	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession 2: %v", err)
	}
	snap2, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot 2: %v", err)
	}
	if snap2.ModelName != "new-model" {
		t.Fatalf("reused controller model = %q, want re-seeded new-model", snap2.ModelName)
	}
	if snap2.ReasoningEffort != "low" || !snap2.ReasoningOverridden {
		t.Fatalf("reused controller reasoning = %q overridden=%v, want low/true",
			snap2.ReasoningEffort, snap2.ReasoningOverridden)
	}
}

func TestControllerContinuesSessionForFollowUpPrompt(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "first question"); err != nil {
		t.Fatalf("SubmitPrompt(first): %v", err)
	}
	waitForIdle(t, controller)

	if _, err := controller.SubmitPrompt(ctx, "follow-up question"); err != nil {
		t.Fatalf("SubmitPrompt(follow-up): %v", err)
	}
	waitForIdle(t, controller)

	calls := model.Calls()
	if len(calls) != 2 {
		t.Fatalf("model calls = %d, want 2", len(calls))
	}
	if got := messageText(calls[1].Messages[0]); got != "first question" {
		t.Fatalf("first message in follow-up context = %q, want first question", got)
	}
	if got := messageText(calls[1].Messages[2]); got != "follow-up question" {
		t.Fatalf("follow-up message in context = %q, want follow-up question", got)
	}

	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if len(snapshot.Messages) != 4 {
		t.Fatalf("message count = %d, want 4", len(snapshot.Messages))
	}
}

// TestControllerArchivesCompletedPlanOnNewTurn pins the turn-boundary plan
// lifecycle: a completed plan is inert for the runtime (never re-injected
// into model context), so a new prompt archives the projection — the
// snapshot drops it and live clients get an empty plan.updated BEFORE the
// turn.started, letting them hide the panel.
func TestControllerArchivesCompletedPlanOnNewTurn(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{
			{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(`{"plan":[{"goal":"step one","status":"done"},{"goal":"step two","status":"done"}]}`)},
		}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "task done", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up answer", StopReason: domain.StopEndTurn},
	)
	broker := runtimeevent.NewBroker()
	defer broker.Close()
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    broker,
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "task"); err != nil {
		t.Fatalf("SubmitPrompt(task): %v", err)
	}
	waitForIdle(t, controller)

	snap, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snap.Plan == nil || !snap.Plan.IsComplete() {
		t.Fatalf("snapshot plan after turn 1 = %+v, want a complete plan", snap.Plan)
	}

	events, unsubscribe := broker.Subscribe()
	defer unsubscribe()
	if _, err := controller.SubmitPrompt(ctx, "follow-up"); err != nil {
		t.Fatalf("SubmitPrompt(follow-up): %v", err)
	}

	// The archive is synchronous: the projection is gone before the
	// submission returns.
	snap, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot after follow-up: %v", err)
	}
	if snap.Plan != nil {
		t.Fatalf("snapshot plan after follow-up = %+v, want archived (nil)", snap.Plan)
	}

	// Live clients see an empty plan.updated ahead of the turn.started.
	deadline := time.After(2 * time.Second)
	var kinds []runtimeevent.RuntimeEventKind
	for len(kinds) < 2 {
		select {
		case evt := <-events:
			kinds = append(kinds, evt.Kind)
			if evt.Kind == runtimeevent.KindPlanUpdated {
				var plan domain.Plan
				if err := json.Unmarshal(evt.Payload, &plan); err != nil {
					t.Fatalf("decode plan.updated payload: %v", err)
				}
				if len(plan.Items) != 0 {
					t.Fatalf("archived plan.updated items = %d, want 0", len(plan.Items))
				}
			}
		case <-deadline:
			t.Fatalf("timed out waiting for archive events; got %v", kinds)
		}
	}
	if kinds[0] != runtimeevent.KindPlanUpdated || kinds[1] != runtimeevent.KindTurnStarted {
		t.Fatalf("event order = %v, want [plan.updated turn.started]", kinds)
	}

	waitForIdle(t, controller)
	snap, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot final: %v", err)
	}
	if snap.Plan != nil {
		t.Fatalf("snapshot plan after follow-up turn = %+v, want nil (no new plan)", snap.Plan)
	}
}

// TestControllerKeepsUnfinishedPlanOnNewTurn: an unfinished plan is still
// live state (re-injected into model context), so a new turn must NOT
// archive the projection — the panel stays until the next update_plan.
func TestControllerKeepsUnfinishedPlanOnNewTurn(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{
			{ID: domain.NewToolCallID(), Name: "update_plan", Arguments: json.RawMessage(`{"plan":[{"goal":"step one","status":"done"},{"goal":"step two","status":"in_progress"}]}`)},
		}, StopReason: domain.StopToolUse},
		fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn},
		// The one-shot reconcile nudge burns one extra scripted call.
		fakes.ScriptEntry{Text: "reconciled", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "task"); err != nil {
		t.Fatalf("SubmitPrompt(task): %v", err)
	}
	waitForIdle(t, controller)

	snap, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snap.Plan == nil || snap.Plan.IsComplete() {
		t.Fatalf("snapshot plan after turn 1 = %+v, want an unfinished plan", snap.Plan)
	}

	if _, err := controller.SubmitPrompt(ctx, "follow-up"); err != nil {
		t.Fatalf("SubmitPrompt(follow-up): %v", err)
	}
	snap, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot after follow-up: %v", err)
	}
	if snap.Plan == nil || len(snap.Plan.Items) != 2 {
		t.Fatalf("snapshot plan after follow-up = %+v, want the unfinished plan kept", snap.Plan)
	}
	waitForIdle(t, controller)
}

// ctxCaptureModel records the loom attribution env attached to the turn
// context of its first Stream call (docs/SERVE_DESIGN.md §4.3).
type ctxCaptureModel struct {
	inner *fakes.FakeModel
	mu    sync.Mutex
	env   map[string]string
}

func (m *ctxCaptureModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	m.mu.Lock()
	if m.env == nil {
		m.env = process.SessionEnvFromContext(ctx)
	}
	m.mu.Unlock()
	return m.inner.Stream(ctx, req)
}

func (m *ctxCaptureModel) capturedEnv() map[string]string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.env
}

// TestControllerInjectsSessionEnvViaTurnContext pins the per-session
// attribution channel: the turn context carries the env (so concurrent
// sessions never share a process-level value), and the Controller path
// never writes the process-level AtomicSessionEnv.
func TestControllerInjectsSessionEnvViaTurnContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	sessionEnv := &process.AtomicSessionEnv{}
	model := &ctxCaptureModel{inner: fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})}
	bootstrap := testBootstrap(store, model)
	bootstrap.Version = "0.2.0-dev"
	bootstrap.SessionEnv = sessionEnv
	controller := NewController(ControllerConfig{
		Bootstrap: bootstrap,
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "question"); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, controller)

	env := model.capturedEnv()
	if env == nil {
		t.Fatalf("model was never called with a session-env context")
	}
	if got := env[process.EnvSessionID]; got != snapshot.SessionID.String() {
		t.Fatalf("LOOM_SESSION_ID = %q, want session %q", got, snapshot.SessionID)
	}
	if got := env[process.EnvAgentName]; got != "loom" {
		t.Fatalf("LOOM_AGENT_NAME = %q, want loom", got)
	}
	if got := env[process.EnvAgentVersion]; got != "0.2.0-dev" {
		t.Fatalf("LOOM_AGENT_VERSION = %q, want 0.2.0-dev", got)
	}
	// The process-level holder stays untouched on the Controller path —
	// the global write channel was removed in favor of ctx injection.
	if got := sessionEnv.Get(); len(got) != 0 {
		t.Fatalf("process-level session env = %v, want untouched", got)
	}
}

// gateModel blocks its first Stream call until Open is called or ctx is
// cancelled, giving tests a deterministic "turn is busy" window. Later
// calls (including relay turns) pass straight through after Open.
type gateModel struct {
	inner   *fakes.FakeModel
	started chan struct{}
	release chan struct{}
	once    sync.Once
}

func newGateModel(inner *fakes.FakeModel) *gateModel {
	return &gateModel{inner: inner, started: make(chan struct{}), release: make(chan struct{})}
}

func (m *gateModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	m.once.Do(func() { close(m.started) })
	select {
	case <-m.release:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	return m.inner.Stream(ctx, req)
}

// Open releases the gate; it must be called exactly once.
func (m *gateModel) Open() { close(m.release) }

func userTexts(messages []domain.Message) []string {
	var out []string
	for _, m := range messages {
		if m.Role == domain.RoleUser {
			out = append(out, strings.Join(m.TextParts(), ""))
		}
	}
	return out
}

// TestControllerSteerInjectsBeforeNextModelCall is the core steer
// acceptance: a message queued while a turn is starting is drained in
// prepare and reaches the very next model request as a regular user
// message, after the turn's prompt.
func TestControllerSteerInjectsBeforeNextModelCall(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	bootstrap := testBootstrap(store, model)
	if err := bootstrap.SteerCell.Put("steer note"); err != nil {
		t.Fatalf("SteerCell.Put: %v", err)
	}
	controller := NewController(ControllerConfig{
		Bootstrap: bootstrap,
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "question"); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, controller)

	calls := model.Calls()
	if len(calls) != 1 {
		t.Fatalf("model calls = %d, want 1", len(calls))
	}
	if got, want := userTexts(calls[0].Messages), []string{"question", "steer note"}; !slices.Equal(got, want) {
		t.Fatalf("request user messages = %v, want %v", got, want)
	}
	// The injection drained the cell: nothing relays after the turn.
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if len(snapshot.PendingSteers) != 0 {
		t.Fatalf("PendingSteers = %v, want drained", snapshot.PendingSteers)
	}
}

// TestControllerSteerWhileBusyAndRelayAfterCancel covers the whole steer
// lifecycle: a busy submission is queued (SubmitResult.Steered), shows up
// in Snapshot.PendingSteers, and Ctrl+C relays it as the next turn's prompt.
func TestControllerSteerWhileBusyAndRelayAfterCancel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	inner := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second", StopReason: domain.StopEndTurn},
	)
	model := newGateModel(inner)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "one"); err != nil {
		t.Fatalf("SubmitPrompt(one): %v", err)
	}
	<-model.started // the first model call is in flight: the turn is busy

	result, err := controller.SubmitPrompt(ctx, "two")
	if err != nil {
		t.Fatalf("SubmitPrompt(two): %v", err)
	}
	if !result.Steered || result.QueueLen != 1 {
		t.Fatalf("SubmitResult = %+v, want steered with queue length 1", result)
	}
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if !slices.Equal(snapshot.PendingSteers, []string{"two"}) {
		t.Fatalf("PendingSteers = %v, want [two]", snapshot.PendingSteers)
	}

	// Ctrl+C flushes: cancel the turn, then let the relay turn through.
	if err := controller.CancelTurn(ctx); err != nil {
		t.Fatalf("CancelTurn: %v", err)
	}
	model.Open()
	// The gate cancelled the first Stream at the select, so it never
	// reached the fake: inner's FIRST call is the relay turn's, and it
	// carries the continued transcript (turn 1's prompt + the steered one).
	waitForCalls(t, inner, 1)

	calls := inner.Calls()
	if got, want := userTexts(calls[0].Messages), []string{"one", "two"}; !slices.Equal(got, want) {
		t.Fatalf("relay turn user messages = %v, want %v", got, want)
	}
	snapshot, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if len(snapshot.PendingSteers) != 0 {
		t.Fatalf("PendingSteers after relay = %v, want empty", snapshot.PendingSteers)
	}
}

// TestControllerSteerLandingAfterTurnEndRelaysImmediately is the M21
// regression: dispatch routes a submission to the steer cell from a state
// read taken while the turn was busy, but the turn finishes before the
// message lands — onTurnFinished's relay already saw an empty cell.
// Without the post-enqueue state re-check the message would strand in the
// cell until the next external submission, with the UI already showing
// "Queued". The steerHook forces exactly that interleaving.
func TestControllerSteerLandingAfterTurnEndRelaysImmediately(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	inner := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second", StopReason: domain.StopEndTurn},
	)
	model := newGateModel(inner)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "one"); err != nil {
		t.Fatalf("SubmitPrompt(one): %v", err)
	}
	<-model.started // the first model call is in flight: the turn is busy

	// Open the TOCTOU window: the hook runs after dispatch read the busy
	// state but before the message reaches the cell, and lets the turn
	// finish (state → Idle, relay against the still-empty cell).
	controller.steerHook = func() {
		model.Open()
		waitForIdle(t, controller)
	}
	result, err := controller.SubmitPrompt(ctx, "two")
	if err != nil {
		t.Fatalf("SubmitPrompt(two): %v", err)
	}
	if !result.Steered {
		t.Fatalf("SubmitResult = %+v, want steered", result)
	}

	// The steer must become the next turn's prompt immediately, not wait
	// for another external submission.
	waitForCalls(t, inner, 2)
	calls := inner.Calls()
	if got := userTexts(calls[1].Messages); !slices.Contains(got, "two") {
		t.Fatalf("relay turn user messages = %v, want one containing %q", got, "two")
	}
	waitForIdle(t, controller)
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if len(snapshot.PendingSteers) != 0 {
		t.Fatalf("PendingSteers after relay = %v, want empty", snapshot.PendingSteers)
	}
}

// slowModel delays each Stream call so the turn stays alive long enough
// for concurrent commands to overlap the turn goroutine, while still
// respecting cancellation.
type slowModel struct {
	inner *fakes.FakeModel
	delay time.Duration
}

func (m *slowModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	timer := time.NewTimer(m.delay)
	defer timer.Stop()
	select {
	case <-timer.C:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	return m.inner.Stream(ctx, req)
}

// Regression (REVIEW H3): runTurn writes c.runID under c.mu in the turn
// goroutine, while handleSteer/handleCancelTurn used to read it without the
// lock when publishing ephemeral events. Run with -race: the unsynchronized
// pair is flagged.
func TestControllerPublishPathsDoNotRaceRunID(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	script := make([]fakes.ScriptEntry, 64)
	for i := range script {
		script[i] = fakes.ScriptEntry{Text: "ok", StopReason: domain.StopEndTurn}
	}
	model := &slowModel{inner: fakes.NewFakeModel(script...), delay: 10 * time.Millisecond}
	bootstrap := testBootstrap(store, model)
	controller := NewController(ControllerConfig{
		Bootstrap: bootstrap,
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	for i := 0; i < 30; i++ {
		if _, err := controller.SubmitPrompt(ctx, "question"); err != nil {
			t.Fatalf("SubmitPrompt #%d: %v", i, err)
		}
		// Spam steers and a cancel while the turn goroutine starts up and
		// publishes; each publish path used to read c.runID unsynchronized.
		for j := 0; j < 8; j++ {
			_, _ = controller.SubmitPrompt(ctx, "steer")
			time.Sleep(time.Millisecond)
		}
		_ = bootstrap.SteerCell.Take() // drain so nothing is relayed
		_ = controller.CancelTurn(ctx) // turn may already be done; error ignored
		waitForIdle(t, controller)
		_ = bootstrap.SteerCell.Take()
	}
}

// TestControllerSteerCellFullRejects checks the soft capacity: busy
// submissions past SteerCellCapacity are rejected so the UI can restore
// the draft.
func TestControllerSteerCellFullRejects(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	inner := fakes.NewFakeModel(fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn})
	model := newGateModel(inner)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "one"); err != nil {
		t.Fatalf("SubmitPrompt(one): %v", err)
	}
	<-model.started

	for i := range agent.SteerCellCapacity {
		if _, err := controller.SubmitPrompt(ctx, fmt.Sprintf("queued %d", i)); err != nil {
			t.Fatalf("SubmitPrompt #%d: %v", i, err)
		}
	}
	if _, err := controller.SubmitPrompt(ctx, "overflow"); err == nil {
		t.Fatal("SubmitPrompt past capacity should fail")
	}

	if err := controller.CancelTurn(ctx); err != nil {
		t.Fatalf("CancelTurn: %v", err)
	}
	model.Open()
	waitForIdle(t, controller)
}

// waitForCalls blocks until the fake model has received at least n
// requests (relay turns reach the model asynchronously via cmdCh).
func waitForCalls(t *testing.T, model *fakes.FakeModel, n int) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if len(model.Calls()) >= n {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("model calls = %d, want >= %d", len(model.Calls()), n)
}

func waitForIdle(t *testing.T, controller *Controller) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if controller.State() == ControllerStateIdle {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("controller state = %q, want idle", controller.State())
}

func messageText(message domain.Message) string {
	for _, part := range message.Parts {
		if part.Kind == domain.PartText {
			return part.Text
		}
	}
	return ""
}

func TestToolCallTargetPrefersWritePaths(t *testing.T) {
	if got := toolCallTarget(toolCallAuditDTO{WritePaths: []string{"w.go"}, ReadPaths: []string{"r.go"}}); got != "w.go" {
		t.Fatalf("target = %q, want write path", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{ReadPaths: []string{"r.go"}}); got != "r.go" {
		t.Fatalf("target = %q, want read path", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{ApprovalDesc: "run: go test ./..."}); got != "run: go test ./..." {
		t.Fatalf("target = %q, want approval description", got)
	}
	if got := toolCallTarget(toolCallAuditDTO{}); got != "" {
		t.Fatalf("target = %q, want empty", got)
	}
}

func TestToolResultPreviewExtractsAndBounds(t *testing.T) {
	callID := domain.NewToolCallID()
	msg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID:  callID,
				Status:  domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{Kind: domain.PartText, Text: "line1\nline2"}},
			},
		}},
	}
	gotID, preview, artifacts := toolResultPreview(msg)
	if gotID != callID {
		t.Fatalf("call ID = %v, want %v", gotID, callID)
	}
	if preview != "line1\nline2" {
		t.Fatalf("preview = %q, want joined text", preview)
	}
	if len(artifacts) != 0 {
		t.Fatalf("artifacts = %v, want none for a text-only result", artifacts)
	}

	// A result carrying an artifact reference surfaces it for live rendering.
	ref := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 42}
	artMsg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID: callID,
				Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{
					{Kind: domain.PartText, Text: "header"},
					{Kind: domain.PartArtifact, Artifact: &ref},
				},
			},
		}},
	}
	gotID, _, gotArtifacts := toolResultPreview(artMsg)
	if gotID != callID || len(gotArtifacts) != 1 || gotArtifacts[0] != ref {
		t.Fatalf("artifact extraction = (%v, %v), want (%v, [%v])", gotID, gotArtifacts, callID, ref)
	}

	errMsg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID: callID,
				Status: domain.ToolStatusError,
				Error:  &domain.ToolError{Code: "x", Message: "boom"},
			},
		}},
	}
	if _, preview, _ := toolResultPreview(errMsg); preview != "boom" {
		t.Fatalf("error preview = %q, want error message", preview)
	}
}

func TestBoundPreviewLinesTruncates(t *testing.T) {
	if got := boundPreviewLines("a\nb\nc\nd", 2, 100); got != "a\nb\n…" {
		t.Fatalf("line-bounded preview = %q", got)
	}
	if got := boundPreviewLines("abcdefgh", 10, 3); got != "abc\n…" {
		t.Fatalf("byte-bounded preview = %q", got)
	}
	if got := boundPreviewLines("  \n", 10, 10); got != "" {
		t.Fatalf("blank preview = %q, want empty", got)
	}
}

func TestControllerSetModel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, fakes.NewFakeModel()),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	// The bootstrap default is in effect before any switch.
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snapshot.ModelName != "test-model" || snapshot.ProviderName != "test" {
		t.Fatalf("snapshot = %q/%q, want bootstrap default", snapshot.ProviderName, snapshot.ModelName)
	}
	if snapshot.ContextWindow != 128000 {
		t.Fatalf("snapshot.ContextWindow = %d, want 128000", snapshot.ContextWindow)
	}

	result, err := controller.SetModel(ctx, "new-model")
	if err != nil {
		t.Fatalf("SetModel: %v", err)
	}
	if result.Prev != (config.ProviderModelRef{Provider: "test", Model: "test-model"}) {
		t.Fatalf("SetModel prev = %+v", result.Prev)
	}
	if result.Meta.ContextWindow != 64000 {
		t.Fatalf("SetModel meta.ContextWindow = %d, want 64000", result.Meta.ContextWindow)
	}
	snapshot, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snapshot.ModelName != "new-model" || snapshot.ContextWindow != 64000 {
		t.Fatalf("snapshot = %q (ctx %d), want new-model/64000", snapshot.ModelName, snapshot.ContextWindow)
	}

	result, err = controller.SetModel(ctx, "test/third-model")
	if err != nil {
		t.Fatalf("SetModel(second): %v", err)
	}
	if result.Prev.Model != "new-model" {
		t.Fatalf("SetModel(second) prev = %+v, want new-model", result.Prev)
	}

	// Unknown references are rejected and keep the current model.
	if _, err := controller.SetModel(ctx, "   "); err == nil {
		t.Fatal("SetModel with blank ref should fail")
	}
	if _, err := controller.SetModel(ctx, "nosuch-model"); err == nil {
		t.Fatal("SetModel with unknown model should fail")
	}
	snapshot, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snapshot.ModelName != "third-model" {
		t.Fatalf("ModelName = %q, want third-model after rejected switch", snapshot.ModelName)
	}
}

func TestControllerSetModelAppliesFromNextTurn(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "one"); err != nil {
		t.Fatalf("SubmitPrompt(one): %v", err)
	}
	waitForIdle(t, controller)

	if _, err := controller.SetModel(ctx, "new-model"); err != nil {
		t.Fatalf("SetModel: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "two"); err != nil {
		t.Fatalf("SubmitPrompt(two): %v", err)
	}
	waitForIdle(t, controller)

	calls := model.Calls()
	if len(calls) != 2 {
		t.Fatalf("model calls = %d, want 2", len(calls))
	}
	if calls[0].ModelName != "test-model" {
		t.Errorf("turn 1 model = %q, want test-model", calls[0].ModelName)
	}
	if calls[1].ModelName != "new-model" {
		t.Errorf("turn 2 model = %q, want new-model", calls[1].ModelName)
	}
}

func TestControllerSetReasoning(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	// The default model carries a configured reasoning default; the other
	// does not, so "back to config" is observable across a model switch.
	resolved := &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:  "test",
			Model: fakes.NewFakeModel(),
			Models: []config.Model{
				{Name: "test-model", ContextWindow: 128000, Reasoning: config.Reasoning{Effort: "medium"}},
				{Name: "new-model", ContextWindow: 64000},
			},
			DefaultModel: "test-model",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "test-model"},
		Limits:  domain.DefaultLimits(),
	}
	proc := &ProcessRuntime{
		Current: resolved.Default,
		Store:   store,
	}
	proc.SwapResolved(resolved)
	controller := NewController(ControllerConfig{
		Bootstrap: &Bootstrap{
			ProcessRuntime: proc,
			Registry:       agent.NewToolRegistry(),
			SteerCell:      agent.NewSteerCell(),
		},
		Broker:   runtimeevent.NewBroker(),
		Approver: NewChannelApprover(),
		Clock:    domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	// Before any override the model's configured default is in effect.
	snapshot, err := controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snapshot.ReasoningEffort != "medium" || snapshot.ReasoningOverridden {
		t.Fatalf("snapshot reasoning = %q (overridden=%v), want medium/config", snapshot.ReasoningEffort, snapshot.ReasoningOverridden)
	}

	result, err := controller.SetReasoning(ctx, "high")
	if err != nil {
		t.Fatalf("SetReasoning(high): %v", err)
	}
	if result.Effective.Effort != domain.ReasoningEffortHigh || !result.Overridden {
		t.Fatalf("SetReasoning(high) = %+v, want high/override", result)
	}

	// The override is per-task intent: a model switch must not clear it.
	if _, err := controller.SetModel(ctx, "new-model"); err != nil {
		t.Fatalf("SetModel: %v", err)
	}
	snapshot, err = controller.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snapshot.ReasoningEffort != "high" || !snapshot.ReasoningOverridden {
		t.Fatalf("snapshot reasoning after /model = %q (overridden=%v), want high/override", snapshot.ReasoningEffort, snapshot.ReasoningOverridden)
	}

	// "default" clears the override; the new model has no configured
	// reasoning, so the provider decides (empty dial).
	result, err = controller.SetReasoning(ctx, "default")
	if err != nil {
		t.Fatalf("SetReasoning(default): %v", err)
	}
	if result.Overridden || !result.Effective.IsZero() {
		t.Fatalf("SetReasoning(default) = %+v, want zero/config", result)
	}

	if _, err := controller.SetReasoning(ctx, "extreme"); err == nil {
		t.Fatal("SetReasoning with unknown level should fail")
	}
}

func TestControllerRequestCompaction(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	// First request schedules; a duplicate only reports the pending state.
	result, err := controller.RequestCompaction(ctx)
	if err != nil {
		t.Fatalf("RequestCompaction: %v", err)
	}
	if result.AlreadyPending {
		t.Fatal("first request should not report AlreadyPending")
	}
	result, err = controller.RequestCompaction(ctx)
	if err != nil {
		t.Fatalf("RequestCompaction(duplicate): %v", err)
	}
	if !result.AlreadyPending {
		t.Fatal("duplicate request should report AlreadyPending")
	}

	// A pending request belongs to its transcript: starting a new session
	// drops it instead of compacting the wrong one.
	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	result, err = controller.RequestCompaction(ctx)
	if err != nil {
		t.Fatalf("RequestCompaction(after new session): %v", err)
	}
	if result.AlreadyPending {
		t.Fatal("the pending flag must not leak into the new session")
	}

	// The next turn consumes the one-shot flag: scheduling starts over.
	if _, err := controller.SubmitPrompt(ctx, "question"); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	waitForIdle(t, controller)

	result, err = controller.RequestCompaction(ctx)
	if err != nil {
		t.Fatalf("RequestCompaction(after turn): %v", err)
	}
	if result.AlreadyPending {
		t.Fatal("the flag must have been consumed by the completed turn")
	}
}

func TestControllerAnswerQuestion(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	questioner := NewChannelQuestioner(nil)
	bootstrap := testBootstrap(store, fakes.NewFakeModel())
	controller := NewController(ControllerConfig{
		Bootstrap:  bootstrap,
		Broker:     runtimeevent.NewBroker(),
		Approver:   NewChannelApprover(),
		Questioner: questioner,
		Clock:      domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	// A real question is pending: the controller's answer unblocks it.
	question := domain.Question{Text: "pick one", Options: []domain.QuestionOption{{Label: "a"}, {Label: "b"}}}
	answerCh := make(chan domain.QuestionAnswer, 1)
	go func() {
		answer, _ := questioner.Ask(ctx, question)
		answerCh <- answer
	}()
	deadline := time.Now().Add(2 * time.Second)
	for len(questioner.PendingQuestions()) == 0 {
		if time.Now().After(deadline) {
			t.Fatal("question never became pending")
		}
		time.Sleep(5 * time.Millisecond)
	}
	pending := questioner.PendingQuestions()

	result, err := controller.AnswerQuestion(ctx, pending[0], domain.QuestionAnswer{Selected: []string{"b"}})
	if err != nil {
		t.Fatalf("AnswerQuestion: %v", err)
	}
	if !result.Resolved {
		t.Fatal("AnswerQuestion should resolve the pending question")
	}
	select {
	case answer := <-answerCh:
		if len(answer.Selected) != 1 || answer.Selected[0] != "b" {
			t.Fatalf("answer = %+v", answer)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ask did not return after AnswerQuestion")
	}

	// Unknown or already-answered questions report Resolved=false.
	result, err = controller.AnswerQuestion(ctx, pending[0], domain.QuestionAnswer{Skipped: true})
	if err != nil {
		t.Fatalf("AnswerQuestion(stale): %v", err)
	}
	if result.Resolved {
		t.Fatal("stale question must not resolve twice")
	}
}

func TestControllerSetReasoningAppliesFromNextTurn(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first answer", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "second answer", StopReason: domain.StopEndTurn},
	)
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, model),
		Broker:    runtimeevent.NewBroker(),
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())

	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "one"); err != nil {
		t.Fatalf("SubmitPrompt(one): %v", err)
	}
	waitForIdle(t, controller)

	if _, err := controller.SetReasoning(ctx, "low"); err != nil {
		t.Fatalf("SetReasoning: %v", err)
	}
	if _, err := controller.SubmitPrompt(ctx, "two"); err != nil {
		t.Fatalf("SubmitPrompt(two): %v", err)
	}
	waitForIdle(t, controller)

	calls := model.Calls()
	if len(calls) != 2 {
		t.Fatalf("model calls = %d, want 2", len(calls))
	}
	if !calls[0].Reasoning.IsZero() {
		t.Errorf("turn 1 reasoning = %+v, want zero (no override yet)", calls[0].Reasoning)
	}
	if calls[1].Reasoning.Effort != domain.ReasoningEffortLow {
		t.Errorf("turn 2 reasoning = %+v, want low (override)", calls[1].Reasoning)
	}
}

// startPendingApproval registers a web_fetch approval request on approver
// and returns its binding once the slot is visible.
func startPendingApproval(t *testing.T, approver *ChannelApprover, url, argsHash string) (ApprovalBinding, <-chan domain.Decision) {
	t.Helper()
	approvalID := domain.NewEventID()
	callID := domain.NewToolCallID()
	result := make(chan domain.Decision, 1)
	go func() {
		decision, _ := approver.RequestApproval(context.Background(), domain.ApprovalRequest{
			ID: approvalID,
			Call: domain.PreparedCall{
				Call:     domain.ToolCall{ID: callID, Name: "web_fetch", Arguments: json.RawMessage(`{"url":` + strconv.Quote(url) + `}`)},
				ArgsHash: argsHash,
			},
		})
		result <- decision
	}()
	deadline := time.After(2 * time.Second)
	for {
		if _, ok := approver.PendingBinding(approvalID); ok {
			break
		}
		select {
		case <-deadline:
			t.Fatal("approval was not registered")
		default:
			time.Sleep(time.Millisecond)
		}
	}
	return ApprovalBinding{ApprovalID: approvalID, CallID: callID, ArgsHash: argsHash}, result
}

// forceAwaitingApproval puts the controller into awaiting_approval the way
// the publishing store would when a permission.requested event persists.
func forceAwaitingApproval(controller *Controller) {
	controller.mu.Lock()
	controller.state = ControllerStateAwaitingApproval
	controller.mu.Unlock()
}

// Regression: a permission.resolved domain event names its approval by the
// permission.requested event's ID in the PAYLOAD; the resolved event's own
// ID is a fresh, unrelated identifier. The publishing store must key every
// projection update by the payload's approval ID — keying by the resolved
// event's ID left zombie approval cards in snapshots (a session switch
// replayed every approval of the turn as still pending), pinned the
// session in awaiting_approval for the rest of the turn (the workspace
// badge overcounted), and broadcast an approval_id no frontend card
// matched.
func TestPublishingStoreApprovalResolvedKeysByApprovalID(t *testing.T) {
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	broker := runtimeevent.NewBroker()
	defer broker.Close()
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, fakes.NewFakeModel()),
		Broker:    broker,
		Approver:  NewChannelApprover(),
		Clock:     domain.RealClock{},
	})
	// The command loop is not started; the publishing store is driven
	// directly, so set the pre-approval run state by hand.
	controller.mu.Lock()
	controller.state = ControllerStateRunning
	controller.mu.Unlock()

	sessionID := domain.NewSessionID()
	ps := &publishingStore{
		broker:      broker,
		sessionID:   sessionID,
		runID:       domain.NewRunID(),
		clock:       domain.RealClock{},
		controller:  controller,
		pendingArgs: make(map[domain.ToolCallID]json.RawMessage),
	}
	events, unsubscribe := broker.Subscribe()
	defer unsubscribe()

	callID := domain.NewToolCallID()
	requestedID := domain.NewEventID()
	reqPayload, err := json.Marshal(toolCallAuditDTO{
		CallID: callID, Tool: "run_cmd", Risk: domain.R3, ArgsHash: "hash-1", ApprovalDesc: "run it",
	})
	if err != nil {
		t.Fatalf("marshal requested payload: %v", err)
	}
	ps.publishForEvent(sessionID, domain.Event{ID: requestedID, Type: domain.EventPermissionRequested, Payload: reqPayload})

	if got := controller.State(); got != ControllerStateAwaitingApproval {
		t.Fatalf("state after requested = %q, want awaiting_approval", got)
	}
	controller.mu.Lock()
	_, cardOK := controller.pendingCards[requestedID]
	// handleResolveApproval records the actor under the approval ID before
	// the loop persists the resolution.
	controller.approvalActors[requestedID] = "web"
	controller.mu.Unlock()
	if !cardOK {
		t.Fatal("requested event did not project a pending card")
	}

	// The resolved event gets its own fresh ID; the approval ID travels in
	// the payload (see agent.awaitApproval).
	resolvedID := domain.NewEventID()
	if resolvedID == requestedID {
		t.Fatal("test premise broken: resolved and requested IDs collide")
	}
	resPayload, err := json.Marshal(permissionResolvedDTO{
		ApprovalID: requestedID, CallID: callID, Decision: domain.DecisionAllow,
	})
	if err != nil {
		t.Fatalf("marshal resolved payload: %v", err)
	}
	ps.publishForEvent(sessionID, domain.Event{ID: resolvedID, Type: domain.EventPermissionResolved, Payload: resPayload})

	controller.mu.Lock()
	_, zombie := controller.pendingCards[requestedID]
	_, actorLeaked := controller.approvalActors[requestedID]
	state := controller.state
	controller.mu.Unlock()
	if zombie {
		t.Fatal("resolved approval card was not removed from the projection")
	}
	if actorLeaked {
		t.Fatal("approval actor was not consumed")
	}
	if state != ControllerStateRunning {
		t.Fatalf("state after last card resolved = %q, want running", state)
	}

	// The broadcast approval.resolved must carry the REQUESTED event's ID
	// (frontends key their cards by it) and the recorded actor.
	deadline := time.After(2 * time.Second)
	for {
		select {
		case evt := <-events:
			if evt.Kind != runtimeevent.KindApprovalResolved {
				continue
			}
			var payload runtimeevent.ApprovalResolvedPayload
			if err := json.Unmarshal(evt.Payload, &payload); err != nil {
				t.Fatalf("decode approval.resolved: %v", err)
			}
			if payload.ApprovalID != requestedID {
				t.Fatalf("broadcast approval_id = %s, want requested id %s", payload.ApprovalID, requestedID)
			}
			if payload.Actor != "web" {
				t.Fatalf("broadcast actor = %q, want web", payload.Actor)
			}
			return
		case <-deadline:
			t.Fatal("timed out waiting for approval.resolved frame")
		}
	}
}

// Regression: the "allow always" memory must land BEFORE the decision wakes
// the agent loop — the woken loop re-evaluates the batch's remaining calls
// against session memory immediately, and a late memory re-prompts a call
// the user just approved categorically (which then auto-resolves underneath
// its still-visible card).
func TestControllerResolveApprovalRemembersBeforeWakingLoop(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	approver := NewChannelApprover()
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, fakes.NewFakeModel()),
		Broker:    runtimeevent.NewBroker(),
		Approver:  approver,
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())
	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	forceAwaitingApproval(controller)

	binding, result := startPendingApproval(t, approver, "https://example.com/a", "hash-a")
	note, err := controller.ResolveApproval(ctx, binding, domain.DecisionAllow, &ApprovalRuleHint{
		ToolName:  "web_fetch",
		Arguments: json.RawMessage(`{"url":"https://example.com/a"}`),
	})
	if err != nil {
		t.Fatalf("ResolveApproval: %v", err)
	}
	if !strings.Contains(note, "example.com") {
		t.Fatalf("note = %q, want the remembered host", note)
	}
	select {
	case decision := <-result:
		if decision != domain.DecisionAllow {
			t.Fatalf("decision = %q, want allow", decision)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("pending approval was not resolved")
	}
	if !controller.sessionRules.MatchDomain("example.com") {
		t.Fatal("domain was not remembered in session rules")
	}

	// The loop's re-evaluation path: a follow-up call on the remembered host
	// auto-allows without ever registering a pending slot.
	followUp, followUpResult := domain.ApprovalRequest{
		ID: domain.NewEventID(),
		Call: domain.PreparedCall{
			Call:     domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_fetch", Arguments: json.RawMessage(`{"url":"https://example.com/b"}`)},
			ArgsHash: "hash-b",
		},
	}, make(chan domain.Decision, 1)
	go func() {
		decision, _ := controller.rulesApprover.RequestApproval(ctx, followUp)
		followUpResult <- decision
	}()
	select {
	case decision := <-followUpResult:
		if decision != domain.DecisionAllow {
			t.Fatalf("follow-up decision = %q, want auto-allow", decision)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("follow-up request reached the user instead of auto-allowing")
	}
	if got := approver.PendingCount(); got != 0 {
		t.Fatalf("pending = %d, want 0 (no user-facing request for the follow-up)", got)
	}
}

// A decision whose binding belongs to nobody's pending request must not
// write an "allow always" memory.
func TestControllerResolveApprovalMismatchedBindingDoesNotRemember(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()

	approver := NewChannelApprover()
	controller := NewController(ControllerConfig{
		Bootstrap: testBootstrap(store, fakes.NewFakeModel()),
		Broker:    runtimeevent.NewBroker(),
		Approver:  approver,
		Clock:     domain.RealClock{},
	})
	go controller.Run(ctx)
	defer controller.Shutdown(context.Background())
	if err := controller.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	forceAwaitingApproval(controller)

	binding, result := startPendingApproval(t, approver, "https://example.com/a", "hash-a")
	tampered := binding
	tampered.ArgsHash = "tampered"
	if _, err := controller.ResolveApproval(ctx, tampered, domain.DecisionAllow, &ApprovalRuleHint{
		ToolName:  "web_fetch",
		Arguments: json.RawMessage(`{"url":"https://example.com/a"}`),
	}); err == nil {
		t.Fatal("mismatched binding was accepted")
	}
	if controller.sessionRules.MatchDomain("example.com") {
		t.Fatal("mismatched binding wrote an allow-always memory")
	}
	if got := approver.PendingCount(); got != 1 {
		t.Fatalf("pending = %d, want 1 (the mismatched resolve must not consume the slot)", got)
	}

	// The genuine resolve still works afterwards.
	if _, err := controller.ResolveApproval(ctx, binding, domain.DecisionAllow, nil); err != nil {
		t.Fatalf("ResolveApproval: %v", err)
	}
	if decision := <-result; decision != domain.DecisionAllow {
		t.Fatalf("decision = %q, want allow", decision)
	}
}

// The controller must stay in awaiting_approval until the LAST pending card
// resolves: a batch can hold several asks, and flipping back to running on
// the first resolved event makes the state gate reject the rest.
func TestControllerSetRunningWaitsForLastPendingCard(t *testing.T) {
	cardA, cardB := domain.NewEventID(), domain.NewEventID()
	controller := &Controller{
		state: ControllerStateAwaitingApproval,
		pendingCards: map[domain.EventID]runtimeevent.ApprovalRequestedPayload{
			cardA: {},
			cardB: {},
		},
	}

	// The persister deletes the card before calling SetRunning.
	delete(controller.pendingCards, cardA)
	controller.SetRunning()
	if controller.state != ControllerStateAwaitingApproval {
		t.Fatalf("state = %q with one card still pending, want awaiting_approval", controller.state)
	}

	delete(controller.pendingCards, cardB)
	controller.SetRunning()
	if controller.state != ControllerStateRunning {
		t.Fatalf("state = %q after the last card resolved, want running", controller.state)
	}
}
