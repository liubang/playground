// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

package app

import (
	"context"
	"fmt"
	"path/filepath"
	"slices"
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
	return &Bootstrap{
		ProcessRuntime: &ProcessRuntime{
			Resolved: resolved,
			Current:  resolved.Default,
			Store:    store,
		},
		Registry:  agent.NewToolRegistry(),
		SteerCell: agent.NewSteerCell(),
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
	controller := NewController(ControllerConfig{
		Bootstrap: &Bootstrap{
			ProcessRuntime: &ProcessRuntime{
				Resolved: resolved,
				Current:  resolved.Default,
				Store:    store,
			},
			Registry:  agent.NewToolRegistry(),
			SteerCell: agent.NewSteerCell(),
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
