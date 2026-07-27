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
// Created: 2026/07/24

package agent

import (
	"context"
	"encoding/json"
	"io"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// --- helpers ---

func toolCallMessage(name string) domain.Message {
	return domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolCall,
			ToolCall: &domain.ToolCall{
				ID:        domain.NewToolCallID(),
				Name:      name,
				Arguments: json.RawMessage(`{}`),
			},
		}},
		CreatedAt: time.Now(),
	}
}

func toolResultMessage(output string) domain.Message {
	return domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID:  domain.NewToolCallID(),
				Status:  domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{Kind: domain.PartText, Text: output}},
			},
		}},
		CreatedAt: time.Now(),
	}
}

func textMessage(role domain.Role, text string) domain.Message {
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      role,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		CreatedAt: time.Now(),
	}
}

func openArtifactStore(t *testing.T) *artifact.Store {
	t.Helper()
	store, err := artifact.Open(t.TempDir(), 1<<20)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	return store
}

func bigOutput(size int) string {
	return strings.Repeat("0123456789abcdef\n", size/17+1)[:size]
}

// --- masking ---

func TestCondenseMasksOversizedToolOutputs(t *testing.T) {
	store := openArtifactStore(t)
	original := bigOutput(8000)
	messages := []domain.Message{
		textMessage(domain.RoleUser, "please run the build"),
		toolCallMessage("run_cmd"),
		toolResultMessage(original),
		textMessage(domain.RoleAssistant, "build failed, fixing"),
		// recent window (6 messages) — must remain untouched
		toolResultMessage(bigOutput(9000)),
		textMessage(domain.RoleAssistant, "a"),
		textMessage(domain.RoleAssistant, "b"),
		textMessage(domain.RoleAssistant, "c"),
		textMessage(domain.RoleAssistant, "d"),
		textMessage(domain.RoleAssistant, "e"),
	}

	cond := Condenser{KeepRecentMessages: 6, MaskMinBytes: 4096}
	result := cond.Condense(context.Background(), &messages, store)

	if len(result.outputs) != 1 {
		t.Fatalf("masked outputs = %d, want 1", len(result.outputs))
	}
	if result.bytesMasked != len(original) {
		t.Fatalf("bytes masked = %d, want %d", result.bytesMasked, len(original))
	}

	masked := messages[2].Parts[0].ToolResult.Content[0].Text
	if !strings.HasPrefix(masked, compactedPlaceholderMark) {
		t.Fatalf("output not masked: %q", masked[:80])
	}
	if !strings.Contains(masked, store.Root()) {
		t.Fatalf("placeholder should carry the artifact path: %q", masked)
	}
	if messages[2].Revision != 1 {
		t.Fatalf("revision = %d, want 1", messages[2].Revision)
	}

	// The recent-window output stays inline.
	recent := messages[4].Parts[0].ToolResult.Content[0].Text
	if strings.HasPrefix(recent, compactedPlaceholderMark) {
		t.Fatal("recent-window output must not be masked")
	}
	if messages[4].Revision != 0 {
		t.Fatalf("recent message revision = %d, want 0", messages[4].Revision)
	}

	// The artifact holds the original bytes and is readable back through
	// the path embedded in the placeholder.
	path := strings.TrimSuffix(strings.SplitN(masked, "externalized to ", 2)[1], " — retrieve specific parts with run_cmd (cat/sed/grep) if needed]")
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("open externalized artifact: %v", err)
	}
	defer f.Close()
	data, err := io.ReadAll(f)
	if err != nil {
		t.Fatalf("read externalized artifact: %v", err)
	}
	if string(data) != original {
		t.Fatalf("artifact content mismatch: got %d bytes", len(data))
	}
}

func TestCondenseSkipsSmallOutputsAndIsIdempotent(t *testing.T) {
	store := openArtifactStore(t)
	messages := []domain.Message{
		toolResultMessage("small output"),
		textMessage(domain.RoleAssistant, "done"),
	}
	// KeepRecentMessages 1 protects only the trailing message; note that a
	// zero value selects the documented default window instead of "none".
	cond := Condenser{KeepRecentMessages: 1, MaskMinBytes: 4096}

	result := cond.Condense(context.Background(), &messages, store)
	if len(result.outputs) != 0 {
		t.Fatalf("small output should not be masked: %+v", result.outputs)
	}

	// Mask once, then run again: the placeholder mark prevents re-masking.
	messages = []domain.Message{
		toolResultMessage(bigOutput(6000)),
		textMessage(domain.RoleAssistant, "done"),
	}
	first := cond.Condense(context.Background(), &messages, store)
	if len(first.outputs) != 1 {
		t.Fatalf("first pass masked %d, want 1", len(first.outputs))
	}
	second := cond.Condense(context.Background(), &messages, store)
	if len(second.outputs) != 0 {
		t.Fatalf("second pass masked %d, want 0 (idempotent)", len(second.outputs))
	}
}

