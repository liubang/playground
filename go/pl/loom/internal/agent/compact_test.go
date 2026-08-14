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
	"unicode/utf8"

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

// windowWithTarget builds a usable WindowModel whose compaction target is
// the given token count (trigger/target ratios preserved).
func windowWithTarget(target int64) WindowModel {
	return WindowModel{
		Effective:      target * 4,
		CompactTrigger: target * 3,
		CompactTarget:  target,
	}
}

func bigOutput(size int) string {
	return strings.Repeat("0123456789abcdef\n", size/17+1)[:size]
}

// condense applies one compaction pass the way the loop does (docs/
// SURFACE_DESIGN.md §4.2): Plan emits pure directives, the shared domain
// application function produces the new surface. Audit counters derive
// from the returned ops (maskCount et al. below).
func condense(t *testing.T, cond Condenser, messages *[]domain.Message, store domain.ArtifactStore) domain.SurfaceOps {
	t.Helper()
	ops := cond.Plan(context.Background(), *messages, store, time.Now().UTC())
	if !ops.Empty() {
		applied, err := domain.ApplySurfaceOps(*messages, ops)
		if err != nil {
			t.Fatalf("ApplySurfaceOps() error = %v", err)
		}
		*messages = applied
	}
	return ops
}

// maskCount is the number of masked outputs in the pass.
func maskCount(ops domain.SurfaceOps) int {
	if ops.Masks == nil {
		return 0
	}
	return len(ops.Masks.Masks)
}

// maskedBytes is the total original size of masked outputs.
func maskedBytes(ops domain.SurfaceOps) int {
	total := 0
	if ops.Masks != nil {
		for _, mask := range ops.Masks.Masks {
			total += mask.OriginalBytes
		}
	}
	return total
}

// archivedCount is the number of messages replaced by the archive marker.
func archivedCount(ops domain.SurfaceOps) int {
	if ops.Archive == nil {
		return 0
	}
	return int(ops.Archive.ToSequence - ops.Archive.FromSequence + 1)
}

// --- masking ---

