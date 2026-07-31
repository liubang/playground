// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

package ui

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// newTestController starts a real controller loop so state-dependent key
// handling can be exercised without fakes. The bootstrap carries one fake
// provider with two models so /model switching works in tests.
func newTestController(t *testing.T) *app.Controller {
	t.Helper()
	resolved := &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:  "test",
			Model: fakes.NewFakeModel(),
			Models: []config.Model{
				{Name: "model-a", ContextWindow: 128000},
				{Name: "model-b", ContextWindow: 64000},
			},
			DefaultModel: "model-a",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "model-a"},
		Limits:  domain.DefaultLimits(),
	}
	ctrl := app.NewController(app.ControllerConfig{
		Bootstrap: &app.Bootstrap{
			Resolved: resolved,
			Current:  resolved.Default,
			Store:    fakes.NewFakeStore(),
			Registry: agent.NewToolRegistry(),
		},
		Broker:     runtimeevent.NewBroker(),
		Approver:   app.NewChannelApprover(),
		Questioner: app.NewChannelQuestioner(nil),
	})
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go ctrl.Run(ctx)
	deadline := time.Now().Add(2 * time.Second)
	for ctrl.State() != app.ControllerStateIdle {
		if time.Now().After(deadline) {
			t.Fatal("controller did not become idle")
		}
		time.Sleep(5 * time.Millisecond)
	}
	return ctrl
}

func mustPayload(t *testing.T, v any) json.RawMessage {
	t.Helper()
	payload, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	return payload
}

func isQuitCmd(cmd tea.Cmd) bool {
	if cmd == nil {
		return false
	}
	_, ok := cmd().(tea.QuitMsg)
	return ok
}

func TestApplyRuntimeEventCoalescesDeltasByTurn(t *testing.T) {
	idx := NewBlockIndex()
	for _, delta := range []string{"hello", " world"} {
		payload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: delta})
		ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 7, Kind: runtimeevent.KindModelTextDelta, Payload: payload})
	}
	if len(idx.Order) != 1 {
		t.Fatalf("block count = %d, want 1", len(idx.Order))
	}
	block, ok := idx.Get("stream-7-1")
	if !ok || block.Content != "hello world" {
		t.Fatalf("stream block = %#v, exists=%v", block, ok)
	}
}

// Each model response gets its own assistant block, so text, tool calls,
// and the final answer interleave chronologically (Claude Code style)
// instead of the final answer overwriting the lead-in text.
func TestStreamBlocksInterleaveWithToolCalls(t *testing.T) {
	idx := NewBlockIndex()
	delta1 := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "我来查询天气，先抓取数据。"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: delta1})
	completed1 := mustPayload(t, runtimeevent.ModelResponseCompletedPayload{Text: "我来查询天气，先抓取数据。"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelResponseCompleted, Payload: completed1})

	callID := domain.NewToolCallID()
	prepared := mustPayload(t, runtimeevent.ToolPreparedPayload{CallID: callID, ToolName: "web_fetch", Risk: domain.R3, Target: "https://x"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindToolPrepared, Payload: prepared})

	delta2 := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "北京未来一周天气如下：..."})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: delta2})
	completed2 := mustPayload(t, runtimeevent.ModelResponseCompletedPayload{Text: "北京未来一周天气如下：..."})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelResponseCompleted, Payload: completed2})

	if len(idx.Order) != 3 {
		t.Fatalf("block count = %d, want 3 (text, tool, text): %v", len(idx.Order), idx.Order)
	}
	leadIn := idx.ByID[idx.Order[0]]
	tool := idx.ByID[idx.Order[1]]
	answer := idx.ByID[idx.Order[2]]
	if leadIn.Kind != BlockKindAssistant || tool.Kind != BlockKindTool || answer.Kind != BlockKindAssistant {
		t.Fatalf("interleaving broken: %v", idx.Order)
	}
	if leadIn.Content != "我来查询天气，先抓取数据。" {
		t.Fatalf("lead-in text was overwritten by the final answer: %q", leadIn.Content)
	}
	if answer.Content != "北京未来一周天气如下：..." || leadIn.ID == answer.ID {
		t.Fatalf("final answer must be its own block: %q id=%q", answer.Content, answer.ID)
	}
}

func TestPendingUserBlockDisplaysImmediatelyAndIsConfirmed(t *testing.T) {
	idx := NewBlockIndex()
	id := idx.AddPendingUserBlock("first prompt")
	block, ok := idx.Get(id)
	if !ok || block.Content != "first prompt" || block.Status != "pending" {
		t.Fatalf("pending block = %#v, exists=%v", block, ok)
	}

	payload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "first prompt"})
	confirmedID := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Kind: runtimeevent.KindTurnStarted, Payload: payload})
	if confirmedID != id || len(idx.Order) != 1 {
		t.Fatalf("confirmed ID = %q, block count = %d", confirmedID, len(idx.Order))
	}
	if block.Status != "success" {
		t.Fatalf("confirmed block status = %q, want success", block.Status)
	}
}

func TestApplyRuntimeEventShowsCollapsibleReasoning(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.ModelReasoningDeltaPayload{Delta: "inspect the request"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 7, Kind: runtimeevent.KindModelReasoningDelta, Payload: payload})

	block, ok := idx.Get("stream-7-1")
	if !ok || block.StreamReasoning != "inspect the request" {
		t.Fatalf("reasoning block = %#v, exists=%v", block, ok)
	}
	m := Model{theme: NoColorTheme()}
	if view := m.renderBlock(block); !strings.Contains(view, "Thinking... (click or Ctrl+R to expand)") {
		t.Fatalf("collapsed reasoning view = %q", view)
	}
	if !idx.ToggleLatestReasoning() {
		t.Fatal("ToggleLatestReasoning() = false")
	}
	if view := m.renderBlock(block); !strings.Contains(view, "Thinking:") || !strings.Contains(view, "inspect the request") {
		t.Fatalf("expanded reasoning view = %q", view)
	}
}

func TestApplyRuntimeEventShowsPreparingTool(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.ModelToolCallDeltaPayload{ToolName: "run_cmd", DeltaBytes: 12})

	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 7, Kind: runtimeevent.KindModelToolCallDelta, Payload: payload})
	block, ok := idx.Get("stream-7-1")
	if !ok {
		t.Fatal("missing streaming block")
	}
	if block.PreparingTool != "run_cmd" || block.Done {
		t.Fatalf("stream block = %#v", block)
	}

	view := Model{theme: NoColorTheme()}.renderBlock(block)
	if !strings.Contains(view, "Preparing tool: run_cmd...") {
		t.Fatalf("renderBlock() = %q", view)
	}
}

func TestLifecycleEventsDoNotSpamTranscript(t *testing.T) {
	idx := NewBlockIndex()
	if got := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Kind: runtimeevent.KindRunCancelRequested}); got != "" {
		t.Fatalf("cancel request produced block %q", got)
	}
	if got := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 2, Kind: runtimeevent.KindRunCompleted}); got != "" {
		t.Fatalf("run completed produced block %q", got)
	}
	if len(idx.Order) != 0 {
		t.Fatalf("lifecycle events added blocks: %v", idx.Order)
	}
}

func TestRunCancelledMarksBlocksAndAddsSingleNotice(t *testing.T) {
	idx := NewBlockIndex()
	delta := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "partial"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Turn: 3, Kind: runtimeevent.KindModelTextDelta, Payload: delta})

	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 2, Kind: runtimeevent.KindRunCancelled})
	stream, ok := idx.Get("stream-3-1")
	if !ok || !stream.Done || stream.Status != "cancelled" {
		t.Fatalf("stream block after cancel = %#v, exists=%v", stream, ok)
	}
	notice, ok := idx.Get("notice-2")
	if !ok || notice.Content != "Turn cancelled" || notice.Status != "cancelled" {
		t.Fatalf("cancel notice = %#v, exists=%v", notice, ok)
	}
}

func TestTurnFinishedWithErrorAddsNotice(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.TurnFinishedPayload{Error: "persist user message: disk full"})
	if got := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 9, Kind: runtimeevent.KindTurnFinished, Payload: payload}); got != "notice-9" {
		t.Fatalf("turn finished error block = %q", got)
	}
	notice := idx.ByID["notice-9"]
	if notice.Status != "error" || !strings.Contains(notice.Content, "disk full") {
		t.Fatalf("error notice = %#v", notice)
	}

	// A clean finish adds nothing.
	if got := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 10, Kind: runtimeevent.KindTurnFinished}); got != "" {
		t.Fatalf("clean turn finish produced block %q", got)
	}
}

func TestToolCompletedMergesErrorAndDuration(t *testing.T) {
	idx := NewBlockIndex()
	callID := domain.NewToolCallID()
	prepared := mustPayload(t, runtimeevent.ToolPreparedPayload{CallID: callID, ToolName: "run_cmd", Risk: domain.R2})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Kind: runtimeevent.KindToolPrepared, Payload: prepared})

	finishedAt := time.Date(2026, 7, 24, 12, 0, 0, 0, time.UTC)
	completed := mustPayload(t, runtimeevent.ToolCompletedPayload{
		CallID: callID, ToolName: "run_cmd", Status: domain.ToolStatusError,
		Error: "exit_code_1", DurationMs: 42, FinishedAt: finishedAt,
	})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Kind: runtimeevent.KindToolCompleted, Payload: completed})

	block, ok := idx.Get("tool-" + callID.String())
	if !ok {
		t.Fatal("missing tool block")
	}
	if block.Status != "error" {
		t.Fatalf("status = %q, want error", block.Status)
	}
	if !strings.Contains(block.Detail, "exit_code_1") || !strings.Contains(block.Detail, "42ms") {
		t.Fatalf("detail = %q, want error code and duration", block.Detail)
	}
	if !block.FinishedAt.Equal(finishedAt) {
		t.Fatalf("FinishedAt = %v, want %v", block.FinishedAt, finishedAt)
	}
}

func TestModelResponseCompletedCorrectsDraftWithCanonicalText(t *testing.T) {
	idx := NewBlockIndex()
	// Simulate a lost delta: the draft is missing the tail of the message.
	delta := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "The answer is 4"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: delta})

	completed := mustPayload(t, runtimeevent.ModelResponseCompletedPayload{Text: "The answer is 42."})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 2, Turn: 1, Kind: runtimeevent.KindModelResponseCompleted, Payload: completed})

	block, ok := idx.Get("stream-1-1")
	if !ok || !block.Done {
		t.Fatalf("stream block = %#v, exists=%v", block, ok)
	}
	if block.Content != "The answer is 42." {
		t.Fatalf("content = %q, want canonical correction", block.Content)
	}
}

