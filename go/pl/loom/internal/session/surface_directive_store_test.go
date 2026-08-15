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

package session

import (
	"context"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Surface directive GC/rewind/inspection tests (docs/SURFACE_DESIGN.md
// §4.6, §4.5): directive events carry artifact references that must keep
// blobs alive even without any checkpoint, rewind must honor the surviving
// directives, and the two InspectSession entry paths must agree.

func testArtifactRef(t *testing.T, fill byte, size int64) domain.ArtifactRef {
	t.Helper()
	id, err := domain.ParseArtifactID("art_sha256_" + strings.Repeat(string(fill), 64))
	if err != nil {
		t.Fatalf("ParseArtifactID: %v", err)
	}
	return domain.ArtifactRef{ID: id, Size: size, MediaType: "text/plain"}
}

func maskedDirectiveEvent(t *testing.T, sessionID domain.SessionID, seq int64, messageID domain.MessageID, ref domain.ArtifactRef) domain.Event {
	t.Helper()
	payload, err := domain.MarshalPayload(domain.ContextMaskedPayload{Masks: []domain.MaskedPart{{
		MessageID: messageID, PartIndex: 0, ContentIndex: 0,
		OriginalBytes: int(ref.Size), Artifact: ref,
		Placeholder: "[tool output compacted]", Revision: 2,
	}}})
	if err != nil {
		t.Fatalf("MarshalPayload: %v", err)
	}
	return newEvent(sessionID, seq, domain.EventContextMasked, payload)
}

// TestRewindKeepsSurvivingDirectiveArtifactRefs: rewind truncates the log;
// references pinned by directive events that SURVIVE the rewind must
// survive, references from truncated events must go.
func TestRewindKeepsSurvivingDirectiveArtifactRefs(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	keptRef := testArtifactRef(t, 'd', 11)
	droppedRef := testArtifactRef(t, 'e', 22)

	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	events1 := []domain.Event{
		maskedDirectiveEvent(t, sessionID, 1, domain.NewMessageID(), keptRef),
	}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("first append: %v", err)
	}
	ckpt2 := testCheckpoint(sessionID, 2, time.Now().UTC().Add(time.Second))
	events2 := []domain.Event{
		maskedDirectiveEvent(t, sessionID, 2, domain.NewMessageID(), droppedRef),
	}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("second append: %v", err)
	}

	if _, err := store.RewindSession(ctx, sessionID, 1); err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		t.Fatalf("ListArtifactRefs: %v", err)
	}
	if _, ok := refs[droppedRef.ID]; ok {
		t.Fatalf("truncated directive's artifact still referenced: %+v", refs)
	}
	if got := refs[keptRef.ID]; got != 11 {
		t.Fatalf("surviving directive's artifact ref = %d, want 11 (refs %+v)", got, refs)
	}
}

