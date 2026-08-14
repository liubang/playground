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

package domain

import (
	"strings"
	"testing"
	"time"
)

func surfaceTestMessage(role Role, seq int64, parts ...ContentPart) Message {
	return Message{
		ID:        NewMessageID(),
		Role:      role,
		Status:    MessageStatusFinal,
		Revision:  1,
		Sequence:  seq,
		Parts:     parts,
		CreatedAt: time.Now().UTC(),
	}
}

func surfaceToolCall(id ToolCallID, name string) ContentPart {
	return ContentPart{Kind: PartToolCall, ToolCall: &ToolCall{
		ID: id, Name: name, Arguments: []byte(`{"path":"main.go"}`),
	}}
}

func surfaceToolResult(callID ToolCallID, texts ...string) ContentPart {
	content := make([]ContentPart, len(texts))
	for i, text := range texts {
		content[i] = ContentPart{Kind: PartText, Text: text}
	}
	return ContentPart{Kind: PartToolResult, ToolResult: &ToolResult{
		CallID: callID, Status: ToolStatusSuccess, Content: content,
		StartedAt: time.Now().UTC(), FinishedAt: time.Now().UTC(),
	}}
}

func surfaceArtifactRef(id ArtifactID) ArtifactRef {
	return ArtifactRef{ID: id, Size: 4096, MediaType: "text/plain"}
}

// maskOps wraps masks into a masks-only SurfaceOps.
func maskOps(masks ...MaskedPart) SurfaceOps {
	return SurfaceOps{Masks: &ContextMaskedPayload{Masks: masks}}
}

// replaceOps wraps replacement messages into a replacement-only SurfaceOps.
func replaceOps(replacement ...Message) SurfaceOps {
	return SurfaceOps{Replacement: &ContextSummarizedPayload{Replacement: replacement}}
}

func TestApplySurfaceOpsEmptyOpsReturnsInputUnchanged(t *testing.T) {
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "hi"}),
	}
	out, err := ApplySurfaceOps(messages, SurfaceOps{})
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if len(out) != 1 || out[0].Sequence != 1 {
		t.Fatalf("ApplySurfaceOps() changed the list for empty ops: %+v", out)
	}
}

func TestApplySurfaceOpsMasks(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "read main.go"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "big output", "second output")),
	}
	target := messages[2]
	artifact := surfaceArtifactRef(NewArtifactID())
	mask := MaskedPart{
		MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
		OriginalBytes: len("big output"), Artifact: artifact,
		Placeholder: "[tool output compacted] 10B externalized to /x",
		Revision:    2,
	}

	out, err := ApplySurfaceOps(messages, maskOps(mask))
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if len(out) != 3 {
		t.Fatalf("ApplySurfaceOps() changed message count: %d", len(out))
	}
	masked := out[2]
	if masked.Revision != 2 {
		t.Fatalf("masked message revision = %d, want 2", masked.Revision)
	}
	result := masked.Parts[0].ToolResult
	if result.Content[0].Text != mask.Placeholder {
		t.Fatalf("masked content = %q, want %q", result.Content[0].Text, mask.Placeholder)
	}
	if result.Content[1].Text != "second output" {
		t.Fatalf("untouched content = %q, want %q", result.Content[1].Text, "second output")
	}
	if len(result.Content) != 3 {
		t.Fatalf("content length = %d, want 3 (artifact appended)", len(result.Content))
	}
	appended := result.Content[2]
	if appended.Kind != PartArtifact || appended.Artifact == nil || appended.Artifact.ID != artifact.ID {
		t.Fatalf("appended part = %+v, want artifact ref %s", appended, artifact.ID)
	}
	// Sequences untouched by masking-only ops (dense already).
	for i, msg := range out {
		if msg.Sequence != int64(i+1) {
			t.Fatalf("message %d sequence = %d, want %d", i, msg.Sequence, i+1)
		}
	}
	// Input must not be mutated.
	if messages[2].Revision != 1 || messages[2].Parts[0].ToolResult.Content[0].Text != "big output" {
		t.Fatal("ApplySurfaceOps mutated its input")
	}
}