func TestModelResponseCompletedCreatesBlockWhenAllDeltasLost(t *testing.T) {
	idx := NewBlockIndex()
	completed := mustPayload(t, runtimeevent.ModelResponseCompletedPayload{Text: "recovered from store"})
	if got := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 5, Turn: 1, Kind: runtimeevent.KindModelResponseCompleted, Payload: completed}); got != "final-5" {
		t.Fatalf("final block ID = %q", got)
	}
	block, ok := idx.Get("final-5")
	if !ok || !block.Done || block.Content != "recovered from store" {
		t.Fatalf("final block = %#v, exists=%v", block, ok)
	}
}

func TestApplyRuntimeEventContextCompacted(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.ContextCompactedPayload{
		MaskedOutputs:   3,
		MaskedBytes:     48_000,
		EstTokensBefore: 182_000,
		EstTokensAfter:  41_000,
	})
	id := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 7, Kind: runtimeevent.KindContextCompacted, Payload: payload})
	if id == "" {
		t.Fatal("expected a notice block for context compaction")
	}
	block, ok := idx.Get(id)
	if !ok || block.Kind != BlockKindNotice {
		t.Fatalf("block = %#v, want notice", block)
	}
	want := "Context compacted: ~182k → ~41k tokens (3 outputs externalized) — long sessions with repeated compactions can reduce accuracy; consider a fresh session for new topics"
	if block.Content != want {
		t.Fatalf("content = %q, want %q", block.Content, want)
	}
}

func TestApplyRuntimeEventBudgetNotice(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.BudgetNoticePayload{
		Text:   "run budget exhausted (wall_time); the model is wrapping up with a final summary",
		WrapUp: true, Dimension: "wall_time",
	})
	id := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 9, Kind: runtimeevent.KindBudgetNotice, Payload: payload})
	if id == "" {
		t.Fatal("expected a notice block for the wrap-up notice")
	}
	block, ok := idx.Get(id)
	if !ok || block.Kind != BlockKindNotice {
		t.Fatalf("block = %#v, want notice", block)
	}
	if block.Status != "error" {
		t.Fatalf("wrap-up notice status = %q, want error", block.Status)
	}
	if !strings.Contains(block.Content, "wrapping up") {
		t.Fatalf("content = %q", block.Content)
	}
}

func TestPlanPanelRendersChecklistAndHides(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	m.plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "read code", Status: domain.PlanItemDone},
		{Index: 1, Goal: "implement feature", Status: domain.PlanItemInProgress},
		{Index: 2, Goal: "add tests", Status: domain.PlanItemTodo},
	}}

	panel := m.renderPlanPanel()
	// The default glyph set is Nerd Font (see icons_test.go); the plain set
	// is covered below.
	for _, want := range []string{"\uf046 read code", "\uf0c8 implement feature", "\uf096 add tests"} {
		if !strings.Contains(panel, want) {
			t.Fatalf("panel missing %q:\n%s", want, panel)
		}
	}
	m.SetIcons(PlainIcons())
	plainPanel := m.renderPlanPanel()
	for _, want := range []string{"[x] read code", "[>] implement feature", "[ ] add tests"} {
		if !strings.Contains(plainPanel, want) {
			t.Fatalf("plain panel missing %q:\n%s", want, plainPanel)
		}
	}
	m.SetIcons(NerdIcons())
	// Without a plan title the title row falls back to the progress summary.
	if !strings.Contains(panel, "plan · 1/3 done") {
		t.Fatalf("title row must show the progress summary:\n%s", panel)
	}
	// Steps indent two columns under the title, the first row carries the
	// tree stub, and every mark glyph lands on the same column.
	if !strings.Contains(panel, "└ \uf046 read code") || !strings.Contains(panel, "\n  \uf0c8 implement feature") {
		t.Fatalf("steps must indent under the title:\n%s", panel)
	}
	// Blank rows on both sides keep the panel from gluing to the transcript
	// above and the composer below.
	if !strings.HasPrefix(panel, "\n") || !strings.HasSuffix(panel, "\n") {
		t.Fatalf("panel must carry a blank row on each side: %q", panel)
	}
	if h := m.planPanelHeight(); h != 6 {
		t.Fatalf("planPanelHeight = %d, want 6 (blank + title + 3 items + blank)", h)
	}

	// A model-authored plan title replaces the progress summary — even while
	// the agent is busy (the title names the whole plan, not the activity).
	m.phase = "tools"
	m.activityLabel = "Reading plan.go"
	m.lastActivityAt = time.Now()
	m.plan.Title = "loom 架构梳理"
	titledPanel := m.renderPlanPanel()
	if !strings.Contains(titledPanel, "loom 架构梳理") {
		t.Fatalf("title row must carry the plan title:\n%s", titledPanel)
	}
	if strings.Contains(titledPanel, "plan · 1/3 done") || strings.Contains(titledPanel, "Reading plan.go") {
		t.Fatalf("title row must show only the plan title:\n%s", titledPanel)
	}

	// ctrl+t collapses the panel; the status bar segment keeps the progress.
	m.planHidden = true
	if panel := m.renderPlanPanel(); panel != "" {
		t.Fatalf("hidden panel rendered: %q", panel)
	}
	if h := m.planPanelHeight(); h != 0 {
		t.Fatalf("hidden planPanelHeight = %d, want 0", h)
	}
	bar := m.renderStatusBar()
	if !strings.Contains(bar, "plan:1/3") {
		t.Fatalf("status bar must keep plan progress while the panel is hidden: %q", bar)
	}
}

// Regression (REVIEW M14): every tea.Msg used to trigger a full
// transcript rebuild — keystrokes and spinner ticks included, each an
// O(transcript) string rebuild + viewport reset. Idle messages must not
// rebuild; block-mutating events still must.
func TestUpdateSkipsTranscriptRebuildWhenNothingChanged(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")

	updated, _ := m.Update(tea.WindowSizeMsg{Width: 100, Height: 30})
	m = updated.(Model)
	baseline := m.transcriptBuilds
	if baseline == 0 {
		t.Fatal("initial layout must build the transcript once")
	}

	// Keystrokes only touch the composer, never the transcript.
	for range 8 {
		updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'x'}})
		m = updated.(Model)
	}
	if m.transcriptBuilds != baseline {
		t.Fatalf("idle keystrokes rebuilt the transcript: builds = %d, want %d", m.transcriptBuilds, baseline)
	}

	// A runtime event that adds a block must still trigger a rebuild.
	updated, _ = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{
		Kind:    runtimeevent.KindTurnStarted,
		Payload: []byte(`{"prompt":"hello"}`),
	}))
	m = updated.(Model)
	if m.transcriptBuilds <= baseline {
		t.Fatalf("block-mutating event did not rebuild: builds = %d, baseline %d", m.transcriptBuilds, baseline)
	}
}

// Regression (REVIEW M12a): the resubscribe counter never reset, so a
// long-lived session that recovered from three disconnects DAYS apart was
// locked out forever. A delivered event proves the stream is healthy and
// must re-arm the recovery budget.
func TestRuntimeEventResetsResubscribeBudget(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	m.resubscribes = maxEventResubscribes - 1

	updated, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnStarted})
	m = updated
	if m.resubscribes != 0 {
		t.Fatalf("resubscribes = %d, want 0 after a delivered event", m.resubscribes)
	}
}

// Regression (REVIEW M12b): resubscribing used to drop the unsubscribe
// handle, leaking every recovered broker subscription until process exit.
func TestHandleEventsClosedReleasesOldSubscription(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	released := 0
	m.unsubscribeEvents = func() { released++ }

	next, _ := m.handleEventsClosed()
	if released != 1 {
		t.Fatalf("old subscription released %d times, want 1", released)
	}
	nm, ok := next.(Model)
	if !ok {
		t.Fatalf("handleEventsClosed returned %T, want Model", next)
	}
	if nm.unsubscribeEvents == nil {
		t.Fatal("new subscription's unsubscribe must be stored")
	}
	if nm.resubscribes != 1 {
		t.Fatalf("resubscribes = %d, want 1", nm.resubscribes)
	}
}

func TestTurnStartedClearsPlanPanel(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	m.plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "read code", Status: domain.PlanItemDone},
		{Index: 1, Goal: "implement feature", Status: domain.PlanItemDone},
	}}
	m.width = 100
	if m.renderPlanPanel() == "" {
		t.Fatal("panel must render while a plan is present")
	}

	// A new turn starts the display fresh; the next plan revision brings the
	// panel back.
	updated, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnStarted})
	m = updated
	if len(m.plan.Items) != 0 {
		t.Fatalf("plan must clear on turn start: %+v", m.plan)
	}
	if m.renderPlanPanel() != "" {
		t.Fatal("panel must hide on turn start")
	}
}

func TestPlanPanelCollapsesLongPlans(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	items := make([]domain.PlanItem, 0, 9)
	for i := 0; i < 9; i++ {
		items = append(items, domain.PlanItem{Index: i, Goal: fmt.Sprintf("step %d", i+1), Status: domain.PlanItemTodo})
	}
	m.plan = domain.Plan{Items: items}

	panel := m.renderPlanPanel()
	if !strings.Contains(panel, "… +3 more") {
		t.Fatalf("long plan must collapse into a +N line:\n%s", panel)
	}
	if h, want := m.planPanelHeight(), planPanelMaxItems+1+3; h != want {
		t.Fatalf("planPanelHeight = %d, want %d", h, want)
	}
}

func TestApprovalOverlayShowsAlwaysAllowWithRulePreview(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID:  domain.NewEventID(),
		CallID:      domain.NewToolCallID(),
		ToolName:    "run_cmd",
		Risk:        domain.R2,
		Description: "Run 'go' 'test' './...'",
		ArgsHash:    "abc",
		Arguments:   json.RawMessage(`{"program":"go","args":["test","./..."]}`),
	}
	m.approvalCursor = 0

	overlay := m.renderApprovalOverlay()
	for _, want := range []string{"Allow once", "Always allow `go test`", "Deny", "y/a/n"} {
		if !strings.Contains(overlay, want) {
			t.Fatalf("overlay missing %q:\n%s", want, overlay)
		}
	}

	// Left/right cycles through the three options; Enter on cursor 1 remembers.
	for i, key := range []tea.KeyMsg{{Type: tea.KeyRight}, {Type: tea.KeyRight}, {Type: tea.KeyLeft}} {
		updated, _ := m.handleApprovalKey(key)
		m = updated.(Model)
		want := []int{1, 2, 1}[i]
		if m.approvalCursor != want {
			t.Fatalf("cursor = %d, want %d after key %v", m.approvalCursor, want, key.Type)
		}
	}
	// j/k navigate the same options vim-style.
	for i, key := range []tea.KeyMsg{{Type: tea.KeyRunes, Runes: []rune{'j'}}, {Type: tea.KeyRunes, Runes: []rune{'k'}}} {
		updated, _ := m.handleApprovalKey(key)
		m = updated.(Model)
		want := []int{2, 1}[i]
		if m.approvalCursor != want {
			t.Fatalf("cursor = %d, want %d after key %q", m.approvalCursor, want, key.String())
		}
	}
	if m.pendingApproval == nil {
		t.Fatal("approval must still be pending after navigation keys")
	}
}