func TestCondensePreservesToolPairing(t *testing.T) {
	store := openArtifactStore(t)
	messages := []domain.Message{
		toolCallMessage("run_cmd"),
		toolResultMessage(bigOutput(6000)),
		toolCallMessage("read_file"),
		toolResultMessage(bigOutput(7000)),
	}
	cond := Condenser{KeepRecentMessages: 1, MaskMinBytes: 4096}
	cond.Condense(context.Background(), &messages, store)

	// Every assistant tool_call must still have a following tool result.
	calls := 0
	results := 0
	for _, msg := range messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolCall {
				calls++
			}
			if part.Kind == domain.PartToolResult {
				results++
			}
		}
	}
	if calls != 2 || results != 2 {
		t.Fatalf("pairing broken: %d calls vs %d results", calls, results)
	}
}

func TestCondenseNilStoreIsNoop(t *testing.T) {
	messages := []domain.Message{
		toolResultMessage(bigOutput(6000)),
	}
	result := Condenser{KeepRecentMessages: 0, MaskMinBytes: 4096}.Condense(context.Background(), &messages, nil)
	if len(result.outputs) != 0 {
		t.Fatalf("nil store must disable masking: %+v", result.outputs)
	}
}

// --- Loop.compact integration ---

func TestLoopCompactMasksAndRecordsEvent(t *testing.T) {
	store := openArtifactStore(t)
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxInputTokens: 200_000}, domain.RealClock{})
	run.AddUserMessage(textMessage(domain.RoleUser, "run the tests"))
	run.Messages = append(
		run.Messages,
		toolResultMessage(bigOutput(8000)),
		textMessage(domain.RoleAssistant, "analyzing"),
	)

	loop := &Loop{Run: run, Artifacts: store, Condenser: Condenser{KeepRecentMessages: 1, MaskMinBytes: 4096}}
	run.Usage.InputTokens = 150_000
	// compact() is only ever invoked from the compacting phase.
	run.State.Phase = domain.PhaseCompacting

	before := estTokens(run.Messages)
	if err := loop.compact(context.Background()); err != nil {
		t.Fatalf("compact() error = %v", err)
	}
	after := estTokens(run.Messages)

	if after >= before {
		t.Fatalf("compact did not shrink the transcript: before=%d after=%d", before, after)
	}
	masked := run.Messages[1].Parts[0].ToolResult.Content[0].Text
	if !strings.HasPrefix(masked, compactedPlaceholderMark) {
		t.Fatalf("output not masked: %q", masked[:80])
	}

	// The compaction event carries the audit facts.
	var found *domain.Event
	for i := range run.pendingEvents {
		if run.pendingEvents[i].Type == domain.EventContextCompacted {
			found = &run.pendingEvents[i]
		}
	}
	if found == nil {
		t.Fatal("EventContextCompacted not recorded")
	}
	var payload contextCompactedPayload
	if err := json.Unmarshal(found.Payload, &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload.MaskedOutputs != 1 || payload.MaskedBytes != 8000 {
		t.Fatalf("unexpected payload: %+v", payload)
	}
	if payload.EstTokensAfter >= payload.EstTokensBefore {
		t.Fatalf("payload estimates not shrinking: %+v", payload)
	}
	if len(payload.Outputs) != 1 || payload.Outputs[0].Bytes != 8000 {
		t.Fatalf("payload outputs: %+v", payload.Outputs)
	}

	// Phase transitioned back to preparing for the next model call.
	if run.State.Phase != domain.PhasePreparing {
		t.Fatalf("phase = %v, want preparing", run.State.Phase)
	}
}

// --- Level 2: span archival ---