func TestApplySurfaceOpsMasksAbsoluteRevision(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleAssistant, 1, surfaceToolCall(callID, "run_cmd")),
		surfaceTestMessage(RoleUser, 2, surfaceToolResult(callID, "out-a", "out-b")),
	}
	target := messages[1]
	// Two masks on the same message across two levels: the absolute revision
	// of the second (level-2b bumped again) must win, matching runtime
	// once-per-level bumps.
	masks := []MaskedPart{
		{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 0, OriginalBytes: 5,
			Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[a]", Revision: 2,
		},
		{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 1, OriginalBytes: 5,
			Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[b]", Revision: 3,
		},
	}
	out, err := ApplySurfaceOps(messages, maskOps(masks...))
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	masked := out[1]
	if masked.Revision != 3 {
		t.Fatalf("revision = %d, want 3 (absolute, last wins)", masked.Revision)
	}
	result := masked.Parts[0].ToolResult
	if result.Content[0].Text != "[a]" || result.Content[1].Text != "[b]" {
		t.Fatalf("masked contents = %q / %q", result.Content[0].Text, result.Content[1].Text)
	}
	if len(result.Content) != 4 {
		t.Fatalf("content length = %d, want 4 (two artifacts appended)", len(result.Content))
	}
}

func TestApplySurfaceOpsArchive(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "content")),
		surfaceTestMessage(RoleAssistant, 4, ContentPart{Kind: PartText, Text: "recent answer"}),
	}
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "[earlier messages archived] 3 messages externalized to /x]"})
	archive := ContextArchivedPayload{
		FromSequence: 1, ToSequence: 3,
		Artifact: surfaceArtifactRef(NewArtifactID()),
		Marker:   marker,
	}

	out, err := ApplySurfaceOps(messages, SurfaceOps{Archive: &archive})
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if len(out) != 2 {
		t.Fatalf("len = %d, want 2 (marker + survivor)", len(out))
	}
	if out[0].ID != marker.ID {
		t.Fatalf("out[0].ID = %s, want marker %s", out[0].ID, marker.ID)
	}
	if out[0].Sequence != 1 || out[1].Sequence != 2 {
		t.Fatalf("sequences = %d,%d, want dense 1,2", out[0].Sequence, out[1].Sequence)
	}
	if out[1].ID != messages[3].ID {
		t.Fatalf("survivor = %s, want %s", out[1].ID, messages[3].ID)
	}
}

func TestApplySurfaceOpsArchivePairingViolation(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "content")),
		surfaceTestMessage(RoleAssistant, 4, ContentPart{Kind: PartText, Text: "done"}),
	}
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "marker"})
	// Archiving [1,2] removes the call but leaves its result at 3: invalid.
	archive := ContextArchivedPayload{
		FromSequence: 1, ToSequence: 2,
		Artifact: surfaceArtifactRef(NewArtifactID()),
		Marker:   marker,
	}
	_, err := ApplySurfaceOps(messages, SurfaceOps{Archive: &archive})
	if err == nil {
		t.Fatal("ApplySurfaceOps() succeeded on a pairing-breaking archive")
	}
	if !strings.Contains(err.Error(), "no preceding call") {
		t.Fatalf("error = %v, want pairing violation", err)
	}
}

func TestApplySurfaceOpsArchiveDanglingCall(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleAssistant, 1, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 2, surfaceToolResult(callID, "content")),
		surfaceTestMessage(RoleAssistant, 3, ContentPart{Kind: PartText, Text: "done"}),
	}
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "marker"})
	// Archiving [2,2] removes the result but leaves its call at 1: invalid.
	archive := ContextArchivedPayload{
		FromSequence: 2, ToSequence: 2,
		Artifact: surfaceArtifactRef(NewArtifactID()),
		Marker:   marker,
	}
	_, err := ApplySurfaceOps(messages, SurfaceOps{Archive: &archive})
	if err == nil {
		t.Fatal("ApplySurfaceOps() succeeded leaving a dangling call")
	}
	if !strings.Contains(err.Error(), "no recorded result") {
		t.Fatalf("error = %v, want dangling-call violation", err)
	}
}