func TestApprovalDecisionGuardIgnoresEarlyKeys(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100, mode: ModeApproval}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		CallID:     domain.NewToolCallID(),
		ToolName:   "web_fetch",
		Risk:       domain.R3,
	}
	m.approvalShownAt = time.Now()

	// A key-repeat or double tap right after the overlay appears must not
	// resolve the approval: no decision command, overlay stays up.
	for _, key := range []tea.KeyMsg{
		{Type: tea.KeyRunes, Runes: []rune{'y'}},
		{Type: tea.KeyEnter},
		{Type: tea.KeyRunes, Runes: []rune{'n'}},
	} {
		updated, cmd := m.handleApprovalKey(key)
		m = updated.(Model)
		if cmd != nil || m.pendingApproval == nil || m.mode != ModeApproval {
			t.Fatalf("early key %v resolved the approval; guard should ignore it", key.Type)
		}
	}

	// Navigation stays responsive during the guard window.
	updated, _ := m.handleApprovalKey(tea.KeyMsg{Type: tea.KeyRight})
	m = updated.(Model)
	if m.approvalCursor != 1 {
		t.Fatalf("navigation key blocked by guard: cursor = %d, want 1", m.approvalCursor)
	}

	// After the guard window the same key resolves normally.
	m.approvalShownAt = time.Now().Add(-2 * approvalDecisionGuard)
	updated, cmd := m.handleApprovalKey(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'y'}})
	m = updated.(Model)
	if cmd == nil || m.pendingApproval != nil {
		t.Fatal("decision key after the guard window must resolve the approval")
	}
}

func TestApprovalOverlayHidesRulePreviewForShell(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		ToolName:   "run_cmd",
		Risk:       domain.R3,
		Arguments:  json.RawMessage(`{"program":"sh","args":["-c","echo hi | cat"]}`),
	}
	overlay := m.renderApprovalOverlay()
	if !strings.Contains(overlay, "Always allow") {
		t.Fatalf("overlay should still offer the always option: %s", overlay)
	}
	if strings.Contains(overlay, "Always allow `") {
		t.Fatalf("compound shell calls must not show a rule preview: %s", overlay)
	}
}

func TestFormatContext(t *testing.T) {
	tests := []struct {
		name          string
		est           int
		lastCallInput int64
		window        int
		wantLabel     string
		wantWarn      bool
	}{
		{name: "estimate without window", est: 41_000, wantLabel: "ctx:~41k"},
		{name: "estimate with window", est: 41_000, window: 128_000, wantLabel: "ctx:~41k/128k"},
		{name: "fallback to provider value", lastCallInput: 12_345, wantLabel: "ctx:~12k"},
		{name: "warning at 80 percent", est: 103_000, window: 128_000, wantLabel: "ctx:~103k/128k", wantWarn: true},
		{name: "nothing known", wantLabel: ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			label, warn := formatContext(tt.est, tt.lastCallInput, tt.window)
			if label != tt.wantLabel || warn != tt.wantWarn {
				t.Fatalf("formatContext(%d, %d, %d) = (%q, %v), want (%q, %v)",
					tt.est, tt.lastCallInput, tt.window, label, warn, tt.wantLabel, tt.wantWarn)
			}
		})
	}
}

func TestContextUsageEventDrivesStatusBar(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	m.theme = NoColorTheme()
	m.width = 140
	m.SetContextWindow(128_000)

	payload := mustPayload(t, runtimeevent.ContextUsagePayload{EstTokens: 41_000, LastCallInputTokens: 39_500})
	updated, _ := m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Sequence: 1, Kind: runtimeevent.KindContextUsage, Payload: payload}))
	m = updated.(Model)

	if m.contextEst != 41_000 || m.lastCallInput != 39_500 {
		t.Fatalf("context fields = (%d, %d), want (41000, 39500)", m.contextEst, m.lastCallInput)
	}
	if bar := m.renderStatusBar(); !strings.Contains(bar, "ctx:~41k/128k") {
		t.Fatalf("status bar missing ctx segment: %q", bar)
	}

	opened := mustPayload(t, runtimeevent.SessionOpenedPayload{Model: "test-model", Workspace: "/ws"})
	updated, _ = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Sequence: 2, Kind: runtimeevent.KindSessionOpened, Payload: opened}))
	m = updated.(Model)
	if m.contextEst != 0 || m.lastCallInput != 0 {
		t.Fatalf("session open must reset context occupancy: (%d, %d)", m.contextEst, m.lastCallInput)
	}
}

func TestCompactionCounterAndStatusBar(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	m.theme = NoColorTheme()
	m.width = 120

	payload := mustPayload(t, runtimeevent.ContextCompactedPayload{
		MaskedOutputs:    3,
		MaskedBytes:      48_000,
		ArchivedMessages: 27,
		EstTokensBefore:  182_000,
		EstTokensAfter:   41_000,
	})
	updated, _ := m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Sequence: 1, Kind: runtimeevent.KindContextCompacted, Payload: payload}))
	m = updated.(Model)

	if m.compactions != 1 {
		t.Fatalf("compactions = %d, want 1", m.compactions)
	}
	if !strings.Contains(m.statusMessage, "Context compacted ~182k → ~41k tokens") {
		t.Fatalf("statusMessage = %q, want compaction summary", m.statusMessage)
	}
	if bar := m.renderStatusBar(); !strings.Contains(bar, "compact:1") {
		t.Fatalf("status bar missing compaction tally: %q", bar)
	}

	// A new session view resets the tally.
	opened := mustPayload(t, runtimeevent.SessionOpenedPayload{Model: "test-model", Workspace: "/ws"})
	updated, _ = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Sequence: 2, Kind: runtimeevent.KindSessionOpened, Payload: opened}))
	m = updated.(Model)
	if m.compactions != 0 {
		t.Fatalf("compactions after session open = %d, want 0", m.compactions)
	}
}

func TestRenderStatusBarShowsActivityAndToolUsage(t *testing.T) {
	m := Model{
		theme:          NoColorTheme(),
		phase:          "tool",
		usage:          domain.Usage{Turns: 2, InputTokens: 11, OutputTokens: 22, ToolCalls: 3},
		activityLabel:  "Running tool: bazel",
		lastActivityAt: time.Now(),
	}
	status := m.renderStatusBar()
	for _, expected := range []string{"[tool]", "tools:3", "Running tool: bazel"} {
		if !strings.Contains(status, expected) {
			t.Fatalf("renderStatusBar() = %q, missing %q", status, expected)
		}
	}
}

func TestRenderStatusBarDropsSegmentsOnNarrowScreens(t *testing.T) {
	m := Model{
		theme:          NoColorTheme(),
		width:          20,
		phase:          "tool",
		usage:          domain.Usage{Turns: 2, InputTokens: 11, OutputTokens: 22, ToolCalls: 3},
		activityLabel:  "Running tool: bazel",
		lastActivityAt: time.Now(),
		statusMessage:  "some very long status message",
	}
	status := m.renderStatusBar()
	if utf8.RuneCountInString(status) > 40 {
		t.Fatalf("narrow status bar = %q (too long)", status)
	}
	if !strings.Contains(status, "[tool]") {
		t.Fatalf("narrow status bar lost phase: %q", status)
	}
}

func TestFormatUsage(t *testing.T) {
	tests := []struct {
		name  string
		usage domain.Usage
		want  string
	}{
		{
			name:  "counters without budget denominators",
			usage: domain.Usage{Turns: 17, InputTokens: 212456, OutputTokens: 6095, ToolCalls: 34},
			want:  "turns:17 in:212k out:6.1k tools:34",
		},
		{
			name:  "zero usage",
			usage: domain.Usage{Turns: 1, InputTokens: 500, OutputTokens: 50},
			want:  "turns:1 in:500 out:50 tools:0",
		},
		{
			name:  "million scale",
			usage: domain.Usage{InputTokens: 2_500_000, OutputTokens: 999},
			want:  "turns:0 in:2.5M out:999 tools:0",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := formatUsage(tt.usage); got != tt.want {
				t.Fatalf("formatUsage() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestBudgetUsageRatio(t *testing.T) {
	if got := budgetUsageRatio(domain.Usage{InputTokens: 1 << 40}, domain.Limits{}); got != 0 {
		t.Fatalf("ratio with zero budgets = %v, want 0", got)
	}
	// Session tokens dominate; per-prompt wall time never factors in.
	limits := domain.Limits{MaxTokens: 100_000, MaxEstimatedCostUSD: 5.0}
	usage := domain.Usage{InputTokens: 80_000, OutputTokens: 10_000, CostUSD: 1.0, WallTime: 999 * time.Hour}
	if got := budgetUsageRatio(usage, limits); got < 0.89 || got > 0.91 {
		t.Fatalf("ratio = %v, want ~0.9 (tokens), got %v", got, usage)
	}
	// Cost dominates when closer to its limit.
	usage.CostUSD = 4.9
	if got := budgetUsageRatio(usage, limits); got < 0.97 {
		t.Fatalf("ratio = %v, want ~0.98 (cost)", got)
	}
}

func TestHumanizeTokens(t *testing.T) {
	tests := []struct {
		n    int64
		want string
	}{
		{0, "0"},
		{999, "999"},
		{1000, "1.0k"},
		{6095, "6.1k"},
		{10000, "10k"},
		{212456, "212k"},
		{999999, "999k"},
		{1_000_000, "1.0M"},
		{2_500_000, "2.5M"},
	}
	for _, tt := range tests {
		if got := humanizeTokens(tt.n); got != tt.want {
			t.Errorf("humanizeTokens(%d) = %q, want %q", tt.n, got, tt.want)
		}
	}
}

func TestInitialSnapshotDoesNotDiscardCompletedRealtimeTurn(t *testing.T) {
	idx := NewBlockIndex()
	pendingID := idx.AddPendingUserBlock("hello")
	turnPayload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "hello"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Turn: 1, Kind: runtimeevent.KindTurnStarted, Payload: turnPayload})
	textPayload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "Hi there"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 2, Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: textPayload})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 3, Turn: 1, Kind: runtimeevent.KindModelResponseCompleted})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 4, Turn: 1, Kind: runtimeevent.KindRunCompleted})

	persistedUser := domain.Message{
		ID:    domain.NewMessageID(),
		Role:  domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}},
	}
	m := Model{blocks: idx, initialSnapshotPending: true}
	updated, _ := m.handleSnapshot(snapshotMsg{snapshot: app.Snapshot{Messages: []domain.Message{persistedUser}}})
	m = updated.(Model)

	for _, id := range []string{pendingID, "stream-1-1"} {
		if _, ok := m.blocks.Get(id); !ok {
			t.Fatalf("snapshot discarded realtime block %q; order=%v", id, m.blocks.Order)
		}
	}
}