func TestCondenseMasksOversizedToolOutputs(t *testing.T) {
	store := openArtifactStore(t)
	original := bigOutput(8000)
	messages := []domain.Message{
		textMessage(domain.RoleUser, "please run the build"),
	}
	messages = append(messages, toolPair("run_cmd", original)...)
	messages = append(messages, textMessage(domain.RoleAssistant, "build failed, fixing"))
	// recent window (6 messages) — must remain untouched
	messages = append(messages, toolPair("run_cmd", bigOutput(9000))...)
	messages = append(
		messages,
		textMessage(domain.RoleAssistant, "b"),
		textMessage(domain.RoleAssistant, "c"),
		textMessage(domain.RoleAssistant, "d"),
		textMessage(domain.RoleAssistant, "e"),
	)

	cond := Condenser{KeepRecentMessages: 6, MaskMinBytes: 4096}
	ops := condense(t, cond, &messages, store)

	if maskCount(ops) != 1 {
		t.Fatalf("masked outputs = %d, want 1", maskCount(ops))
	}
	if maskedBytes(ops) != len(original) {
		t.Fatalf("bytes masked = %d, want %d", maskedBytes(ops), len(original))
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
	recent := messages[5].Parts[0].ToolResult.Content[0].Text
	if strings.HasPrefix(recent, compactedPlaceholderMark) {
		t.Fatal("recent-window output must not be masked")
	}
	if messages[5].Revision != 0 {
		t.Fatalf("recent message revision = %d, want 0", messages[5].Revision)
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

// Regression (REVIEW H1): a masked tool output was previously referenced
// only from the placeholder TEXT, which checkpointArtifactRefs never scans —
// `loom gc` could reclaim the artifact and leave a dead pointer in the
// transcript. The masked result must carry a PartArtifact reference (both
// providers skip PartArtifact when rendering, so the wire form is unchanged)
// so session persistence tracks it for garbage collection.
func TestCondenseMaskRegistersArtifactRef(t *testing.T) {
	store := openArtifactStore(t)
	messages := []domain.Message{
		textMessage(domain.RoleUser, "please run the build"),
	}
	messages = append(messages, toolPair("run_cmd", bigOutput(8000))...)
	messages = append(
		messages,
		textMessage(domain.RoleAssistant, "a"),
		textMessage(domain.RoleAssistant, "b"),
		textMessage(domain.RoleAssistant, "c"),
		textMessage(domain.RoleAssistant, "d"),
		textMessage(domain.RoleAssistant, "e"),
		textMessage(domain.RoleAssistant, "f"),
	)

	cond := Condenser{KeepRecentMessages: 6, MaskMinBytes: 4096}
	ops := condense(t, cond, &messages, store)
	if maskCount(ops) != 1 {
		t.Fatalf("masked outputs = %d, want 1", maskCount(ops))
	}

	content := messages[2].Parts[0].ToolResult.Content
	var ref *domain.ArtifactRef
	for i := range content {
		if content[i].Kind == domain.PartArtifact {
			ref = content[i].Artifact
		}
	}
	if ref == nil {
		t.Fatal("masked tool result must carry a PartArtifact reference so GC keeps the artifact")
	}
	if ref.ID != ops.Masks.Masks[0].Artifact.ID {
		t.Fatalf("artifact ref = %s, want masked output %s", ref.ID, ops.Masks.Masks[0].Artifact.ID)
	}
	if err := messages[2].Validate(); err != nil {
		t.Fatalf("masked message must stay valid: %v", err)
	}
}

// Regression (REVIEW H1): archiving replaces a message span with a marker,
// dropping every artifact reference the span carried (masked outputs, run_cmd
// overflow artifacts) plus the archive artifact itself from the checkpoint.
// The marker must record all of them in Metadata so checkpointArtifactRefs
// keeps them alive.
func TestCondenseArchiveMarkerCarriesArtifactRefs(t *testing.T) {
	store := openArtifactStore(t)
	innerRef := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 10}

	var messages []domain.Message
	refMsg := textMessage(domain.RoleAssistant, strings.Repeat("x", 400))
	refMsg.Sequence = 1
	refMsg.Parts = append(refMsg.Parts, domain.ContentPart{Kind: domain.PartArtifact, Artifact: &innerRef})
	messages = append(messages, refMsg)
	for i := 0; i < 9; i++ {
		msg := textMessage(domain.RoleAssistant, strings.Repeat("x", 400))
		msg.Sequence = int64(i + 2)
		messages = append(messages, msg)
	}

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, Window: windowWithTarget(250)}
	ops := condense(t, cond, &messages, store)
	if archivedCount(ops) == 0 {
		t.Fatal("expected archival to happen")
	}

	encoded := messages[0].Metadata[domain.MetadataCompactedArtifacts]
	if encoded == "" {
		t.Fatal("archive marker must record the artifact refs it depends on for GC tracking")
	}
	var refs []domain.ArtifactRef
	if err := json.Unmarshal([]byte(encoded), &refs); err != nil {
		t.Fatalf("marker artifact refs are not valid JSON: %v", err)
	}
	foundInner, foundArchive := false, false
	for _, r := range refs {
		if r.ID == innerRef.ID {
			foundInner = true
		}
		if r.ID != innerRef.ID && !r.ID.IsZero() {
			foundArchive = true
		}
	}
	if !foundInner {
		t.Fatalf("marker refs %v missing archived span ref %s", refs, innerRef.ID)
	}
	if !foundArchive {
		t.Fatalf("marker refs %v missing the archive artifact itself", refs)
	}
}

func TestCondenseSkipsSmallOutputsAndIsIdempotent(t *testing.T) {
	store := openArtifactStore(t)
	messages := toolPair("run_cmd", "small output")
	messages = append(messages, textMessage(domain.RoleAssistant, "done"))
	// KeepRecentMessages 1 protects only the trailing message; note that a
	// zero value selects the documented default window instead of "none".
	cond := Condenser{KeepRecentMessages: 1, MaskMinBytes: 4096}

	ops := condense(t, cond, &messages, store)
	if maskCount(ops) != 0 {
		t.Fatalf("small output should not be masked: %+v", ops.Masks)
	}

	// Mask once, then run again: the placeholder mark prevents re-masking.
	messages = toolPair("run_cmd", bigOutput(6000))
	messages = append(messages, textMessage(domain.RoleAssistant, "done"))
	first := condense(t, cond, &messages, store)
	if maskCount(first) != 1 {
		t.Fatalf("first pass masked %d, want 1", maskCount(first))
	}
	second := condense(t, cond, &messages, store)
	if maskCount(second) != 0 {
		t.Fatalf("second pass masked %d, want 0 (idempotent)", maskCount(second))
	}
}

// --- pruning (Level 0) ---

// pruneCount is the number of inline-pruned outputs in the pass.
func pruneCount(ops domain.SurfaceOps) int {
	if ops.Masks == nil {
		return 0
	}
	return len(ops.Masks.Prunes)
}

func TestCondensePrunesMediumToolOutputs(t *testing.T) {
	store := openArtifactStore(t)
	original := bigOutput(10 * 1024) // inside the default [8KB, 16KB) prune band
	messages := []domain.Message{
		textMessage(domain.RoleUser, "please run the build"),
	}
	messages = append(messages, toolPair("run_cmd", original)...)
	// recent window (6 messages) — must remain untouched
	messages = append(messages, toolPair("run_cmd", bigOutput(9*1024))...)
	messages = append(
		messages,
		textMessage(domain.RoleAssistant, "b"),
		textMessage(domain.RoleAssistant, "c"),
		textMessage(domain.RoleAssistant, "d"),
		textMessage(domain.RoleAssistant, "e"),
	)

	cond := Condenser{KeepRecentMessages: 6}
	ops := condense(t, cond, &messages, store)

	if maskCount(ops) != 0 {
		t.Fatalf("masked outputs = %d, want 0 (a band output is pruned, not externalized)", maskCount(ops))
	}
	if pruneCount(ops) != 1 {
		t.Fatalf("pruned outputs = %d, want 1", pruneCount(ops))
	}

	pruned := messages[2].Parts[0].ToolResult.Content[0].Text
	if !strings.HasPrefix(pruned, original[:100]) {
		t.Fatalf("pruned output lost its head: %q", pruned[:80])
	}
	if !strings.Contains(pruned, prunedMiddleMark) {
		t.Fatalf("pruned output misses the marker: %q", pruned[:200])
	}
	if !strings.HasSuffix(pruned, original[len(original)-100:]) {
		t.Fatal("pruned output lost its tail")
	}
	if !utf8.ValidString(pruned) {
		t.Fatal("pruned output is not valid UTF-8")
	}
	if len(pruned) >= len(original) {
		t.Fatalf("pruned output = %d bytes, want smaller than %d", len(pruned), len(original))
	}
	if len(messages[2].Parts[0].ToolResult.Content) != 1 {
		t.Fatal("a prune must not append an artifact reference")
	}
	if messages[2].Revision != 1 {
		t.Fatalf("revision = %d, want 1", messages[2].Revision)
	}

	// The recent-window output stays inline.
	recent := messages[4].Parts[0].ToolResult.Content[0].Text
	if strings.Contains(recent, prunedMiddleMark) {
		t.Fatal("recent-window output must not be pruned")
	}

	// A second pass is idempotent: the pruned form is below the band and
	// carries the marker.
	second := condense(t, cond, &messages, store)
	if pruneCount(second) != 0 || maskCount(second) != 0 {
		t.Fatalf("second pass pruned %d masked %d, want 0/0 (idempotent)", pruneCount(second), maskCount(second))
	}
}

func TestCondensePruneBandEdges(t *testing.T) {
	store := openArtifactStore(t)
	// Below the band: stays verbatim. At/above MaskMinBytes: externalized,
	// not pruned (full-fidelity preservation wins for very large outputs).
	small := bigOutput(8*1024 - 1)
	huge := bigOutput(16 * 1024)
	messages := toolPair("run_cmd", small)
	messages = append(messages, toolPair("run_cmd", huge)...)
	messages = append(messages, textMessage(domain.RoleAssistant, "done"))

	cond := Condenser{KeepRecentMessages: 1}
	ops := condense(t, cond, &messages, store)

	if pruneCount(ops) != 0 {
		t.Fatalf("pruned = %d, want 0 (below-band verbatim, at-mask-threshold externalized)", pruneCount(ops))
	}
	if maskCount(ops) != 1 || maskedBytes(ops) != len(huge) {
		t.Fatalf("masked = %d (%d bytes), want 1 (%d bytes)", maskCount(ops), maskedBytes(ops), len(huge))
	}
	if got := messages[1].Parts[0].ToolResult.Content[0].Text; got != small {
		t.Fatalf("below-band output changed: %d bytes, want %d verbatim", len(got), len(small))
	}
}

// Level 0 needs no artifact store: a nil store disables masking/archival
// but pruning still shrinks the surface.
func TestCondensePrunesWithoutArtifactStore(t *testing.T) {
	messages := toolPair("run_cmd", bigOutput(10*1024))
	messages = append(messages, textMessage(domain.RoleAssistant, "done"))
	ops := condense(t, Condenser{KeepRecentMessages: 1}, &messages, nil)
	if pruneCount(ops) != 1 {
		t.Fatalf("pruned = %d, want 1 without an artifact store", pruneCount(ops))
	}
	if maskCount(ops) != 0 {
		t.Fatalf("masked = %d, want 0 without an artifact store", maskCount(ops))
	}
}

func TestCondensePreservesToolPairing(t *testing.T) {
	store := openArtifactStore(t)
	var messages []domain.Message
	messages = append(messages, toolPair("run_cmd", bigOutput(6000))...)
	messages = append(messages, toolPair("read_file", bigOutput(7000))...)
	cond := Condenser{KeepRecentMessages: 1, MaskMinBytes: 4096}
	condense(t, cond, &messages, store)

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
	ops := condense(t, Condenser{KeepRecentMessages: 0, MaskMinBytes: 4096}, &messages, nil)
	if maskCount(ops) != 0 {
		t.Fatalf("nil store must disable masking: %+v", ops.Masks)
	}
}

// --- Loop.compact integration ---

func TestLoopCompactMasksAndRecordsEvent(t *testing.T) {
	store := openArtifactStore(t)
	run := NewRun(domain.NewSessionID(), domain.Limits{MaxInputTokens: 200_000}, domain.RealClock{})
	run.AddUserMessage(textMessage(domain.RoleUser, "run the tests"))
	run.Messages = append(
		run.Messages,
		toolPair("run_cmd", bigOutput(8000))...,
	)
	run.Messages = append(run.Messages, textMessage(domain.RoleAssistant, "analyzing"))

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
	masked := run.Messages[2].Parts[0].ToolResult.Content[0].Text
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

// Regression (REVIEW R10): the byte-budget truncation in
// buildSummaryReplacement could split a multi-byte rune, persisting invalid
// UTF-8 into the compacted transcript (and later into provider requests).
func TestBuildSummaryReplacementTruncatesAtRuneBoundary(t *testing.T) {
	// 81920 % 3 = 2: the byte-budget cut lands inside a 3-byte rune.
	cjk := strings.Repeat("中", 30000) // 90000 bytes > summaryUserMessageMaxBytes
	messages := []domain.Message{
		textMessage(domain.RoleUser, cjk),
	}
	out, _ := buildSummaryReplacement(messages, "summary", time.Now(), 80*1024)
	if len(out) != 2 {
		t.Fatalf("replacement messages = %d, want 2 (user + summary bridge)", len(out))
	}
	text := strings.Join(out[0].TextParts(), "")
	if !utf8.ValidString(text) {
		t.Fatal("truncated user message is not valid UTF-8")
	}
	if !strings.Contains(text, "earlier part of this message truncated") {
		t.Fatal("truncation marker missing")
	}
}

// --- Level 2: span archival ---

func TestCondenseArchivesOldestSpan(t *testing.T) {
	store := openArtifactStore(t)
	var messages []domain.Message
	for i := 0; i < 10; i++ {
		msg := textMessage(domain.RoleAssistant, strings.Repeat("x", 400))
		msg.Sequence = int64(i + 1)
		messages = append(messages, msg)
	}

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, Window: windowWithTarget(250)}
	ops := condense(t, cond, &messages, store)

	if archivedCount(ops) != 8 {
		t.Fatalf("archived = %d, want 8", archivedCount(ops))
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

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 1 << 20, Window: windowWithTarget(250)}
	ops := condense(t, cond, &messages, store)
	if archivedCount(ops) == 0 {
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
	var messages []domain.Message
	messages = append(messages, textMessage(domain.RoleUser, "fix the build"))
	messages = append(messages, toolPair("run_cmd", bigOutput(1000))...)
	messages = append(messages, toolPair("run_cmd", bigOutput(1000))...)
	messages = append(messages, textMessage(domain.RoleAssistant, "working on it"))

	cond := Condenser{KeepRecentMessages: 2, MaskMinBytes: 100, Window: windowWithTarget(200)}
	ops := condense(t, cond, &messages, store)

	// Both tool results sit outside the keep-recent window and are masked;
	// the final message is untouched.
	if maskCount(ops) != 2 {
		t.Fatalf("masked outputs = %d, want 2", maskCount(ops))
	}
	if messages[5].Parts[0].Text != "working on it" {
		t.Fatal("final message must remain untouched")
	}
	if est := estTokens(messages); est > 200 {
		t.Fatalf("estimate after window masking = %d, want ≤ 200", est)
	}
}

// --- trigger decision ---

func TestShouldCompactTriggers(t *testing.T) {
	newLoop := func() *Loop {
		return &Loop{
			Run:    NewRun(domain.NewSessionID(), domain.Limits{MaxInputTokens: 200_000}, domain.RealClock{}),
			Window: WindowModel{Effective: 1000, CompactTrigger: 800, CompactTarget: 500},
		}
	}

	t.Run("idle run does not compact", func(t *testing.T) {
		if newLoop().shouldCompact() {
			t.Fatal("no pressure, no compaction")
		}
	})

	t.Run("cold-start estimate triggers past the window trigger", func(t *testing.T) {
		// With no metered input yet, occupancy is the pure transcript
		// estimate — the cold-start path (docs/CONTEXT_DESIGN.md §4.2).
		loop := newLoop()
		loop.Run.Messages = append(loop.Run.Messages, textMessage(domain.RoleAssistant, strings.Repeat("x", 4000)))
		if !loop.shouldCompact() {
			t.Fatal("estimate-based occupancy past the trigger should compact")
		}
	})

	t.Run("metered occupancy triggers at the window trigger", func(t *testing.T) {
		loop := newLoop()
		loop.lastCallInput = 700 // below the 800 trigger
		if loop.shouldCompact() {
			t.Fatal("occupancy below the trigger must not compact")
		}
		loop.lastCallInput = 900 // past the trigger
		if !loop.shouldCompact() {
			t.Fatal("occupancy past the trigger should compact")
		}
	})

	t.Run("zero window disables automatic compaction", func(t *testing.T) {
		loop := newLoop()
		loop.Window = WindowModel{}
		loop.lastCallInput = 1 << 30
		if loop.shouldCompact() {
			t.Fatal("an unusable window disables occupancy-driven compaction")
		}
	})

	t.Run("forced compaction after provider context overflow", func(t *testing.T) {
		loop := newLoop()
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

func TestEstTokensImageWireFootprint(t *testing.T) {
	// A 343KB screenshot must count its base64 wire size (457KB → ~114k
	// tokens), not the flat 1500-token floor — gateways metering raw prompt
	// length see the wire form.
	big := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 343323, MediaType: "image/jpeg"}
	small := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 100, MediaType: "image/png"}
	presentOnly := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 1 << 20, MediaType: "image/png"}
	messages := []domain.Message{
		{
			ID: domain.NewMessageID(), Role: domain.RoleUser,
			Parts: []domain.ContentPart{
				{Kind: domain.PartArtifact, Artifact: &big},
				{Kind: domain.PartArtifact, Artifact: &small},
				{Kind: domain.PartArtifact, Artifact: &presentOnly, PresentOnly: true},
			},
			CreatedAt: time.Now(),
		},
	}
	got := estTokens(messages)
	wantBig := 343323 * 4 / 3 / 4 // base64 chars / 4
	want := wantBig + 1500        // big wire + small floor; present-only excluded
	if got != want {
		t.Fatalf("estTokens = %d, want %d (wire %d + floor 1500, present-only excluded)", got, want, wantBig)
	}

	// Inline image parts count their base64 payload length directly.
	inline := []domain.Message{{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{
			Kind: domain.PartImage,
			Image: &domain.ImageContent{
				MediaType: "image/png",
				Data:      strings.Repeat("a", 8000),
			},
		}},
		CreatedAt: time.Now(),
	}}
	if got := estTokens(inline); got != 2000 {
		t.Fatalf("estTokens(inline 8000 base64 chars) = %d, want 2000", got)
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

	replacement, _ := buildSummaryReplacement(messages, "HANDOFF", now, 80*1024)

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
		// compaction target still forces the summarization path.
		Condenser: Condenser{KeepRecentMessages: 10, MaskMinBytes: 4096, Window: windowWithTarget(10)},
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