func TestApplySurfaceOpsReplacement(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "old task"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "content")),
	}
	replacement := []Message{
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "old task"}),
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "summary bridge"}),
	}
	out, err := ApplySurfaceOps(messages, replaceOps(replacement...))
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if len(out) != 2 || out[0].ID != replacement[0].ID || out[1].ID != replacement[1].ID {
		t.Fatalf("replacement not applied: %+v", out)
	}
	if out[0].Sequence != 1 || out[1].Sequence != 2 {
		t.Fatalf("sequences = %d,%d, want dense 1,2", out[0].Sequence, out[1].Sequence)
	}
}

func TestApplySurfaceOpsOrderingMaskThenArchive(t *testing.T) {
	// The mask event always precedes the archive event; the archive locates
	// its span in pre-compaction numbering, which masking does not disturb.
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "huge output")),
		surfaceTestMessage(RoleAssistant, 4, ContentPart{Kind: PartText, Text: "recent"}),
	}
	masked, err := ApplyMaskDirective(messages, []MaskedPart{{
		MessageID: messages[2].ID, PartIndex: 0, ContentIndex: 0, OriginalBytes: 11,
		Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[masked]", Revision: 2,
	}})
	if err != nil {
		t.Fatalf("mask: %v", err)
	}
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "marker"})
	archived, err := ApplySurfaceOps(masked, SurfaceOps{Archive: &ContextArchivedPayload{
		FromSequence: 1, ToSequence: 3,
		Artifact: surfaceArtifactRef(NewArtifactID()),
		Marker:   marker,
	}})
	if err != nil {
		t.Fatalf("archive: %v", err)
	}
	if len(archived) != 2 || archived[0].ID != marker.ID {
		t.Fatalf("archive after mask produced wrong surface: %+v", archived)
	}
	texts := archived[1].TextParts()
	if len(texts) != 1 || texts[0] != "recent" {
		t.Fatalf("survivor text = %v, want [\"recent\"]", texts)
	}
}

func TestApplySurfaceOpsNegativeCases(t *testing.T) {
	callID := NewToolCallID()
	validResult := surfaceToolResult(callID, "content")
	messages := []Message{
		surfaceTestMessage(RoleAssistant, 1, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 2, validResult),
	}
	target := messages[1]
	validArtifact := surfaceArtifactRef(NewArtifactID())

	tests := []struct {
		name string
		ops  SurfaceOps
	}{
		{"mask unknown message", maskOps(MaskedPart{
			MessageID: NewMessageID(), PartIndex: 0, ContentIndex: 0,
			Artifact: validArtifact, Placeholder: "[x]", Revision: 2,
		})},
		{"mask part index out of range", maskOps(MaskedPart{
			MessageID: target.ID, PartIndex: 7, ContentIndex: 0,
			Artifact: validArtifact, Placeholder: "[x]", Revision: 2,
		})},
		{"mask non-tool-result part", maskOps(MaskedPart{
			MessageID: messages[0].ID, PartIndex: 0, ContentIndex: 0,
			Artifact: validArtifact, Placeholder: "[x]", Revision: 2,
		})},
		{"mask content index out of range", maskOps(MaskedPart{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 9,
			Artifact: validArtifact, Placeholder: "[x]", Revision: 2,
		})},
		{"mask empty placeholder", maskOps(MaskedPart{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
			Artifact: validArtifact, Placeholder: "", Revision: 2,
		})},
		{"mask revision not increasing", maskOps(MaskedPart{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
			Artifact: validArtifact, Placeholder: "[x]", Revision: 1,
		})},
		{"archive unknown span", SurfaceOps{Archive: &ContextArchivedPayload{
			FromSequence: 40, ToSequence: 50,
			Artifact: validArtifact,
			Marker:   surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "m"}),
		}}},
		{"archive inverted span", SurfaceOps{Archive: &ContextArchivedPayload{
			FromSequence: 2, ToSequence: 1,
			Artifact: validArtifact,
			Marker:   surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "m"}),
		}}},
		{"archive invalid marker", SurfaceOps{Archive: &ContextArchivedPayload{
			FromSequence: 1, ToSequence: 1,
			Artifact: validArtifact,
			Marker:   Message{Role: RoleSystem, Parts: []ContentPart{{Kind: PartText, Text: "m"}}},
		}}},
		{"replacement with invalid message", replaceOps(
			Message{Role: RoleUser, Parts: []ContentPart{{Kind: PartText, Text: "x"}}},
		)},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if _, err := ApplySurfaceOps(messages, tt.ops); err == nil {
				t.Fatal("ApplySurfaceOps() succeeded on an invalid directive")
			}
		})
	}
}