func TestHandleSnapshotPreservesRealtimeFirstPrompt(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "first prompt"})
	id := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 1, Kind: runtimeevent.KindTurnStarted, Payload: payload})
	m := Model{blocks: idx, initialSnapshotPending: true}
	updated, _ := m.handleSnapshot(snapshotMsg{snapshot: app.Snapshot{}})
	m = updated.(Model)
	if block, ok := m.blocks.Get(id); !ok || block.Content != "first prompt" {
		t.Fatalf("first prompt block = %#v, exists=%v", block, ok)
	}
}

func TestHandleSnapshotMergesRealtimePromptWithPersistedTranscript(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 2, Prompt: "second prompt"})
	id := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 2, Kind: runtimeevent.KindTurnStarted, Payload: payload})
	persisted := domain.Message{
		ID:    domain.NewMessageID(),
		Role:  domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "first prompt"}},
	}
	m := Model{blocks: idx}
	updated, _ := m.handleSnapshot(snapshotMsg{snapshot: app.Snapshot{Messages: []domain.Message{persisted}}})
	m = updated.(Model)
	if len(m.blocks.Order) != 2 {
		t.Fatalf("block count = %d, want 2", len(m.blocks.Order))
	}
	if block, ok := m.blocks.Get(id); !ok || block.Content != "second prompt" {
		t.Fatalf("second prompt block = %#v, exists=%v", block, ok)
	}
}

func TestSnapshotDismissesStaleApproval(t *testing.T) {
	m := Model{blocks: NewBlockIndex(), mode: ModeApproval}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		CallID:     domain.NewToolCallID(),
	}
	updated, _ := m.handleSnapshot(snapshotMsg{snapshot: app.Snapshot{}})
	m = updated.(Model)
	if m.pendingApproval != nil || m.mode != ModeChat {
		t.Fatalf("stale approval survived snapshot: pending=%v mode=%s", m.pendingApproval, m.mode)
	}
}

func TestRebuildTranscript(t *testing.T) {
	messages := []domain.Message{
		{ID: domain.NewMessageID(), Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "question"}}},
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Status: domain.MessageStatusInterrupted, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "partial"}}},
	}
	idx := RebuildTranscript(messages)
	if len(idx.Order) != 2 {
		t.Fatalf("block count = %d, want 2", len(idx.Order))
	}
	if got := idx.ByID[idx.Order[0]]; got.Kind != BlockKindUser || got.Content != "question" {
		t.Fatalf("user block = %#v", got)
	}
	if got := idx.ByID[idx.Order[1]]; got.Kind != BlockKindInterrupted || got.Content != "partial" {
		t.Fatalf("assistant block = %#v", got)
	}
}

func TestRebuildTranscriptRestoresToolHistory(t *testing.T) {
	callID := domain.NewToolCallID()
	orphanCallID := domain.NewToolCallID()
	startedAt := time.Unix(1000, 0).UTC()
	messages := []domain.Message{
		{ID: domain.NewMessageID(), Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "run it"}}},
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartText, Text: "running"},
			{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: callID, Name: "run_cmd", Arguments: []byte(`{}`)}},
			{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: orphanCallID, Name: "apply_patch", Arguments: []byte(`{}`)}},
		}},
		{ID: domain.NewMessageID(), Role: domain.RoleAssistant, Parts: []domain.ContentPart{
			{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
				CallID: callID, Status: domain.ToolStatusSuccess,
				StartedAt: startedAt, FinishedAt: startedAt.Add(5 * time.Millisecond),
			}},
		}},
	}
	idx := RebuildTranscript(messages)

	done, ok := idx.Get("tool-" + callID.String())
	if !ok {
		t.Fatalf("missing restored tool block; order=%v", idx.Order)
	}
	if done.Status != "success" || !strings.Contains(done.Detail, "5ms") {
		t.Fatalf("restored tool block = %#v", done)
	}

	orphan, ok := idx.Get("tool-" + orphanCallID.String())
	if !ok {
		t.Fatal("missing orphan tool block")
	}
	if orphan.Status != "cancelled" || !strings.Contains(orphan.Detail, "verify side effects") {
		t.Fatalf("orphan tool block = %#v", orphan)
	}
}

func TestTruncateDisplayWidthPreservesUTF8(t *testing.T) {
	got := truncateDisplayWidth("你好世界", 5)
	if got != "你..." {
		t.Fatalf("truncateDisplayWidth() = %q, want %q", got, "你...")
	}
	if !utf8.ValidString(got) {
		t.Fatalf("truncateDisplayWidth() returned invalid UTF-8: %q", got)
	}
}

func TestShortenPath(t *testing.T) {
	cases := map[string]string{
		"~/workspace/github/liubang/playground": "~/w/g/l/playground",
		"/usr/local/bin":                        "/u/l/bin",
		"relative/path/here":                    "r/p/here",
		"~/.config/loom/state":                  "~/.c/l/state",
		"/":                                     "/",
		"~":                                     "~",
		"playground":                            "playground",
		"":                                      "",
	}
	for in, want := range cases {
		if got := shortenPath(in); got != want {
			t.Errorf("shortenPath(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestAbbreviateHome(t *testing.T) {
	home := string(filepath.Separator) + filepath.Join("home", "tester")
	t.Setenv("HOME", home)
	cases := map[string]string{
		home:                    "~",
		home + "/ws/playground": "~/ws/playground",
		"/other/place":          "/other/place",
		home + "ish/sibling":    home + "ish/sibling", // prefix must match a whole component
		"relative/path":         "relative/path",
	}
	for in, want := range cases {
		if got := abbreviateHome(in); got != want {
			t.Errorf("abbreviateHome(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestSessionFinderSelection(t *testing.T) {
	first, second := domain.NewSessionID(), domain.NewSessionID()
	host := Model{theme: NoColorTheme()}
	finder := host.NewSessionFinder()
	finder.Load(sessionFinderItems([]app.SessionSummary{{ID: first}, {ID: second}}), nil)
	finder.MoveDown()
	if got := finder.Selected(); got == nil || got.ID != second {
		t.Fatalf("Selected() = %+v, want %s", got, second)
	}
}

func TestSessionFinderModalNavigation(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	m.mode = ModeSessionPicker
	m.sessionFinder = m.NewSessionFinder()
	first, second := domain.NewSessionID(), domain.NewSessionID()
	m.sessionFinder.Load(sessionFinderItems([]app.SessionSummary{{ID: first}, {ID: second}}), nil)

	// Insert mode (the default): j types into the fuzzy filter instead of
	// navigating.
	updatedModel, cmd := m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'j'}})
	m = updatedModel.(Model)
	if cmd != nil {
		t.Fatal("typing should not spawn a command")
	}
	if got := m.sessionFinder.Query(); got != "j" {
		t.Fatalf("query = %q, want j (insert mode types into the filter)", got)
	}

	// Esc enters normal mode; j/k navigate there.
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if !m.sessionFinder.Normal() {
		t.Fatal("Esc should enter normal mode")
	}
	// The filter is still active; clear it so both rows are visible.
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'i'}})
	m = updatedModel.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyBackspace})
	m = updatedModel.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)

	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'j'}})
	m = updatedModel.(Model)
	if got := m.sessionFinder.Selected(); got == nil || got.ID != second {
		t.Fatalf("Selected() after j = %+v, want %s", got, second)
	}
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'k'}})
	m = updatedModel.(Model)
	if got := m.sessionFinder.Selected(); got == nil || got.ID != first {
		t.Fatalf("Selected() after k = %+v, want %s", got, first)
	}

	// q closes the picker from normal mode.
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'q'}})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode after q = %s, want chat", m.mode)
	}
}

func TestSessionFinderWindowsLongLists(t *testing.T) {
	host := Model{theme: NoColorTheme()}
	finder := host.NewSessionFinder()
	var summaries []app.SessionSummary
	for i := 0; i < 20; i++ {
		summaries = append(summaries, app.SessionSummary{ID: domain.NewSessionID(), UpdatedAt: time.Now()})
	}
	finder.Load(sessionFinderItems(summaries), nil)
	for i := 0; i < 15; i++ {
		finder.MoveDown()
	}
	rendered := finder.Render(80, 8)
	if !strings.Contains(rendered, "↑ more") {
		t.Fatalf("windowed finder missing upward hint:\n%s", rendered)
	}
	if strings.Count(rendered, "sess_") > 10 {
		t.Fatalf("windowed finder rendered too many rows:\n%s", rendered)
	}
}

func TestComposerTypingIsNotHijackedByShortcuts(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	// Seed a reasoning block: previously this made plain "r" untypable.
	reasoning := mustPayload(t, runtimeevent.ModelReasoningDeltaPayload{Delta: "thinking"})
	ApplyRuntimeEvent(m.blocks, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelReasoningDelta, Payload: reasoning})

	for _, runes := range []string{"G", "r"} {
		updated, _ := m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(runes)})
		m = updated.(Model)
	}
	if got := m.textArea.Value(); got != "Gr" {
		t.Fatalf("composer value = %q, want %q (shortcut keys must type normally)", got, "Gr")
	}
}

func TestEnterSubmitsAndAltEnterInsertsNewline(t *testing.T) {
	ctrl := newTestController(t)

	m := NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("hello loom")
	updated, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("Enter did not issue a submit command")
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("composer not cleared after submit: %q", got)
	}
	if len(m.blocks.Order) != 1 {
		t.Fatalf("optimistic user block missing: %v", m.blocks.Order)
	}

	// Alt+Enter must not submit; it inserts a newline through the composer keymap.
	m2 := NewModel(ctrl, "model", "/ws")
	updated, _ = m2.Update(tea.KeyMsg{Type: tea.KeyEnter, Alt: true})
	m2 = updated.(Model)
	if !strings.Contains(m2.textArea.Value(), "\n") {
		t.Fatalf("Alt+Enter did not insert a newline: %q", m2.textArea.Value())
	}
	if len(m2.blocks.Order) != 0 {
		t.Fatalf("Alt+Enter must not submit; blocks=%v", m2.blocks.Order)
	}
}

func TestCtrlCStateTable(t *testing.T) {
	// booting/fatal/closed: Ctrl+C quits immediately.
	booting := app.NewController(app.ControllerConfig{
		Broker:   runtimeevent.NewBroker(),
		Approver: app.NewChannelApprover(),
	})
	m := NewModel(booting, "model", "/ws")
	if _, cmd := m.Update(tea.KeyMsg{Type: tea.KeyCtrlC}); !isQuitCmd(cmd) {
		t.Fatal("Ctrl+C in booting state did not quit")
	}

	// idle with empty input: first Ctrl+C arms the confirm, second quits.
	ctrl := newTestController(t)
	m = NewModel(ctrl, "model", "/ws")
	updated, cmd := m.Update(tea.KeyMsg{Type: tea.KeyCtrlC})
	m = updated.(Model)
	if cmd != nil || !m.quitConfirm {
		t.Fatalf("first idle Ctrl+C should arm quit confirm, cmd=%v confirm=%v", cmd, m.quitConfirm)
	}
	if _, cmd := m.Update(tea.KeyMsg{Type: tea.KeyCtrlC}); !isQuitCmd(cmd) {
		t.Fatal("second idle Ctrl+C did not quit")
	}

	// idle with a draft: Ctrl+C clears the draft instead of quitting.
	m = NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("draft")
	updated, cmd = m.Update(tea.KeyMsg{Type: tea.KeyCtrlC})
	m = updated.(Model)
	if cmd != nil || m.textArea.Value() != "" {
		t.Fatalf("Ctrl+C with draft should clear input, value=%q cmd=%v", m.textArea.Value(), cmd)
	}
}

