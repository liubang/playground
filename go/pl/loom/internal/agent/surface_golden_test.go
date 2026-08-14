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
// Created: 2026/08/14

package agent

import (
	"bytes"
	"context"
	"encoding/json"
	"log/slog"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// Golden consistency tests (docs/SURFACE_DESIGN.md §5.1): the surface
// replayed from the pure event log must be byte-identical to the surface
// the live loop held — "model-visible ⟺ logged" in executable form.

// assertMessagesMatchRuntime compares the replayed surface against the
// loop's in-memory messages via canonical JSON per message.
func assertMessagesMatchRuntime(t *testing.T, replayed, runtime []domain.Message) {
	t.Helper()
	if len(replayed) != len(runtime) {
		t.Fatalf("replayed %d messages, runtime has %d", len(replayed), len(runtime))
	}
	for i := range runtime {
		want, err := json.Marshal(runtime[i])
		if err != nil {
			t.Fatalf("marshal runtime message %d: %v", i, err)
		}
		got, err := json.Marshal(replayed[i])
		if err != nil {
			t.Fatalf("marshal replayed message %d: %v", i, err)
		}
		if !bytes.Equal(want, got) {
			t.Fatalf("message %d diverges:\n  runtime: %s\n  replay:  %s", i, want, got)
		}
	}
}

// bigResultTool returns a fake tool whose output is size bytes — large
// enough to trigger Level-1 masking under the test's MaskMinBytes.
func bigResultTool(size int) *fakes.FakeTool {
	def := domain.ToolDefinition{
		Name:         "big_output",
		Description:  "Return a large output",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}
	return fakes.NewFakeTool(def, domain.ToolResult{
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: bigOutput(size)}},
		StartedAt:  time.Now(),
		FinishedAt: time.Now(),
	})
}

func openLoopArtifacts(t *testing.T) *artifact.Store {
	t.Helper()
	store, err := artifact.Open(t.TempDir(), 1<<20)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	return store
}

func eventTypesIn(events []domain.Event) map[domain.EventType]int {
	counts := map[domain.EventType]int{}
	for _, evt := range events {
		counts[evt.Type]++
	}
	return counts
}