func TestApplySurfaceOpsMasksSameLevelSharedRevision(t *testing.T) {
	// The runtime bumps a message's revision once per LEVEL even when
	// several contents are masked: masks from one level share the revision.
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleAssistant, 1, surfaceToolCall(callID, "run_cmd")),
		surfaceTestMessage(RoleUser, 2, surfaceToolResult(callID, "out-a", "out-b")),
	}
	target := messages[1]
	masks := []MaskedPart{
		{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 0, OriginalBytes: 5,
			Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[a]", Revision: 2,
		},
		{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 1, OriginalBytes: 5,
			Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[b]", Revision: 2,
		},
	}
	out, err := ApplySurfaceOps(messages, maskOps(masks...))
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if out[1].Revision != 2 {
		t.Fatalf("revision = %d, want 2", out[1].Revision)
	}
	result := out[1].Parts[0].ToolResult
	if result.Content[0].Text != "[a]" || result.Content[1].Text != "[b]" || len(result.Content) != 4 {
		t.Fatalf("unexpected masked contents: %+v", result.Content)
	}
}

func TestApplySurfaceOpsFullChainSingleCall(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "huge output")),
		surfaceTestMessage(RoleAssistant, 4, ContentPart{Kind: PartText, Text: "recent"}),
	}
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "marker"})
	replacement := []Message{
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "summary bridge"}),
	}
	ops := SurfaceOps{
		Masks: &ContextMaskedPayload{Masks: []MaskedPart{{
			MessageID: messages[2].ID, PartIndex: 0, ContentIndex: 0, OriginalBytes: 11,
			Artifact: surfaceArtifactRef(NewArtifactID()), Placeholder: "[masked]", Revision: 2,
		}}},
		Archive: &ContextArchivedPayload{
			FromSequence: 1, ToSequence: 3,
			Artifact: surfaceArtifactRef(NewArtifactID()),
			Marker:   marker,
		},
		Replacement: &ContextSummarizedPayload{Replacement: replacement},
	}
	out, err := ApplySurfaceOps(messages, ops)
	if err != nil {
		t.Fatalf("ApplySurfaceOps() error = %v", err)
	}
	if len(out) != 2 || out[0].ID != replacement[0].ID || out[1].ID != replacement[1].ID {
		t.Fatalf("replacement did not cover prior directives: %+v", out)
	}
	if out[0].Sequence != 1 || out[1].Sequence != 2 {
		t.Fatalf("sequences = %d,%d, want dense 1,2", out[0].Sequence, out[1].Sequence)
	}
}

// TestApplySurfaceOpsFullChainPerEvent locks the projector rhythm: the same
// three directives applied one event at a time (the replay path) must yield
// the identical final surface as one combined application (the runtime
// path) — this is the executable proof of the dense-renumber equivalence.
func TestApplySurfaceOpsFullChainPerEvent(t *testing.T) {
	callID := NewToolCallID()
	build := func() []Message {
		return []Message{
			surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "task"}),
			surfaceTestMessage(RoleAssistant, 2, surfaceToolCall(callID, "read_file")),
			surfaceTestMessage(RoleUser, 3, surfaceToolResult(callID, "huge output")),
			surfaceTestMessage(RoleAssistant, 4, ContentPart{Kind: PartText, Text: "recent"}),
		}
	}
	maskArtifact := surfaceArtifactRef(NewArtifactID())
	archiveArtifact := surfaceArtifactRef(NewArtifactID())
	marker := surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "marker"})
	replacement := []Message{
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "task"}),
		surfaceTestMessage(RoleUser, 0, ContentPart{Kind: PartText, Text: "summary bridge"}),
	}
	base := build()
	masks := []MaskedPart{{
		MessageID: base[2].ID, PartIndex: 0, ContentIndex: 0, OriginalBytes: 11,
		Artifact: maskArtifact, Placeholder: "[masked]", Revision: 2,
	}}
	archive := ContextArchivedPayload{FromSequence: 1, ToSequence: 3, Artifact: archiveArtifact, Marker: marker}

	// Per-event rhythm (projector). ApplySurfaceOps never mutates its
	// input, so both rhythms can share the same base list.
	step1, err := ApplyMaskDirective(base, masks)
	if err != nil {
		t.Fatalf("mask directive: %v", err)
	}
	step2, err := ApplyArchiveDirective(step1, archive)
	if err != nil {
		t.Fatalf("archive directive: %v", err)
	}
	perEvent, err := ApplyReplacementDirective(step2, replacement)
	if err != nil {
		t.Fatalf("replacement directive: %v", err)
	}

	// Single-call rhythm (runtime).
	singleCall, err := ApplySurfaceOps(base, SurfaceOps{
		Masks:       &ContextMaskedPayload{Masks: masks},
		Archive:     &archive,
		Replacement: &ContextSummarizedPayload{Replacement: replacement},
	})
	if err != nil {
		t.Fatalf("single call: %v", err)
	}

	if len(perEvent) != len(singleCall) {
		t.Fatalf("len mismatch: per-event %d vs single-call %d", len(perEvent), len(singleCall))
	}
	for i := range perEvent {
		if perEvent[i].ID != singleCall[i].ID || perEvent[i].Sequence != singleCall[i].Sequence ||
			perEvent[i].Revision != singleCall[i].Revision || perEvent[i].Role != singleCall[i].Role {
			t.Fatalf("message %d diverged: per-event %+v vs single-call %+v", i, perEvent[i], singleCall[i])
		}
	}
}