func TestSlashCommandFailurePreservesInput(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("/frobnicate")
	updated, _ := m.handleSlashCommand("/frobnicate")
	m = updated.(Model)
	if got := m.textArea.Value(); got != "/frobnicate" {
		t.Fatalf("unknown command should keep the draft, got %q", got)
	}
	if !m.statusIsError {
		t.Fatal("unknown command should be flagged as an error status")
	}
}

func TestSlashCommandModel(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model-a", "/ws")

	// No argument: report the current model and clear the draft.
	m.textArea.SetValue("/model")
	updated, cmd := m.handleSlashCommand("/model")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("bare /model should not spawn a command")
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("bare /model should clear the draft, got %q", got)
	}
	if !strings.Contains(m.statusMessage, "model-a") || m.statusIsError {
		t.Fatalf("status = %q (error=%v), want current model name", m.statusMessage, m.statusIsError)
	}

	// Switch: the ack arrives as a modelChangedMsg and updates the status bar.
	m.textArea.SetValue("/model model-b")
	updated, cmd = m.handleSlashCommand("/model model-b")
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("/model <name> should spawn a command")
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("/model <name> should clear the draft, got %q", got)
	}
	msg, ok := cmd().(modelChangedMsg)
	if !ok {
		t.Fatalf("cmd returned %T, want modelChangedMsg", msg)
	}
	if msg.err != nil {
		t.Fatalf("modelChangedMsg err = %v", msg.err)
	}
	updatedModel, _ := m.Update(msg)
	m = updatedModel.(Model)
	if got := m.modelName; got != "test/model-b" {
		t.Fatalf("modelName = %q, want test/model-b", got)
	}
	if got := m.contextWindow; got != 64000 {
		t.Fatalf("contextWindow = %d, want 64000 (from model metadata)", got)
	}
	if !strings.Contains(m.statusMessage, "test/model-b") || m.statusIsError {
		t.Fatalf("status = %q (error=%v), want switch confirmation", m.statusMessage, m.statusIsError)
	}

	// Extra arguments: usage error, draft preserved.
	m.textArea.SetValue("/model a b")
	updated, cmd = m.handleSlashCommand("/model a b")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("usage error should not spawn a command")
	}
	if got := m.textArea.Value(); got != "/model a b" {
		t.Fatalf("usage error should keep the draft, got %q", got)
	}
	if !m.statusIsError {
		t.Fatal("usage error should be flagged as an error status")
	}
}

func TestSlashCommandReasoning(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	// No argument: open the picker with the cursor on the active dial
	// ("default" while following the model's configuration).
	m.textArea.SetValue("/reasoning")
	updated, cmd := m.handleSlashCommand("/reasoning")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("bare /reasoning should not spawn a command")
	}
	if m.mode != ModeReasoningPicker {
		t.Fatalf("mode = %s, want reasoning_picker", m.mode)
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("bare /reasoning should clear the draft, got %q", got)
	}
	if m.reasoningFinder == nil || m.reasoningFinder.Selected().Arg != "default" {
		t.Fatalf("picker cursor = %+v, want default", m.reasoningFinder.Selected())
	}

	// Set an override: the ack arrives as a reasoningChangedMsg and the
	// header dial updates with the override marker.
	m.textArea.SetValue("/reasoning high")
	updated, cmd = m.handleSlashCommand("/reasoning high")
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("/reasoning <level> should spawn a command")
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("/reasoning <level> should clear the draft, got %q", got)
	}
	msg, ok := cmd().(reasoningChangedMsg)
	if !ok {
		t.Fatalf("cmd returned %T, want reasoningChangedMsg", msg)
	}
	if msg.err != nil {
		t.Fatalf("reasoningChangedMsg err = %v", msg.err)
	}
	updatedModel, _ := m.Update(msg)
	m = updatedModel.(Model)
	if got := m.reasoningEffort; got != "high" {
		t.Fatalf("reasoningEffort = %q, want high", got)
	}
	if !m.reasoningOverridden {
		t.Fatal("reasoningOverridden = false, want true after /reasoning high")
	}

	// Back to the model's configured default.
	m.textArea.SetValue("/reasoning default")
	updated, cmd = m.handleSlashCommand("/reasoning default")
	m = updated.(Model)
	msg, ok = cmd().(reasoningChangedMsg)
	if !ok || msg.err != nil {
		t.Fatalf("default cmd = %+v (ok=%v, err=%v)", msg, ok, msg.err)
	}
	updatedModel, _ = m.Update(msg)
	m = updatedModel.(Model)
	if m.reasoningEffort != "" || m.reasoningOverridden {
		t.Fatalf("dial = %q (overridden=%v), want provider default", m.reasoningEffort, m.reasoningOverridden)
	}

	// Unknown level: the controller rejects and the draft is restored.
	m.textArea.SetValue("/reasoning extreme")
	updated, cmd = m.handleSlashCommand("/reasoning extreme")
	m = updated.(Model)
	msg, ok = cmd().(reasoningChangedMsg)
	if !ok {
		t.Fatalf("cmd returned %T, want reasoningChangedMsg", msg)
	}
	if msg.err == nil {
		t.Fatal("unknown level should be rejected by the controller")
	}
	updatedModel, _ = m.Update(msg)
	m = updatedModel.(Model)
	if got := m.textArea.Value(); got != "/reasoning extreme" {
		t.Fatalf("rejected command should restore the draft, got %q", got)
	}
	if !m.statusIsError {
		t.Fatal("rejected command should be flagged as an error status")
	}

	// Extra arguments: usage error, no command spawned.
	m.textArea.SetValue("/reasoning high low")
	updated, cmd = m.handleSlashCommand("/reasoning high low")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("usage error should not spawn a command")
	}
	if got := m.textArea.Value(); got != "/reasoning high low" {
		t.Fatalf("usage error should keep the draft, got %q", got)
	}
}

func TestSlashCommandCompact(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	// Schedule: the ack confirms with the current context estimate.
	m.contextEst = 45200
	updated, cmd := m.handleSlashCommand("/compact")
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("/compact should spawn a command")
	}
	if got := m.textArea.Value(); got != "" {
		t.Fatalf("/compact should clear the draft, got %q", got)
	}
	msg, ok := cmd().(compactRequestedMsg)
	if !ok || msg.err != nil {
		t.Fatalf("cmd = %+v (ok=%v, err=%v)", msg, ok, msg.err)
	}
	updatedModel, _ := m.Update(msg)
	m = updatedModel.(Model)
	if !strings.Contains(m.statusMessage, "Will compact") || !strings.Contains(m.statusMessage, "45k") {
		t.Fatalf("status = %q, want scheduling confirmation with estimate", m.statusMessage)
	}

	// Duplicate: reported as already pending.
	updated, cmd = m.handleSlashCommand("/compact")
	m = updated.(Model)
	msg, ok = cmd().(compactRequestedMsg)
	if !ok || msg.err != nil {
		t.Fatalf("cmd = %+v (ok=%v, err=%v)", msg, ok, msg.err)
	}
	if !msg.result.AlreadyPending {
		t.Fatal("duplicate /compact should report AlreadyPending")
	}
	updatedModel, _ = m.Update(msg)
	m = updatedModel.(Model)
	if !strings.Contains(m.statusMessage, "already scheduled") {
		t.Fatalf("status = %q, want already-pending notice", m.statusMessage)
	}

	// Arguments: usage error, draft preserved.
	m.textArea.SetValue("/compact now")
	updated, cmd = m.handleSlashCommand("/compact now")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("usage error should not spawn a command")
	}
	if got := m.textArea.Value(); got != "/compact now" {
		t.Fatalf("usage error should keep the draft, got %q", got)
	}
}

func TestQuestionOverlayFlow(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	// A model question arrives: the overlay opens with the generic choice
	// list (checkbox mode, free-text row appended).
	questionID := domain.NewEventID()
	evt := runtimeevent.RuntimeEvent{
		Kind: runtimeevent.KindQuestionAsked,
		Payload: mustPayload(t, runtimeevent.QuestionAskedPayload{
			QuestionID: questionID,
			Text:       "which modules should the refactor cover?",
			Options: []domain.QuestionOption{
				{Label: "codec", Description: "most of the change"},
				{Label: "index", Description: "depends on codec"},
				{Label: "merge", Description: "can follow later"},
			},
			AllowMultiple: true,
		}),
	}
	updatedModel, _ := m.Update(runtimeEventMsg(evt))
	m = updatedModel.(Model)
	if m.mode != ModeQuestion {
		t.Fatalf("mode = %s, want question", m.mode)
	}
	if m.choiceList == nil || m.pendingQuestion == nil {
		t.Fatal("choice list not initialized")
	}
	// Skip the anti-fat-finger guard window so the keys below take effect.
	m.questionShownAt = time.Now().Add(-time.Second)

	// Toggle two options, then confirm: the answer goes to the controller.
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeySpace})
	m = updatedModel.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyDown})
	m = updatedModel.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyDown})
	m = updatedModel.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeySpace})
	m = updatedModel.(Model)
	updatedModel, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode = %s, want chat after confirm", m.mode)
	}
	if cmd == nil {
		t.Fatal("confirm should spawn the answer command")
	}
	msg, ok := cmd().(questionAnsweredMsg)
	if !ok {
		t.Fatalf("cmd returned %T, want questionAnsweredMsg", msg)
	}
	if msg.err != nil {
		t.Fatalf("answer delivery = %v", msg.err)
	}

	// A second question: Esc skips without touching the composer.
	evt.Payload = mustPayload(t, runtimeevent.QuestionAskedPayload{
		QuestionID: domain.NewEventID(),
		Text:       "proceed with the risky migration?",
		Options:    []domain.QuestionOption{{Label: "yes"}, {Label: "no"}},
	})
	updatedModel, _ = m.Update(runtimeEventMsg(evt))
	m = updatedModel.(Model)
	if m.mode != ModeQuestion {
		t.Fatalf("mode = %s, want question", m.mode)
	}
	m.questionShownAt = time.Now().Add(-time.Second)
	updatedModel, cmd = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode = %s, want chat after skip", m.mode)
	}
	if cmd == nil {
		t.Fatal("skip should still answer the questioner")
	}
	if !strings.Contains(m.statusMessage, "skipped") {
		t.Fatalf("status = %q, want skip ack", m.statusMessage)
	}
}