func TestCondenseArchivesOldestSpan(t *testing.T) {
	store := openArtifactStore(t)
	var messages []domain.Message
	for i := 0; i < 10; i++ {
		messages = append(messages, textMessage(domain.RoleAssistant, strings.Repeat("x", 400)))
	}

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, TargetTokens: 250}
	result := cond.Condense(context.Background(), &messages, store)

	if result.archived != 8 {
		t.Fatalf("archived = %d, want 8", result.archived)
	}
	if len(messages) != 3 {
		t.Fatalf("len(messages) = %d, want 3 (marker + window)", len(messages))
	}

	marker := messages[0]
	if marker.Role != domain.RoleSystem || marker.Metadata["compacted"] != "archived" {
		t.Fatalf("marker metadata wrong: %+v", marker.Metadata)
	}
	text := marker.Parts[0].Text
	if !strings.HasPrefix(text, archivedSpanMark) || !strings.Contains(text, "8 messages") {
		t.Fatalf("marker text wrong: %q", text)
	}
	if !strings.Contains(text, store.Root()) {
		t.Fatalf("marker should carry the artifact path: %q", text)
	}
	if est := estTokens(messages); est > 250+len(text)/4 {
		t.Fatalf("estimate after archival = %d, want ~target", est)
	}

	// The archive artifact is a full-fidelity JSON-lines transcript.
	path := strings.TrimSuffix(strings.SplitN(text, "externalized to ", 2)[1], " — read specific parts with run_cmd (cat/sed/grep/jq) if needed.]")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read archive artifact: %v", err)
	}
	lines := strings.Split(strings.TrimSpace(string(data)), "\n")
	if len(lines) != 8 {
		t.Fatalf("archive lines = %d, want 8", len(lines))
	}
	var decoded domain.Message
	if err := json.Unmarshal([]byte(lines[0]), &decoded); err != nil {
		t.Fatalf("archive line 0 is not a message: %v", err)
	}
	if decoded.Parts[0].Text != strings.Repeat("x", 400) {
		t.Fatalf("archive lost message content")
	}
}

// Regression: archival punches a hole in the sequence numbering. Messages
// appended later are numbered len(messages)+1, so sparse survivor sequences
// collide with them ("sequence N already assigned to message ...") and brick
// session recovery on the next continuation. Condense must hand back a
// densely renumbered 1..N list.
func TestCondenseRenumbersSequencesDensely(t *testing.T) {
	store := openArtifactStore(t)
	var messages []domain.Message
	for i := 0; i < 10; i++ {
		msg := textMessage(domain.RoleAssistant, strings.Repeat("x", 400))
		msg.Sequence = int64(i + 1)
		messages = append(messages, msg)
	}

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, TargetTokens: 250}
	result := cond.Condense(context.Background(), &messages, store)
	if result.archived == 0 {
		t.Fatal("expected archival to happen")
	}

	marker := messages[0]
	if marker.Sequence != 1 {
		t.Fatalf("marker sequence = %d, want 1 (dense renumbering)", marker.Sequence)
	}
	for i, msg := range messages {
		if msg.Sequence != int64(i+1) {
			t.Fatalf("messages[%d].Sequence = %d, want %d (dense)", i, msg.Sequence, i+1)
		}
	}

	// The post-compaction append path numbers the next message len+1; it
	// must not collide with any survivor.
	next := int64(len(messages) + 1)
	for _, msg := range messages {
		if msg.Sequence == next {
			t.Fatalf("next appended sequence %d collides with message %s", next, msg.ID)
		}
	}
}

// toolPair builds a tool_call message and its result message sharing one
// call ID, as the agent loop records them.
func toolPair(name, output string) []domain.Message {
	callID := domain.NewToolCallID()
	call := domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind:     domain.PartToolCall,
			ToolCall: &domain.ToolCall{ID: callID, Name: name, Arguments: json.RawMessage(`{}`)},
		}},
		CreatedAt: time.Now(),
	}
	result := domain.Message{
		ID:   domain.NewMessageID(),
		Role: domain.RoleAssistant,
		Parts: []domain.ContentPart{{
			Kind: domain.PartToolResult,
			ToolResult: &domain.ToolResult{
				CallID:  callID,
				Status:  domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{Kind: domain.PartText, Text: output}},
			},
		}},
		CreatedAt: time.Now(),
	}
	return []domain.Message{call, result}
}