// TestInspectSessionMatchesPureReplayWithDirectives locks §4.5's semantic
// unification: with a checkpoint present (checkpoint + tail path) and
// without it (pure log path), InspectSession must return the same surface.
func TestInspectSessionMatchesPureReplayWithDirectives(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	base := time.Date(2026, 8, 14, 10, 0, 0, 0, time.UTC)
	callID := domain.NewToolCallID()
	callMsg := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{
			ID: callID, Name: "read_file", Arguments: json.RawMessage(`{"path":"a.go"}`),
		}}},
		CreatedAt: base,
	}
	resultMsg := domain.Message{
		ID: domain.NewMessageID(), Sequence: 2, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
			CallID: callID, Status: domain.ToolStatusSuccess,
			Content:   []domain.ContentPart{{Kind: domain.PartText, Text: strings.Repeat("x", 4000)}},
			StartedAt: base, FinishedAt: base,
		}}},
		CreatedAt: base,
	}
	maskedRef := testArtifactRef(t, 'f', 4000)
	maskPayload, err := domain.MarshalPayload(domain.ContextMaskedPayload{Masks: []domain.MaskedPart{{
		MessageID: resultMsg.ID, PartIndex: 0, ContentIndex: 0,
		OriginalBytes: 4000, Artifact: maskedRef,
		Placeholder: "[tool output compacted 3.9KB externalized]", Revision: 2,
	}}})
	if err != nil {
		t.Fatalf("MarshalPayload: %v", err)
	}

	events := []domain.Event{
		newEventAt(sessionID, 1, domain.EventSessionCreated, nil, base),
		newEventAt(sessionID, 2, domain.EventModelResponseCompleted, messagePayload(t, callMsg), base),
		newEventAt(sessionID, 3, domain.EventToolResultAdded, messagePayload(t, resultMsg), base),
		newEventAt(sessionID, 4, domain.EventContextMasked, maskPayload, base),
	}
	// The checkpoint carries the post-mask surface, exactly as flushEvents
	// persists it in the same transaction as the directive event.
	surface, err := domain.ApplyMaskDirective([]domain.Message{callMsg, resultMsg},
		domain.ContextMaskedPayload{Masks: []domain.MaskedPart{{
			MessageID: resultMsg.ID, PartIndex: 0, ContentIndex: 0,
			OriginalBytes: 4000, Artifact: maskedRef,
			Placeholder: "[tool output compacted 3.9KB externalized]", Revision: 2,
		}}})
	if err != nil {
		t.Fatalf("ApplyMaskDirective: %v", err)
	}
	ckpt := testCheckpoint(sessionID, 4, base)
	ckpt.Messages = surface
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}

	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession: %v", err)
	}
	replayed, err := Replay(inspection.Events)
	if err != nil {
		t.Fatalf("Replay from pure log: %v", err)
	}
	if len(inspection.Transcript.Messages) != len(replayed.Messages) {
		t.Fatalf("checkpoint path %d messages vs pure-log path %d",
			len(inspection.Transcript.Messages), len(replayed.Messages))
	}
	for i := range replayed.Messages {
		want, _ := json.Marshal(replayed.Messages[i])
		got, _ := json.Marshal(inspection.Transcript.Messages[i])
		if string(want) != string(got) {
			t.Fatalf("message %d diverges between entries:\n pure-log:   %s\n checkpoint: %s", i, want, got)
		}
	}
	// And the surface really is masked (not the full-fidelity original).
	content := inspection.Transcript.Messages[1].Parts[0].ToolResult.Content[0].Text
	if !strings.HasPrefix(content, "[tool output compacted") {
		t.Fatalf("expected masked placeholder in inspected surface, got %q", content[:60])
	}
}

// TestLegacySessionWithoutDirectivesUnchanged: §4.7 — a session with no
// directive events replays byte-identically to the pre-change behavior
// (the directive code paths never engage).
func TestLegacySessionWithoutDirectivesUnchanged(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	events := transcriptEvents(t, sessionID)
	ckpt := testCheckpoint(sessionID, 3, time.Now().UTC())
	ckpt.Messages = []domain.Message{}
	for _, evt := range events {
		if evt.Type == domain.EventUserMessageAdded || evt.Type == domain.EventModelResponseCompleted {
			payload, err := domain.UnmarshalMessageEventPayload(evt.Payload)
			if err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			ckpt.Messages = append(ckpt.Messages, payload.Message)
		}
	}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}

	replayed, err := Replay(events)
	if err != nil {
		t.Fatalf("Replay legacy events: %v", err)
	}
	if len(replayed.Messages) != 2 {
		t.Fatalf("legacy replay messages = %d, want 2", len(replayed.Messages))
	}
	full, err := ReplayFull(events)
	if err != nil {
		t.Fatalf("ReplayFull legacy events: %v", err)
	}
	if len(full.Messages) != len(replayed.Messages) {
		t.Fatalf("without directives Replay and ReplayFull must agree: %d vs %d",
			len(replayed.Messages), len(full.Messages))
	}
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession legacy: %v", err)
	}
	if len(inspection.Transcript.Messages) != 2 {
		t.Fatalf("legacy inspect messages = %d, want 2", len(inspection.Transcript.Messages))
	}
}