func TestReasoningPickerFlow(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	// Open the picker and move the cursor from "default" down to "high".
	updated, _ := m.handleSlashCommand("/reasoning")
	m = updated.(Model)
	for i := 0; i < 4; i++ {
		updatedModel, _ := m.Update(tea.KeyMsg{Type: tea.KeyDown})
		m = updatedModel.(Model)
	}
	if got := m.reasoningFinder.Selected().Arg; got != "high" {
		t.Fatalf("cursor = %q, want high", got)
	}

	// Enter applies the selection through the controller; the header dial
	// picks up the override marker.
	updatedModel, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode = %s, want chat after selection", m.mode)
	}
	if cmd == nil {
		t.Fatal("picker Enter should spawn the SetReasoning command")
	}
	msg, ok := cmd().(reasoningChangedMsg)
	if !ok || msg.err != nil {
		t.Fatalf("cmd = %+v (ok=%v, err=%v)", msg, ok, msg.err)
	}
	updatedModel, _ = m.Update(msg)
	m = updatedModel.(Model)
	if m.reasoningEffort != "high" || !m.reasoningOverridden {
		t.Fatalf("dial = %q (overridden=%v), want high/override", m.reasoningEffort, m.reasoningOverridden)
	}

	// Reopening puts the cursor on the override level; confirming the
	// active dial is a no-op ack instead of a controller round-trip.
	updated, _ = m.handleSlashCommand("/reasoning")
	m = updated.(Model)
	if got := m.reasoningFinder.Selected().Arg; got != "high" {
		t.Fatalf("reopened cursor = %q, want high", got)
	}
	updatedModel, cmd = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updatedModel.(Model)
	if cmd != nil {
		t.Fatal("confirming the active dial should not spawn a command")
	}
	if !strings.Contains(m.statusMessage, "unchanged") {
		t.Fatalf("status = %q, want unchanged ack", m.statusMessage)
	}
	if m.reasoningEffort != "high" {
		t.Fatalf("dial = %q after unchanged ack, want high", m.reasoningEffort)
	}

	// Esc steps out one level at a time (insert → normal → closed) and
	// never touches the dial.
	updated, _ = m.handleSlashCommand("/reasoning")
	m = updated.(Model)
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if m.mode != ModeReasoningPicker || !m.reasoningFinder.Normal() {
		t.Fatalf("first Esc should enter normal mode, mode = %s", m.mode)
	}
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode = %s, want chat after second Esc", m.mode)
	}
	if m.reasoningEffort != "high" {
		t.Fatalf("dial = %q after Esc, want high", m.reasoningEffort)
	}
}

func TestSlashCommandModelOpensPicker(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	m.SetModels([]ModelOption{
		{Provider: "test", Name: "model-a", ContextWindow: 128000, WireAPI: "chat"},
		{Provider: "test", Name: "model-b", ContextWindow: 64000, WireAPI: "responses"},
	})

	// Bare /model opens the picker with the cursor on the active model.
	updated, cmd := m.handleSlashCommand("/model")
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("opening the picker should not spawn a command")
	}
	if m.mode != ModeModelPicker {
		t.Fatalf("mode = %q, want model_picker", m.mode)
	}
	if m.modelFinder == nil || m.modelFinder.Selected() == nil ||
		m.modelFinder.Selected().Ref() != "test/model-a" {
		t.Fatalf("picker cursor = %+v, want the active model", m.modelFinder.Selected())
	}

	// j/k navigate in normal mode (Esc enters it from insert mode).
	updatedModel, _ := m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if !m.modelFinder.Normal() {
		t.Fatal("Esc should enter normal mode")
	}
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'j'}})
	m = updatedModel.(Model)
	if got := m.modelFinder.Selected().Ref(); got != "test/model-b" {
		t.Fatalf("selected after j = %q, want test/model-b", got)
	}
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'k'}})
	m = updatedModel.(Model)
	if got := m.modelFinder.Selected().Ref(); got != "test/model-a" {
		t.Fatalf("selected after k = %q, want test/model-a", got)
	}

	// Enter on the second row switches the model.
	updatedModel, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'j'}})
	m = updatedModel.(Model)
	updatedModel, cmd = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updatedModel.(Model)
	if m.mode != ModeChat {
		t.Fatalf("mode after Enter = %q, want chat", m.mode)
	}
	if cmd == nil {
		t.Fatal("Enter should spawn the switch command")
	}
	msg, ok := cmd().(modelChangedMsg)
	if !ok {
		t.Fatalf("cmd returned %T, want modelChangedMsg", msg)
	}
	if msg.err != nil {
		t.Fatalf("modelChangedMsg err = %v", msg.err)
	}
	updatedModel, _ = m.Update(msg)
	m = updatedModel.(Model)
	if got := m.modelName; got != "test/model-b" {
		t.Fatalf("modelName = %q, want test/model-b", got)
	}
	if got := m.contextWindow; got != 64000 {
		t.Fatalf("contextWindow = %d, want 64000", got)
	}
}

func TestModelPickerEscCancels(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	m.SetModels([]ModelOption{
		{Provider: "test", Name: "model-a", ContextWindow: 128000},
		{Provider: "test", Name: "model-b", ContextWindow: 64000},
	})
	updated, _ := m.handleSlashCommand("/model")
	m = updated.(Model)
	// Esc steps out one level at a time: insert → normal → closed.
	updatedModel, _ := m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	updatedModel, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updatedModel.(Model)
	if cmd != nil {
		t.Fatal("Esc should not spawn a command")
	}
	if m.mode != ModeChat {
		t.Fatalf("mode after Esc = %q, want chat", m.mode)
	}
	if got := m.modelName; got != "test/model-a" {
		t.Fatalf("modelName = %q, want unchanged test/model-a", got)
	}
}

func TestSteerAckRemovesOptimisticEcho(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	// Simulate a submitted prompt whose optimistic echo is visible.
	m.pendingSubmitID = m.blocks.AddPendingUserBlock("follow up")
	m.pendingSubmitPrompt = "follow up"

	updatedModel, _ := m.Update(promptSubmittedMsg{
		prompt: "follow up",
		result: app.SubmitResult{Steered: true, QueueLen: 2},
	})
	m = updatedModel.(Model)

	if m.pendingSubmitID != "" || m.pendingSubmitPrompt != "" {
		t.Fatal("steer ack must clear the pending submit tracking")
	}
	for _, id := range m.blocks.Order {
		if m.blocks.ByID[id].Content == "follow up" {
			t.Fatal("steered message must not stay in the transcript as a user block")
		}
	}
	if !strings.Contains(m.statusMessage, "Queued") || m.statusIsError {
		t.Fatalf("status = %q (error=%v), want queued hint", m.statusMessage, m.statusIsError)
	}
}

func TestSteerPanelLifecycle(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	m.width = 100

	// steer.queued feeds the panel in order.
	for _, text := range []string{"first note", "second note"} {
		updatedModel, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{
			Sequence: 1,
			Kind:     runtimeevent.KindSteerQueued,
			Payload:  mustPayload(t, runtimeevent.SteerQueuedPayload{Text: text, QueueLen: len(m.pendingSteers) + 1}),
		})
		m = updatedModel
	}
	if len(m.pendingSteers) != 2 || m.pendingSteers[0] != "first note" {
		t.Fatalf("pendingSteers = %v", m.pendingSteers)
	}
	panel := m.renderSteerPanel()
	for _, want := range []string{"Steering (2 queued", "↳ first note", "↳ second note"} {
		if !strings.Contains(panel, want) {
			t.Fatalf("steer panel missing %q:\n%s", want, panel)
		}
	}

	// steer.injected drains head-first and appends the transcript block.
	updatedModel, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{
		Sequence: 2,
		Kind:     runtimeevent.KindSteerInjected,
		Payload:  mustPayload(t, runtimeevent.SteerInjectedPayload{Text: "first note"}),
	})
	m = updatedModel
	if len(m.pendingSteers) != 1 || m.pendingSteers[0] != "second note" {
		t.Fatalf("pendingSteers after inject = %v, want [second note]", m.pendingSteers)
	}
	found := false
	for _, id := range m.blocks.Order {
		if m.blocks.ByID[id].Content == "first note" && m.blocks.ByID[id].Kind == BlockKindUser {
			found = true
		}
	}
	if !found {
		t.Fatal("injected steer must append a user block to the transcript")
	}
}

func TestSteerPanelFlushesOnTurnStarted(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	m.pendingSteers = []string{"leftover"}

	// The relayed leftovers come back as the new turn's prompt: the panel
	// must flush instead of double-showing them alongside the user block.
	updatedModel, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{
		Sequence: 3,
		Kind:     runtimeevent.KindTurnStarted,
		Payload:  mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 2, Prompt: "leftover"}),
	})
	m = updatedModel
	if len(m.pendingSteers) != 0 {
		t.Fatalf("pendingSteers after turn start = %v, want flushed", m.pendingSteers)
	}
}

func TestSteerPanelRebuildsFromSnapshot(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")
	updatedModel, _ := m.handleSnapshot(snapshotMsg{snapshot: app.Snapshot{
		State:         app.ControllerStateRunning,
		PendingSteers: []string{"kept"},
	}})
	m = updatedModel.(Model)
	if len(m.pendingSteers) != 1 || m.pendingSteers[0] != "kept" {
		t.Fatalf("pendingSteers from snapshot = %v", m.pendingSteers)
	}
}

func TestSlashCommandModelFailureRestoresDraft(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "test/model-a", "/ws")

	updatedModel, _ := m.Update(modelChangedMsg{command: "/model oops", err: fmt.Errorf("boom")})
	m = updatedModel.(Model)
	if got := m.textArea.Value(); got != "/model oops" {
		t.Fatalf("failure should restore the draft, got %q", got)
	}
	if !m.statusIsError {
		t.Fatal("failure should be flagged as an error status")
	}
	if got := m.modelName; got != "test/model-a" {
		t.Fatalf("modelName = %q, want unchanged test/model-a", got)
	}
}

func TestApprovalResolvedEventClearsOverlay(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	approvalID := domain.NewEventID()
	callID := domain.NewToolCallID()
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: approvalID,
		CallID:     callID,
	}
	m.mode = ModeApproval

	payload := mustPayload(t, runtimeevent.ApprovalResolvedPayload{
		ApprovalID: approvalID,
		CallID:     callID,
		Decision:   domain.DecisionAllow,
	})
	updated, _ := m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Kind: runtimeevent.KindApprovalResolved, Payload: payload}))
	m = updated.(Model)
	if m.pendingApproval != nil || m.mode != ModeChat {
		t.Fatalf("resolved approval left overlay stuck: pending=%v mode=%s", m.pendingApproval, m.mode)
	}
}