func TestLoopMaskCompactionSurfaceMatchesEventReplay(t *testing.T) {
	store := fakes.NewFakeStore()
	artifacts := openLoopArtifacts(t)
	tool := bigResultTool(8000)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"next"}`)},
			},
			StopReason: domain.StopToolUse,
			// Past the 3000 trigger: the metered input alone decides,
			// because tool-result messages are assistant-role and the
			// occupancy tail after the last assistant message is empty.
			// Compaction fires before call three, when the big result is
			// outside the keep-recent window.
			UsageIn:  3050,
			UsageOut: 30,
		},
		fakes.ScriptEntry{Text: "analysis done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register echo: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "check the log")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		Condenser: Condenser{KeepRecentMessages: 2, MaskMinBytes: 4096},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	counts := eventTypesIn(events)
	if counts[domain.EventContextMasked] != 1 {
		t.Fatalf("context.masked events = %d, want 1 (types: %v)", counts[domain.EventContextMasked], counts)
	}
	if counts[domain.EventContextCompacted] != 1 {
		t.Fatalf("context.compacted events = %d, want 1", counts[domain.EventContextCompacted])
	}

	// The golden invariant: replay from the pure log reproduces the exact
	// surface the loop held, masking included.
	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)

	// ReplayFull ignores directives: the original 8KB output survives there.
	full, err := session.ReplayFull(events)
	if err != nil {
		t.Fatalf("ReplayFull: %v", err)
	}
	if estTokens(full.Messages) <= estTokens(transcript.Messages) {
		t.Fatalf("ReplayFull should exceed the masked surface: full=%d surface=%d",
			estTokens(full.Messages), estTokens(transcript.Messages))
	}
	foundOriginal := false
	for _, msg := range full.Messages {
		for _, part := range msg.Parts {
			if part.ToolResult != nil && len(part.ToolResult.Content) > 0 &&
				len(part.ToolResult.Content[0].Text) == 8000 {
				foundOriginal = true
			}
		}
	}
	if !foundOriginal {
		t.Fatal("ReplayFull lost the original tool output")
	}
}

// TestLoopPruneCompactionSurfaceMatchesEventReplay covers Level 0: a
// medium tool output is middle-pruned inline, and the context.masked
// directive (carrying prunes) must replay to the exact runtime surface.
func TestLoopPruneCompactionSurfaceMatchesEventReplay(t *testing.T) {
	store := fakes.NewFakeStore()
	artifacts := openLoopArtifacts(t)
	tool := bigResultTool(10 * 1024) // inside the default [8KB, 16KB) prune band

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   30,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"next"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    3050,
			UsageOut:   30,
		},
		fakes.ScriptEntry{Text: "analysis done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register echo: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "check the log")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		// The post-prune surface (~1.4k est tokens) fits the 2000 target, so
		// no costlier level fires.
		Window: WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 2000},
		// Default MaskMinBytes (16KB): the 10KB output falls in the prune
		// band and must be pruned, never externalized.
		Condenser: Condenser{KeepRecentMessages: 2},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	counts := eventTypesIn(events)
	if counts[domain.EventContextMasked] != 1 || counts[domain.EventContextArchived] != 0 ||
		counts[domain.EventContextSummarized] != 0 {
		t.Fatalf("directives = %v, want exactly one context.masked", counts)
	}
	for _, evt := range events {
		if evt.Type != domain.EventContextMasked {
			continue
		}
		var payload domain.ContextMaskedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("unmarshal masked payload: %v", err)
		}
		if len(payload.Prunes) != 1 || len(payload.Masks) != 0 {
			t.Fatalf("payload = %d prunes / %d masks, want 1/0", len(payload.Prunes), len(payload.Masks))
		}
	}

	// The golden invariant holds for prunes too: replay from the pure log
	// reproduces the exact surface the loop held.
	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)

	// ReplayFull ignores directives: the original 10KB output survives there.
	full, err := session.ReplayFull(events)
	if err != nil {
		t.Fatalf("ReplayFull: %v", err)
	}
	foundOriginal := false
	for _, msg := range full.Messages {
		for _, part := range msg.Parts {
			if part.ToolResult != nil && len(part.ToolResult.Content) > 0 &&
				len(part.ToolResult.Content[0].Text) == 10*1024 {
				foundOriginal = true
			}
		}
	}
	if !foundOriginal {
		t.Fatal("ReplayFull lost the original tool output")
	}
}

func TestLoopSummaryCompactionSurfaceMatchesEventReplay(t *testing.T) {
	store := fakes.NewFakeStore()
	artifacts := openLoopArtifacts(t)
	tool := bigResultTool(2000)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    3050,
			UsageOut:   30,
		},
		// The compaction summarization call.
		fakes.ScriptEntry{Text: "HANDOFF SUMMARY", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 20},
		// The post-compaction main-loop call.
		fakes.ScriptEntry{Text: "continued after compaction", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "summarize my work")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		// KeepRecentMessages covers the whole transcript (no archival) and
		// MaskMinBytes exceeds every output (no masking), so the mechanical
		// levels cannot reach the tiny target and Level 3 fires.
		Condenser: Condenser{KeepRecentMessages: 50, MaskMinBytes: 1 << 20, Window: windowWithTarget(10)},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 3 {
		t.Fatalf("model calls = %d, want 3 (main, summarize, main)", calls)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	counts := eventTypesIn(events)
	if counts[domain.EventContextSummarized] != 1 {
		t.Fatalf("context.summarized events = %d, want 1 (types: %v)", counts[domain.EventContextSummarized], counts)
	}

	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)

	// The replayed surface is the summarized one plus the post-compaction
	// continuation: user message + bridge + final assistant answer.
	if len(transcript.Messages) != 3 {
		t.Fatalf("replayed %d messages, want 3 (user + summary bridge + continuation)", len(transcript.Messages))
	}

	// And the full-fidelity replay retains the pre-summary history.
	full, err := session.ReplayFull(events)
	if err != nil {
		t.Fatalf("ReplayFull: %v", err)
	}
	if len(full.Messages) <= len(transcript.Messages) {
		t.Fatalf("ReplayFull messages = %d, want more than the summarized surface %d",
			len(full.Messages), len(transcript.Messages))
	}
}

// TestCompactionDirectivesReplayedFromCheckpointTail covers the crash
// window: a compaction whose directive events landed in the log must be
// re-applied when the session is rebuilt from an EARLIER checkpoint plus
// the event tail (docs/SURFACE_DESIGN.md §5.2).
func TestCompactionDirectivesReplayedFromCheckpointTail(t *testing.T) {
	artifacts := openLoopArtifacts(t)
	store := fakes.NewFakeStore()
	tool := bigResultTool(8000)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 100, UsageOut: 30,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"next"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 3050, UsageOut: 30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)
	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register echo: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "check the log")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		Condenser: Condenser{KeepRecentMessages: 2, MaskMinBytes: 4096},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	// Find the compaction boundary: the checkpoint just BEFORE the masked
	// directive, then replay from it plus the tail.
	maskedIdx := -1
	for i, evt := range events {
		if evt.Type == domain.EventContextMasked {
			maskedIdx = i
			break
		}
	}
	if maskedIdx < 0 {
		t.Fatal("no context.masked event persisted")
	}
	// Simulate a stale checkpoint at the event just before the directive:
	// replay its prefix as the "checkpoint" state, then apply the tail.
	prefix, err := session.Replay(events[:maskedIdx])
	if err != nil {
		t.Fatalf("Replay prefix: %v", err)
	}
	staleCkpt := domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: run.SessionID,
		Sequence:  events[maskedIdx].Sequence - 1,
		Messages:  prefix.Messages,
	}
	fromTail, err := session.ReplayFromCheckpoint(staleCkpt, events[maskedIdx:])
	if err != nil {
		t.Fatalf("ReplayFromCheckpoint with directive tail: %v", err)
	}
	assertMessagesMatchRuntime(t, fromTail.Messages, run.Messages)
}

// TestLoopArchiveCompactionSurfaceMatchesEventReplay covers Level 2a: the
// oldest span is archived into a full-fidelity artifact and replaced by a
// marker; the context.archived directive must replay to the same surface.
func TestLoopArchiveCompactionSurfaceMatchesEventReplay(t *testing.T) {
	store := fakes.NewFakeStore()
	artifacts := openLoopArtifacts(t)
	tool := bigResultTool(400)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"a.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 100, UsageOut: 30,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"b.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 100, UsageOut: 30,
		},
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"c.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 3050, UsageOut: 30,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 15},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "read the logs")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		// MaskMinBytes exceeds every output so masking cannot help; the
		// target only fits the keep-recent window, forcing Level-2a
		// archival of the oldest span.
		Condenser: Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, Window: windowWithTarget(200)},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	counts := eventTypesIn(events)
	if counts[domain.EventContextArchived] != 1 {
		t.Fatalf("context.archived events = %d, want 1 (types: %v)", counts[domain.EventContextArchived], counts)
	}
	if counts[domain.EventContextMasked] != 0 || counts[domain.EventContextSummarized] != 0 {
		t.Fatalf("unexpected directives: %v", counts)
	}

	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)
	// The marker heads the surface.
	if transcript.Messages[0].Metadata["compacted"] != "archived" {
		t.Fatalf("first message should be the archive marker: %+v", transcript.Messages[0])
	}

	full, err := session.ReplayFull(events)
	if err != nil {
		t.Fatalf("ReplayFull: %v", err)
	}
	if len(full.Messages) <= len(transcript.Messages) {
		t.Fatalf("ReplayFull messages = %d, want more than the archived surface %d",
			len(full.Messages), len(transcript.Messages))
	}
}

// TestLoopSummaryOverflowRetryDirectivesMatchReplay covers the Level-3
// overflow retry: the summarize request itself overflows, the oldest span
// is archived (dropOps) and the retry succeeds. Both the drop and the
// summary directives must replay to the runtime surface.
func TestLoopSummaryOverflowRetryDirectivesMatchReplay(t *testing.T) {
	store := fakes.NewFakeStore()
	artifacts := openLoopArtifacts(t)
	tool := bigResultTool(2000)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 3050, UsageOut: 30,
		},
		// Summarize attempt 1 overflows the window.
		fakes.ScriptEntry{Error: "request failed: maximum context length exceeded"},
		// Summarize attempt 2 succeeds after archiving the oldest span.
		fakes.ScriptEntry{Text: "HANDOFF", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
		// Post-compaction continuation.
		fakes.ScriptEntry{Text: "continued", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "summarize my work")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver:  fakes.NewFakeApprover(domain.DecisionAllow),
		Registry:  registry,
		Logger:    slog.Default(),
		Artifacts: artifacts,
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		Condenser: Condenser{KeepRecentMessages: 50, MaskMinBytes: 1 << 20, Window: windowWithTarget(10)},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	if calls := len(model.Calls()); calls != 4 {
		t.Fatalf("model calls = %d, want 4 (main, overflowed summarize, summarize, main)", calls)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	counts := eventTypesIn(events)
	if counts[domain.EventContextArchived] != 1 {
		t.Fatalf("drop-op context.archived events = %d, want 1 (types: %v)", counts[domain.EventContextArchived], counts)
	}
	if counts[domain.EventContextSummarized] != 1 {
		t.Fatalf("context.summarized events = %d, want 1", counts[domain.EventContextSummarized])
	}

	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)
}

// TestLoopSummaryOverflowRetryWithoutArtifactStore covers the zero-artifact
// drop path: no artifact store, so the summary-overflow retry drops the
// oldest span with a marker only. The zero-artifact directive must still
// replay to the runtime surface.
func TestLoopSummaryOverflowRetryWithoutArtifactStore(t *testing.T) {
	store := fakes.NewFakeStore()
	tool := bigResultTool(2000)

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "big_output", Arguments: json.RawMessage(`{"path":"x.log"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 3050, UsageOut: 30,
		},
		fakes.ScriptEntry{Error: "maximum context length exceeded"},
		fakes.ScriptEntry{Text: "HANDOFF", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
		fakes.ScriptEntry{Text: "continued", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
	)

	registry := NewToolRegistry()
	if err := registry.Register(tool); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.Limits{MaxOutputTokens: 4096})
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "summarize my work")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry,
		Logger:   slog.Default(),
		// Artifacts deliberately nil: mechanical levels no-op, and the
		// overflow retry drops without preservation.
		Window:    WindowModel{Effective: 4000, CompactTrigger: 3000, CompactTarget: 1000},
		Condenser: Condenser{KeepRecentMessages: 50, MaskMinBytes: 1 << 20, Window: windowWithTarget(10)},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	// The drop directive carries a zero artifact.
	zeroArtifactDrops := 0
	for _, evt := range events {
		if evt.Type != domain.EventContextArchived {
			continue
		}
		var payload domain.ContextArchivedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("unmarshal archived payload: %v", err)
		}
		if payload.Artifact.ID.IsZero() {
			zeroArtifactDrops++
		}
	}
	if zeroArtifactDrops != 1 {
		t.Fatalf("zero-artifact drops = %d, want 1", zeroArtifactDrops)
	}

	transcript, err := session.Replay(events)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	assertMessagesMatchRuntime(t, transcript.Messages, run.Messages)
}