func TestApplyDirectivesRejectEmpty(t *testing.T) {
	messages := []Message{
		surfaceTestMessage(RoleUser, 1, ContentPart{Kind: PartText, Text: "hi"}),
	}
	if _, err := ApplyMaskDirective(messages, nil); err == nil {
		t.Fatal("ApplyMaskDirective() accepted an empty directive")
	}
	if _, err := ApplyReplacementDirective(messages, nil); err == nil {
		t.Fatal("ApplyReplacementDirective() accepted an empty directive")
	}
}

func TestApplySurfaceOpsAdditionalNegativeCases(t *testing.T) {
	callID := NewToolCallID()
	messages := []Message{
		surfaceTestMessage(RoleAssistant, 1, surfaceToolCall(callID, "read_file")),
		surfaceTestMessage(RoleUser, 2, surfaceToolResult(callID, "content")),
	}
	target := messages[1]
	validArtifact := surfaceArtifactRef(NewArtifactID())

	tests := []struct {
		name string
		ops  SurfaceOps
	}{
		{"mask invalid artifact", maskOps(MaskedPart{
			MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
			Artifact: ArtifactRef{}, Placeholder: "[x]", Revision: 2,
		})},
		{"mask duplicate content directive", maskOps(
			MaskedPart{
				MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
				Artifact: validArtifact, Placeholder: "[x]", Revision: 2,
			},
			MaskedPart{
				MessageID: target.ID, PartIndex: 0, ContentIndex: 0,
				Artifact: validArtifact, Placeholder: "[y]", Revision: 3,
			},
		)},
		{"archive invalid artifact", SurfaceOps{Archive: &ContextArchivedPayload{
			FromSequence: 1, ToSequence: 2,
			Artifact: ArtifactRef{ID: NewArtifactID(), Size: -1},
			Marker:   surfaceTestMessage(RoleSystem, 0, ContentPart{Kind: PartText, Text: "m"}),
		}}},
		{"replacement duplicate open call", replaceOps(
			surfaceTestMessage(RoleAssistant, 0,
				surfaceToolCall(callID, "read_file"),
				surfaceToolCall(callID, "read_file")),
		)},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if _, err := ApplySurfaceOps(messages, tt.ops); err == nil {
				t.Fatal("ApplySurfaceOps() succeeded on an invalid directive")
			}
		})
	}
}

func TestSurfaceOpEventTypesValidate(t *testing.T) {
	for _, evtType := range []EventType{EventContextMasked, EventContextArchived, EventContextSummarized} {
		evt := Event{
			ID: NewEventID(), Sequence: 1, SessionID: NewSessionID(),
			Type: evtType, Timestamp: time.Now().UTC(),
		}
		if err := evt.Validate(); err != nil {
			t.Fatalf("Event(%s).Validate() error = %v", evtType, err)
		}
	}
}