func TestPairingSafeCutBoundaries(t *testing.T) {
	var messages []domain.Message
	messages = append(messages, toolPair("run_cmd", "first")...)
	messages = append(messages, toolPair("read_file", "second")...)
	messages = append(messages, textMessage(domain.RoleAssistant, "tail"))

	if cut := pairingSafeCut(messages, 1, 4); cut != 2 {
		t.Fatalf("pairingSafeCut(1) = %d, want 2 (call/result not split)", cut)
	}
	if cut := pairingSafeCut(messages, 3, 4); cut != 4 {
		t.Fatalf("pairingSafeCut(3) = %d, want 4", cut)
	}
	if !isPairingSafe(messages, 0) {
		t.Fatal("cut at 0 is trivially safe")
	}
}

func TestCondenseMasksIntoWindowWhenTailHeavy(t *testing.T) {
	store := openArtifactStore(t)
	messages := []domain.Message{
		textMessage(domain.RoleUser, "fix the build"),
		toolResultMessage(bigOutput(1000)),
		toolResultMessage(bigOutput(1000)),
		textMessage(domain.RoleAssistant, "working on it"),
	}

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 100, TargetTokens: 200}
	result := cond.Condense(context.Background(), &messages, store)

	// Level 1 masks the first output; the window alone stays above target,
	// so Level 2b masks the second one too. The final message is untouched.
	if len(result.outputs) != 2 {
		t.Fatalf("masked outputs = %d, want 2", len(result.outputs))
	}
	if messages[3].Parts[0].Text != "working on it" {
		t.Fatal("final message must remain untouched")
	}
	if est := estTokens(messages); est > 200 {
		t.Fatalf("estimate after window masking = %d, want ≤ 200", est)
	}
}

// --- trigger decision ---

func TestShouldCompactTriggers(t *testing.T) {
	newLoop := func(target int) *Loop {
		return &Loop{
			Run:       NewRun(domain.NewSessionID(), domain.Limits{MaxInputTokens: 200_000}, domain.RealClock{}),
			Condenser: Condenser{TargetTokens: target},
		}
	}

	t.Run("idle run does not compact", func(t *testing.T) {
		loop := newLoop(1000)
		if loop.shouldCompact() {
			t.Fatal("no pressure, no compaction")
		}
	})

	t.Run("context pressure ignores usage", func(t *testing.T) {
		loop := newLoop(50)
		loop.Run.Messages = append(loop.Run.Messages, textMessage(domain.RoleAssistant, strings.Repeat("x", 400)))
		if !loop.shouldCompact() {
			t.Fatal("oversized transcript should trigger compaction with zero usage")
		}
	})

	t.Run("occupancy pressure triggers at 80 percent of the window", func(t *testing.T) {
		loop := newLoop(1 << 30)
		loop.lastCallInput = 100_000 // 50% of the 200k window
		if loop.shouldCompact() {
			t.Fatal("occupancy below 80% of the window must not trigger compaction")
		}
		loop.lastCallInput = 170_000 // 85% → over the threshold
		if !loop.shouldCompact() {
			t.Fatal("occupancy at 85% of the window should trigger compaction")
		}
	})

	t.Run("forced compaction after provider context overflow", func(t *testing.T) {
		loop := newLoop(1 << 30)
		loop.ForceCompact = true
		if !loop.shouldCompact() {
			t.Fatal("forceCompact must trigger compaction with no other pressure")
		}
	})
}

// --- small units ---

func TestEstTokens(t *testing.T) {
	messages := []domain.Message{
		textMessage(domain.RoleUser, strings.Repeat("a", 400)),
		toolResultMessage(strings.Repeat("b", 400)),
	}
	if got := estTokens(messages); got != 200 {
		t.Fatalf("estTokens = %d, want 200", got)
	}
}

func TestHumanBytes(t *testing.T) {
	if got := humanBytes(512); got != "512B" {
		t.Fatalf("humanBytes(512) = %q", got)
	}
	if got := humanBytes(4096); got != "4.0KB" {
		t.Fatalf("humanBytes(4096) = %q", got)
	}
	if got := humanBytes(3 << 20); got != "3.0MB" {
		t.Fatalf("humanBytes(3MB) = %q", got)
	}
}

// --- Summarizing compaction (Level 3) ---