func TestApprovalOverlayNavigation(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		CallID:     domain.NewToolCallID(),
		ToolName:   "run_cmd",
		Risk:       domain.R2,
		// Rule-eligible arguments: Enter on the always-allow option must
		// resolve (calls without a derivable rule keep the overlay up).
		Arguments: json.RawMessage(`{"program":"go","args":["test","./..."]}`),
	}
	m.mode = ModeApproval
	if m.approvalCursor != 0 {
		t.Fatalf("initial cursor = %d, want 0 (allow)", m.approvalCursor)
	}

	updated, cmd := m.Update(tea.KeyMsg{Type: tea.KeyRight})
	m = updated.(Model)
	if m.approvalCursor != 1 || cmd != nil {
		t.Fatalf("Right: cursor = %d, cmd = %v", m.approvalCursor, cmd)
	}
	// Three options: allow once (0) / allow + remember rule (1) / deny (2).
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyTab})
	m = updated.(Model)
	if m.approvalCursor != 2 {
		t.Fatalf("Tab: cursor = %d, want 2 (deny)", m.approvalCursor)
	}
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyShiftTab})
	m = updated.(Model)
	if m.approvalCursor != 1 {
		t.Fatalf("Shift+Tab: cursor = %d, want 1", m.approvalCursor)
	}

	// Enter on the allow option resolves the approval asynchronously and
	// returns to chat immediately.
	updated, cmd = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("Enter did not issue a resolve command")
	}
	if m.pendingApproval != nil || m.mode != ModeChat {
		t.Fatalf("overlay stuck after Enter: pending=%v mode=%s", m.pendingApproval, m.mode)
	}
}

func TestApprovalOverlayRendersFieldsAndOptions(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 80}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID:  domain.NewEventID(),
		CallID:      domain.NewToolCallID(),
		ToolName:    "run_cmd",
		Risk:        domain.R2,
		Description: "Run `make test`",
		ArgsHash:    "0123456789abcdef",
		ReadPaths:   []string{"./src"},
		WritePaths:  []string{"./out"},
	}
	view := m.renderApprovalOverlay()
	for _, want := range []string{
		"Approval Required", "R2 (write)", "run_cmd", "make test",
		"./src", "./out", "Allow once", "Deny", "Ctrl+C",
		"1.", "2.", "3.", "▍",
	} {
		if !strings.Contains(view, want) {
			t.Fatalf("approval overlay missing %q:\n%s", want, view)
		}
	}
	// The args hash is audit plumbing and no longer shown; the audit trail
	// keeps it in the session events.
	if strings.Contains(view, "0123456789ab") {
		t.Fatalf("overlay must not display the args hash:\n%s", view)
	}
}

func TestApprovalOverlayStructuresRunCmdDescription(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100, workspace: "/ws"}
	m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
		ApprovalID: domain.NewEventID(),
		CallID:     domain.NewToolCallID(),
		ToolName:   "run_cmd",
		Risk:       domain.R3,
		Description: "Run; 'sh' '-c' 'which weather'; env[none]; cwd='.'; timeout=120000ms; " +
			"network=loopback-only; shell=R3; note[检查 weather 是否安装]; args_hash=a47946448cfa",
		ReadPaths:  []string{"/ws"},
		WritePaths: []string{"/ws"},
	}
	view := m.renderApprovalOverlay()
	// Action keeps the command, metadata folds into one dim row, the note
	// stands alone, and the workspace root collapses to a relative label.
	for _, want := range []string{
		"Run 'sh' '-c' 'which weather'",
		"cwd=. · timeout=120000ms · network=loopback-only",
		"检查 weather 是否安装",
		"workspace (.)",
	} {
		if !strings.Contains(view, want) {
			t.Fatalf("structured overlay missing %q:\n%s", want, view)
		}
	}
	if strings.Contains(view, "args_hash") {
		t.Fatalf("overlay must drop the args_hash segment:\n%s", view)
	}
}

func TestApprovalNumberKeysAndDisabledAlways(t *testing.T) {
	newApprovalModel := func(toolName string, args json.RawMessage) Model {
		m := Model{theme: NoColorTheme(), width: 100, mode: ModeApproval}
		m.pendingApproval = &runtimeevent.ApprovalRequestedPayload{
			ApprovalID: domain.NewEventID(),
			CallID:     domain.NewToolCallID(),
			ToolName:   toolName,
			Risk:       domain.R2,
			Arguments:  args,
		}
		return m
	}

	// "1" resolves allow-once directly.
	m := newApprovalModel("run_cmd", json.RawMessage(`{"program":"go","args":["test","./..."]}`))
	updated, cmd := m.handleApprovalKey(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'1'}})
	m = updated.(Model)
	if cmd == nil || m.pendingApproval != nil {
		t.Fatal("number key 1 must resolve allow-once")
	}

	// "2" remembers a rule when one is derivable.
	m = newApprovalModel("run_cmd", json.RawMessage(`{"program":"go","args":["test","./..."]}`))
	updated, cmd = m.handleApprovalKey(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'2'}})
	m = updated.(Model)
	if cmd == nil || m.pendingApproval != nil {
		t.Fatal("number key 2 must resolve always-allow for a rule-eligible call")
	}

	// For a compound shell call the rule is not derivable: "2", "a" and
	// Enter on the always option are all inert, and the overlay stays up.
	m = newApprovalModel("run_cmd", json.RawMessage(`{"program":"sh","args":["-c","echo hi | cat"]}`))
	m.approvalCursor = 1
	for _, key := range []tea.KeyMsg{
		{Type: tea.KeyRunes, Runes: []rune{'2'}},
		{Type: tea.KeyRunes, Runes: []rune{'a'}},
		{Type: tea.KeyEnter},
	} {
		updated, cmd := m.handleApprovalKey(key)
		m = updated.(Model)
		if cmd != nil || m.pendingApproval == nil {
			t.Fatalf("key %v must be inert when always-allow is unavailable", key.Type)
		}
	}
}

func TestSpinnerRunsOnlyWhileBusy(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")

	// While idle a stray tick must not start a chain.
	updated, cmd := m.Update(spinner.TickMsg{Time: time.Now(), ID: m.spinner.ID()})
	m = updated.(Model)
	if cmd != nil || m.spinning {
		t.Fatal("spinner should not tick while idle")
	}

	// A busy event starts the chain.
	updated, cmd = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Kind: runtimeevent.KindModelRequestStarted}))
	m = updated.(Model)
	if !m.spinning || cmd == nil {
		t.Fatal("busy turn did not start the spinner")
	}

	// Ticks keep flowing while busy.
	updated, cmd = m.Update(spinner.TickMsg{Time: time.Now(), ID: m.spinner.ID()})
	m = updated.(Model)
	if cmd == nil {
		t.Fatal("spinner chain stopped while busy")
	}

	// Once the turn idles, the next tick stops the chain.
	updated, _ = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnFinished}))
	m = updated.(Model)
	updated, cmd = m.Update(spinner.TickMsg{Time: time.Now(), ID: m.spinner.ID()})
	m = updated.(Model)
	if cmd != nil || m.spinning {
		t.Fatal("spinner chain did not stop after idle")
	}
}

func TestToolRunningSummaryShowsSpinnerAndElapsed(t *testing.T) {
	m := Model{theme: NoColorTheme(), spinner: spinner.New(spinner.WithSpinner(spinner.MiniDot))}
	m.SetIcons(PlainIcons())
	block := &TranscriptBlock{
		Kind:      BlockKindTool,
		Title:     "run_cmd",
		Status:    "running",
		StartedAt: time.Now().Add(-2 * time.Second),
	}
	summary := m.renderToolSummary(block)
	if !strings.Contains(summary, "run_cmd") || !strings.Contains(summary, "2s") {
		t.Fatalf("running summary = %q, want tool name and elapsed time", summary)
	}

	block.Status = "success"
	block.Detail = "42ms"
	summary = m.renderToolSummary(block)
	if !strings.Contains(summary, "✓") || !strings.Contains(summary, "42ms") {
		t.Fatalf("success summary = %q, want ✓ and duration", summary)
	}
}

func TestStreamingBlockShowsCaret(t *testing.T) {
	idx := NewBlockIndex()
	payload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "half"})
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 2, Kind: runtimeevent.KindModelTextDelta, Payload: payload})
	block, ok := idx.Get("stream-2-1")
	if !ok {
		t.Fatal("missing stream block")
	}
	view := Model{theme: NoColorTheme()}.renderBlock(block)
	if !strings.Contains(view, "▌") {
		t.Fatalf("streaming block missing caret: %q", view)
	}

	// Finalized blocks lose the caret.
	ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Turn: 2, Kind: runtimeevent.KindModelResponseCompleted})
	view = Model{theme: NoColorTheme()}.renderBlock(block)
	if strings.Contains(view, "▌") {
		t.Fatalf("finalized block still has caret: %q", view)
	}
}

func TestCompletionCandidatesFiltering(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")

	m.textArea.SetValue("/")
	if got := len(m.completionCandidates()); got != len(slashCommands) {
		t.Fatalf("\"/\" candidates = %d, want %d", got, len(slashCommands))
	}
	if !m.completionVisible() {
		t.Fatal("completion should be visible for \"/\"")
	}

	m.textArea.SetValue("/re")
	names := []string{}
	for _, c := range m.completionCandidates() {
		names = append(names, c.name)
	}
	if len(names) != 2 || names[0] != "/resume" || names[1] != "/reasoning" {
		t.Fatalf("\"/re\" candidates = %v, want [/resume /reasoning]", names)
	}

	m.textArea.SetValue("/resu")
	names = []string{}
	for _, c := range m.completionCandidates() {
		names = append(names, c.name)
	}
	if len(names) != 1 || names[0] != "/resume" {
		t.Fatalf("\"/resu\" candidates = %v, want [/resume]", names)
	}

	m.textArea.SetValue("/x")
	if got := m.completionCandidates(); got != nil {
		t.Fatalf("\"/x\" candidates = %v, want nil", got)
	}

	// Completion ends once the command name is finished typing.
	m.textArea.SetValue("/resume sess_abc")
	if got := m.completionCandidates(); got != nil {
		t.Fatalf("candidates after space = %v, want nil", got)
	}
}

func TestCompletionNavigationAndTabApply(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("/")

	updated, _ := m.Update(tea.KeyMsg{Type: tea.KeyDown})
	m = updated.(Model)
	if m.completionCursor != 1 {
		t.Fatalf("cursor after Down = %d, want 1", m.completionCursor)
	}
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyUp})
	m = updated.(Model)
	if m.completionCursor != 0 {
		t.Fatalf("cursor after Up = %d, want 0", m.completionCursor)
	}
	// Wrap around upwards.
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyUp})
	m = updated.(Model)
	if m.completionCursor != len(slashCommands)-1 {
		t.Fatalf("cursor wrap = %d, want %d", m.completionCursor, len(slashCommands)-1)
	}

	// Tab applies the selected command without submitting.
	m.textArea.SetValue("/ses")
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyTab})
	m = updated.(Model)
	if got := m.textArea.Value(); got != "/sessions" {
		t.Fatalf("Tab applied %q, want /sessions", got)
	}
	if len(m.blocks.Order) != 0 {
		t.Fatal("Tab must not submit")
	}

	// Argument-taking commands get a trailing space.
	m.textArea.SetValue("/res")
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyTab})
	m = updated.(Model)
	if got := m.textArea.Value(); got != "/resume " {
		t.Fatalf("Tab applied %q, want \"/resume \"", got)
	}
}

