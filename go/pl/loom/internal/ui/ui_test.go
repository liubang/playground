// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

package ui

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// newTestController starts a real controller loop so state-dependent key
// handling can be exercised without fakes.
func newTestController(t *testing.T) *app.Controller {
	t.Helper()
	ctrl := app.NewController(app.ControllerConfig{
		Broker:   runtimeevent.NewBroker(),
		Approver: app.NewChannelApprover(),
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
	block, ok := idx.Get("stream-7")
	if !ok || block.Content != "hello world" {
		t.Fatalf("stream block = %#v, exists=%v", block, ok)
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

	block, ok := idx.Get("stream-7")
	if !ok || block.StreamReasoning != "inspect the request" {
		t.Fatalf("reasoning block = %#v, exists=%v", block, ok)
	}
	m := Model{theme: NoColorTheme()}
	if view := m.renderBlock(block); !strings.Contains(view, "Thinking... (press Ctrl+R to expand)") {
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
	block, ok := idx.Get("stream-7")
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
	stream, ok := idx.Get("stream-3")
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

	block, ok := idx.Get("stream-1")
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

	for _, id := range []string{pendingID, "stream-1"} {
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

func TestSessionPickerSelection(t *testing.T) {
	first, second := domain.NewSessionID(), domain.NewSessionID()
	picker := NewSessionPicker()
	picker.Load([]app.SessionSummary{{ID: first}, {ID: second}}, nil)
	picker.MoveDown()
	if got := picker.Selected(); got != second {
		t.Fatalf("Selected() = %s, want %s", got, second)
	}
}

func TestSessionPickerWindowsLongLists(t *testing.T) {
	picker := NewSessionPicker()
	var summaries []app.SessionSummary
	for i := 0; i < 20; i++ {
		summaries = append(summaries, app.SessionSummary{ID: domain.NewSessionID(), UpdatedAt: time.Now()})
	}
	picker.Load(summaries, nil)
	for i := 0; i < 15; i++ {
		picker.MoveDown()
	}
	rendered := picker.Render(80, 8)
	if !strings.Contains(rendered, "↑ more") {
		t.Fatalf("windowed picker missing upward hint:\n%s", rendered)
	}
	if strings.Count(rendered, "sess_") > 10 {
		t.Fatalf("windowed picker rendered too many rows:\n%s", rendered)
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
	m.textArea.SetValue("/compact")
	updated, _ := m.handleSlashCommand("/compact")
	m = updated.(Model)
	if got := m.textArea.Value(); got != "/compact" {
		t.Fatalf("unimplemented command should keep the draft, got %q", got)
	}
	if !strings.Contains(m.statusMessage, "not implemented") {
		t.Fatalf("status = %q", m.statusMessage)
	}

	m.textArea.SetValue("/frobnicate")
	updated, _ = m.handleSlashCommand("/frobnicate")
	m = updated.(Model)
	if got := m.textArea.Value(); got != "/frobnicate" {
		t.Fatalf("unknown command should keep the draft, got %q", got)
	}
	if !m.statusIsError {
		t.Fatal("unknown command should be flagged as an error status")
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
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyTab})
	m = updated.(Model)
	if m.approvalCursor != 1 {
		t.Fatalf("Tab: cursor = %d, want 1", m.approvalCursor)
	}
	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyShiftTab})
	m = updated.(Model)
	if m.approvalCursor != 0 {
		t.Fatalf("Shift+Tab: cursor = %d, want 0", m.approvalCursor)
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
		"./src", "./out", "0123456789ab", "Allow once", "Deny", "Ctrl+C",
	} {
		if !strings.Contains(view, want) {
			t.Fatalf("approval overlay missing %q:\n%s", want, view)
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
	block, ok := idx.Get("stream-2")
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
	if len(names) != 1 || names[0] != "/resume" {
		t.Fatalf("\"/re\" candidates = %v, want [/resume]", names)
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