func TestBuildSummaryReplacement(t *testing.T) {
	now := time.Now().UTC()
	messages := []domain.Message{
		textMessage(domain.RoleUser, "first task"),
		textMessage(domain.RoleAssistant, "working on it"),
		toolResultMessage(bigOutput(8000)),
		textMessage(domain.RoleUser, "second task"),
		{
			ID: domain.NewMessageID(), Role: domain.RoleSystem, Status: domain.MessageStatusFinal, Revision: 1, Sequence: 5,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "[budget notice] ..."}}, CreatedAt: now,
		},
		{
			ID: domain.NewMessageID(), Role: domain.RoleUser, Status: domain.MessageStatusFinal, Revision: 1, Sequence: 6,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: CompactionSummaryPrefix + "\nold summary"}}, CreatedAt: now,
			Metadata: map[string]string{"compacted": compactedSummaryMeta},
		},
	}

	replacement := buildSummaryReplacement(messages, "HANDOFF", now)

	// Only user-role messages remain: the two real user messages (chronological)
	// plus the summary bridge; assistant/tool/system messages and the old
	// bridge are gone.
	if len(replacement) != 3 {
		t.Fatalf("replacement len = %d, want 3: %+v", len(replacement), replacement)
	}
	for i, msg := range replacement {
		if msg.Role != domain.RoleUser {
			t.Fatalf("replacement[%d].Role = %s, want user", i, msg.Role)
		}
		if msg.Sequence != int64(i+1) {
			t.Fatalf("replacement[%d].Sequence = %d, want %d", i, msg.Sequence, i+1)
		}
	}
	if replacement[0].TextParts()[0] != "first task" || replacement[1].TextParts()[0] != "second task" {
		t.Fatalf("real user messages not preserved chronologically: %+v", replacement)
	}
	bridge := replacement[2]
	if bridge.Metadata["compacted"] != compactedSummaryMeta {
		t.Fatalf("bridge metadata = %v, want compacted=%s", bridge.Metadata, compactedSummaryMeta)
	}
	text := bridge.TextParts()[0]
	if !strings.HasPrefix(text, CompactionSummaryPrefix) || !strings.Contains(text, "HANDOFF") {
		t.Fatalf("bridge text missing prefix/summary: %q", text[:120])
	}
}

func TestLoopCompactSummarizesWhenMaskingInsufficient(t *testing.T) {
	store := openArtifactStore(t)
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxInputTokens: 200_000, MaxOutputTokens: 4096}, domain.RealClock{})
	run.AddUserMessage(textMessage(domain.RoleUser, "task one"))
	run.Messages = append(
		run.Messages,
		toolResultMessage(bigOutput(8000)),
		textMessage(domain.RoleAssistant, "analyzing"),
	)
	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text: "HANDOFF SUMMARY", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 20,
	})
	loop := &Loop{
		Run: run, Model: model, Artifacts: store,
		// KeepRecentMessages covers the whole transcript so no archival
		// drops the user message before summarization; the tiny
		// TargetTokens still forces the summarization path.
		Condenser: Condenser{KeepRecentMessages: 10, MaskMinBytes: 4096, TargetTokens: 10},
	}
	run.State.Phase = domain.PhaseCompacting

	if err := loop.compact(context.Background()); err != nil {
		t.Fatalf("compact() error = %v", err)
	}

	// The transcript is rebuilt around the model-written summary: the real
	// user message plus the summary bridge, nothing else.
	if len(run.Messages) != 2 {
		t.Fatalf("messages after summarization = %d, want 2: %+v", len(run.Messages), run.Messages)
	}
	if got := run.Messages[0].TextParts()[0]; got != "task one" {
		t.Fatalf("user message = %q, want task one", got)
	}
	bridge := run.Messages[1]
	if bridge.Metadata["compacted"] != compactedSummaryMeta ||
		!strings.Contains(bridge.TextParts()[0], "HANDOFF SUMMARY") {
		t.Fatalf("summary bridge missing: %+v", bridge)
	}
	if calls := len(model.Calls()); calls != 1 {
		t.Fatalf("summarization model calls = %d, want 1", calls)
	}
	if run.Usage.InputTokens != 100 || run.Usage.OutputTokens != 20 {
		t.Fatalf("summarization usage not accounted: %+v", run.Usage)
	}

	// The audit event records the summarized compaction.
	var payload contextCompactedPayload
	for _, evt := range run.pendingEvents {
		if evt.Type == domain.EventContextCompacted {
			if err := json.Unmarshal(evt.Payload, &payload); err != nil {
				t.Fatalf("unmarshal compaction payload: %v", err)
			}
		}
	}
	if !payload.Summarized || payload.SummaryBytes != len("HANDOFF SUMMARY") {
		t.Fatalf("compaction payload = %+v, want summarized with summary bytes", payload)
	}
}