func TestEnterCompletesPartialCommandBeforeSubmit(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("/he")

	updated, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updated.(Model)
	if cmd != nil {
		t.Fatal("Enter on a partial command must not submit")
	}
	if got := m.textArea.Value(); got != "/help" {
		t.Fatalf("Enter completed to %q, want /help", got)
	}

	// A full command name submits and opens help.
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = updated.(Model)
	if m.mode != ModeHelp {
		t.Fatalf("submitting /help should open help, mode=%s", m.mode)
	}
}

func TestEscDismissesCompletionUntilDraftChanges(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.textArea.SetValue("/")

	updated, _ := m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = updated.(Model)
	if m.completionVisible() {
		t.Fatal("Esc did not dismiss completion")
	}

	// Typing again re-arms the popup.
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'h'}})
	m = updated.(Model)
	if !m.completionVisible() {
		t.Fatal("completion did not re-arm after typing")
	}
}

func TestHelpOverlayListsAllCommands(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 100}
	view := m.renderHelpOverlay()
	for _, want := range []string{"Loom TUI Help", "Keyboard", "Commands", "Enter", "Ctrl+R"} {
		if !strings.Contains(view, want) {
			t.Fatalf("help overlay missing %q:\n%s", want, view)
		}
	}
	for _, c := range slashCommands {
		if !strings.Contains(view, c.usage) {
			t.Fatalf("help overlay missing command %q:\n%s", c.usage, view)
		}
	}
}

func TestRuntimeEventsFromOtherSessionsAreIgnored(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")

	first := domain.NewSessionID()
	payload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: 1, Prompt: "hello"})
	updated, _ := m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{SessionID: first, Sequence: 1, Kind: runtimeevent.KindTurnStarted, Payload: payload}))
	m = updated.(Model)
	if m.sessionID != first {
		t.Fatalf("unbound UI did not adopt session %s", first)
	}
	if len(m.blocks.Order) != 1 {
		t.Fatalf("block count = %d, want 1", len(m.blocks.Order))
	}

	other := domain.NewSessionID()
	updated, _ = m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{SessionID: other, Sequence: 2, Kind: runtimeevent.KindTurnStarted, Payload: payload}))
	m = updated.(Model)
	if m.sessionID != first {
		t.Fatalf("session hijacked by foreign event: %s", m.sessionID)
	}
	if len(m.blocks.Order) != 1 {
		t.Fatalf("foreign session event added blocks: %v", m.blocks.Order)
	}
}

func TestToggleReasoningAtClick(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 80, height: 24, mode: ModeChat}
	m.blocks = NewBlockIndex()
	m.viewport = lineView{Width: 80, Height: 10}
	m.blocks.Add(&TranscriptBlock{ID: "u1", Kind: BlockKindUser, Title: "You", Content: "hello", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "a1", Kind: BlockKindAssistant, Title: "Assistant", Content: "answer one", StreamReasoning: "chain of thought", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "a2", Kind: BlockKindAssistant, Title: "Assistant", Content: "answer two", Done: true})
	m.syncTranscript()

	assistant, _ := m.blocks.Get("a1")
	rowOf := func(id string) int { return m.blockOffsets[id] + 1 } // +1: header row

	// Clicking the assistant block with reasoning expands it; clicking again collapses.
	updated, _ := m.handleMouse(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonLeft, Y: rowOf("a1")})
	m = updated.(Model)
	if !assistant.ReasoningExpanded {
		t.Fatal("click on reasoning block should expand the reasoning")
	}
	updated, _ = m.handleMouse(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonLeft, Y: rowOf("a1")})
	m = updated.(Model)
	if assistant.ReasoningExpanded {
		t.Fatal("second click should collapse the reasoning")
	}

	// Blocks without reasoning and rows above the transcript ignore clicks.
	updated, _ = m.handleMouse(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonLeft, Y: rowOf("a2")})
	m = updated.(Model)
	if assistant.ReasoningExpanded {
		t.Fatal("click on a block without reasoning must not toggle anything")
	}
	if m.toggleReasoningAt(0) {
		t.Fatal("click on the header row must not toggle anything")
	}
}

// Blocks get a blank separator row between logical sections; consecutive
// tool calls stay packed so a retry burst still reads as one list.
func TestSyncTranscriptSeparatesSectionsButPacksToolRuns(t *testing.T) {
	m := Model{theme: NoColorTheme(), width: 80, height: 30}
	m.blocks = NewBlockIndex()
	m.viewport = lineView{Width: 80, Height: 20}
	m.blocks.Add(&TranscriptBlock{ID: "u1", Kind: BlockKindUser, Title: "You", Content: "q", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "a1", Kind: BlockKindAssistant, Title: "Assistant", Content: "lead in", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "t1", Kind: BlockKindTool, Title: "run_cmd", Status: "success", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "t2", Kind: BlockKindTool, Title: "run_cmd", Status: "success", Done: true})
	m.blocks.Add(&TranscriptBlock{ID: "a2", Kind: BlockKindAssistant, Title: "Assistant", Content: "answer", Done: true})
	m.syncTranscript()

	// u1(1) + blank + a1(1) + blank + t1(1) t2(1) + blank + a2(1)
	wantOffsets := map[string]int{"u1": 0, "a1": 2, "t1": 4, "t2": 5, "a2": 7}
	for id, want := range wantOffsets {
		if got := m.blockOffsets[id]; got != want {
			t.Fatalf("blockOffsets[%s] = %d, want %d", id, got, want)
		}
	}
	if got := m.viewport.TotalLineCount(); got != 8 {
		t.Fatalf("transcript rows = %d, want 8 (2 blanks): offsets=%v", got, m.blockOffsets)
	}
}

func TestReasoningBlockThemeContrast(t *testing.T) {
	dark := DefaultTheme()
	light := LightTheme()
	if dark.ReasoningBlock.GetBackground() == light.ReasoningBlock.GetBackground() {
		t.Fatal("dark and light reasoning panels must use different background colors")
	}
	if dark.ReasoningBlock.GetBackground() == nil {
		t.Fatal("dark theme reasoning block must have a panel background")
	}
	if _, isNoColor := NoColorTheme().ReasoningBlock.GetBackground().(lipgloss.NoColor); !isNoColor {
		t.Fatalf("no-color reasoning block must not paint a background, got %v", NoColorTheme().ReasoningBlock.GetBackground())
	}
}

// Fragments of one escape sequence must come back as a single read, so the
// input parser never sees a lone ESC byte (which it would misread as the
// Escape key and turn the rest of the sequence into text). The reader
// understands sequence shape, so the inter-byte gap does not matter.
func TestAnsiSeqReaderReassemblesFragments(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	rd := newInputReader(r)

	seq := []byte("\x1b[<65;47;16M")
	go func() {
		for _, b := range seq {
			if _, err := w.Write([]byte{b}); err != nil {
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()

	buf := make([]byte, 64)
	n, err := rd.Read(buf)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if got := string(buf[:n]); got != string(seq) {
		t.Fatalf("read = %q, want the whole sequence %q", got, seq)
	}
}

// A severed head/tail pair is reassembled even when the tail arrives much
// later than any debounce window would allow (as long as it beats the
// escape-hatch timeout).
func TestAnsiSeqReaderWaitsForSlowTail(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	rd := newInputReader(r)

	go func() {
		if _, err := w.Write([]byte("\x1b")); err != nil {
			return
		}
		time.Sleep(fragmentTimeout / 2)
		_, _ = w.Write([]byte("[<65;47;16M"))
	}()

	buf := make([]byte, 64)
	n, err := rd.Read(buf)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if got := string(buf[:n]); got != "\x1b[<65;47;16M" {
		t.Fatalf("read = %q, want the whole sequence", got)
	}
}

// Plain text passes through immediately, and a manual ESC keypress is
// delivered after the escape-hatch timeout rather than being stuck.
func TestAnsiSeqReaderPassthroughAndLoneEsc(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	rd := newInputReader(r)

	if _, err := w.Write([]byte("ab")); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 64)
	n, err := rd.Read(buf)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if got := string(buf[:n]); got != "ab" {
		t.Fatalf("read = %q, want %q", got, "ab")
	}

	// A lone ESC is forwarded after the timeout (parser maps it to the
	// Escape key).
	if _, err := w.Write([]byte("\x1b")); err != nil {
		t.Fatal(err)
	}
	n, err = rd.Read(buf)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if got := string(buf[:n]); got != "\x1b" {
		t.Fatalf("lone ESC read = %q, want ESC byte", got)
	}
}

// Sequence-boundary detection: CSI, SS3, X10 mouse, OSC and Alt+char.
func TestSeqComplete(t *testing.T) {
	cases := []struct {
		in   string
		want bool
	}{
		{"\x1b", false},
		{"\x1b[", false},
		{"\x1b[<", false},
		{"\x1b[<65;47;16", false},
		{"\x1b[<65;47;16M", true},
		{"\x1b[<65;47;16m", true},
		{"\x1b[A", true},
		{"\x1b[200~", true},
		{"\x1b[M", false},
		{"\x1b[MaO0", true},
		{"\x1bO", false},
		{"\x1bOA", true},
		{"\x1b]", false},
		{"\x1b]11;rgb:0000/0000/0000\x07", true},
		{"\x1b]11;rgb:0000\x1b\\", true},
		{"\x1bx", true},
	}
	for _, tt := range cases {
		if got := seqComplete([]byte(tt.in)); got != tt.want {
			t.Errorf("seqComplete(%q) = %v, want %v", tt.in, got, tt.want)
		}
	}
}

func TestGradientColors(t *testing.T) {
	colors := gradientColors("#e69875", "#dbbc7f", 6)
	if len(colors) != 6 {
		t.Fatalf("gradient length = %d, want 6", len(colors))
	}
	if colors[0] != "#e69875" || colors[5] != "#dbbc7f" {
		t.Fatalf("gradient endpoints = %v, want exact endpoints", colors)
	}
	// Midpoint at t=0.6: r=230+(219-230)*0.6≈223, g=152+(188-152)*0.6≈174, b=117+(127-117)*0.6=123.
	if colors[3] != "#dfae7b" {
		t.Fatalf("gradient midpoint = %s, want #dfae7b", colors[3])
	}
	if gradientColors("", "#dbbc7f", 3) != nil {
		t.Fatal("non-hex endpoint must yield nil so callers can fall back")
	}
	if got := gradientColors("#e69875", "#dbbc7f", 1); len(got) != 1 || got[0] != "#e69875" {
		t.Fatalf("single-step gradient = %v, want the start color", got)
	}
}
