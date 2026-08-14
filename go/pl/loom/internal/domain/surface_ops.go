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
	"fmt"
)

// Surface compaction directive events (docs/SURFACE_DESIGN.md §4.1).
//
// The event log is append-only and full-fidelity; the model-visible surface
// is a pure function of it. Compaction persists its decisions as directive
// events so replay reconstructs exactly what the model saw:
//
//	context.masked? → context.archived? → context.summarized? → context.compacted
//
// Directive events carry the complete replacement artifacts (placeholder
// text, marker message, replacement messages) generated once at runtime;
// replay applies them verbatim via ApplySurfaceOps — the projector never
// re-derives compaction decisions.
const (
	// EventContextMasked records Level 1/2b observation masking: oversized
	// tool outputs externalized to the artifact store and replaced with
	// pointer placeholders.
	EventContextMasked EventType = "context.masked"
	// EventContextArchived records Level 2a span archival: the oldest
	// message span serialized into one full-fidelity artifact and replaced
	// by a marker message.
	EventContextArchived EventType = "context.archived"
	// EventContextSummarized records Level 3 summary replacement: the whole
	// surface rebuilt around a model-written handoff summary.
	EventContextSummarized EventType = "context.summarized"
)

// MaskedPart locates one externalized tool output: which content of which
// tool result of which message. Location is by MessageID + PartIndex +
// ContentIndex, independent of list order and of whether an archive also
// removed the message later in the same pass (masks always apply first).
type MaskedPart struct {
	MessageID MessageID `json:"message_id"`
	// PartIndex is the SLICE INDEX into Message.Parts (matching the runtime
	// masker's `for pi := range msg.Parts`), not ContentPart.PartIndex —
	// the two differ for messages using explicit part indexing.
	PartIndex    int `json:"part_index"`
	ContentIndex int `json:"content_index"`
	// OriginalBytes is the pre-masking text size (audit).
	OriginalBytes int `json:"original_bytes"`
	// Artifact is the FINAL appended reference form: MediaType already
	// overridden to "text/plain" by the generator (mirroring the runtime
	// masker), not the raw store commit result. estTokens and media
	// classification consume MediaType, so replay must apply it verbatim.
	Artifact ArtifactRef `json:"artifact"`
	// Placeholder is the full replacement text (including the human-readable
	// locator path), generated once at runtime and applied verbatim.
	Placeholder string `json:"placeholder"`
	// Revision is the message revision AFTER this mask. Absolute (not a
	// delta) so replay is byte-identical regardless of how many masks hit
	// the same message across compaction levels. Generator contract: masks
	// targeting the same message MUST appear in non-decreasing Revision
	// order (Level 1 entries before Level 2b entries); equal revisions are
	// masks from the same level — the runtime bumps a message's revision
	// once per level even when several of its contents are masked.
	Revision int `json:"revision"`
}

// PrunedPart locates one inline middle-pruned tool output: which content
// of which tool result of which message. Unlike a MaskedPart nothing is
// externalized — the head and tail stay inline and the middle is dropped
// (the original remains in the append-only log regardless), so a prune
// costs no artifact round-trip and keeps the output's shape visible to
// the model. Location and Revision semantics are exactly MaskedPart's.
type PrunedPart struct {
	MessageID    MessageID `json:"message_id"`
	PartIndex    int       `json:"part_index"`
	ContentIndex int       `json:"content_index"`
	// OriginalBytes is the pre-pruning text size (audit).
	OriginalBytes int `json:"original_bytes"`
	// Replacement is the full replacement text (head + marker + tail),
	// generated once at runtime and applied verbatim.
	Replacement string `json:"replacement"`
	// Revision follows the MaskedPart contract: prunes and masks from the
	// same pass share one revision bump per message.
	Revision int `json:"revision"`
}

// ContextMaskedPayload is the EventContextMasked payload: one observation
// reduction pass carrying both externalized masks (full-fidelity
// preservation) and inline prunes (cheap middle-truncation). Prunes apply
// before masks.
type ContextMaskedPayload struct {
	Masks []MaskedPart `json:"masks"`
	// Prunes carries the inline middle-prunings. Old logs lack the field
	// (nil); old binaries reading a payload with prunes ignore the field
	// and replay the fuller pre-prune surface — a conservative divergence,
	// never a failure.
	Prunes []PrunedPart `json:"prunes,omitempty"`
}

// Empty reports whether the payload carries any directive at all.
func (p ContextMaskedPayload) Empty() bool {
	return len(p.Masks) == 0 && len(p.Prunes) == 0
}

// ContextArchivedPayload is the EventContextArchived payload.
// [FromSequence, ToSequence] (both inclusive) locates the archived span in
// the pre-compaction surface numbering. Validity at replay rests on the
// inductive invariant that the projector has applied the same directive
// prefix as the runtime, so its surface numbering matches generation time
// (docs/SURFACE_DESIGN.md §4.1.2).
type ContextArchivedPayload struct {
	FromSequence int64 `json:"from_sequence"`
	ToSequence   int64 `json:"to_sequence"`
	// Artifact is the full-fidelity span blob. A ZERO Artifact means the
	// span was dropped without preservation — only the compaction summary
	// overflow-retry path does this when no artifact store is available
	// (the original messages remain in the event log regardless).
	Artifact ArtifactRef `json:"artifact"`
	// Marker is the complete replacement message (including the artifact
	// reference list in Metadata[MetadataCompactedArtifacts]); the projector
	// inserts it verbatim with zero computation. Its Sequence is ignored —
	// dense renumbering assigns the final value.
	Marker Message `json:"marker"`
}

// ContextSummarizedPayload is the EventContextSummarized payload.
type ContextSummarizedPayload struct {
	// Replacement is the complete post-summary surface (verbatim user
	// messages plus the summary bridge), carrying full IDs/Parts/Metadata.
	Replacement []Message `json:"replacement"`
	// DroppedUserMessages counts real user messages that did not fit the
	// budget and were dropped (audit).
	DroppedUserMessages int `json:"dropped_user_messages,omitempty"`
}

// SurfaceOps is the complete directive set produced by one compaction pass
// (docs/SURFACE_DESIGN.md §4.2). Fields are the event payloads themselves,
// so emitting the directive events needs no re-wrapping. They may be
// simultaneously non-nil (mask → archive → replacement escalation) and
// apply in that order.
type SurfaceOps struct {
	Masks       *ContextMaskedPayload
	Archive     *ContextArchivedPayload
	Replacement *ContextSummarizedPayload
}

// Empty reports whether the ops carry any directive at all.
func (ops SurfaceOps) Empty() bool {
	return ops.Masks == nil && ops.Archive == nil && ops.Replacement == nil
}

// ApplySurfaceOps applies compaction directives to a message list and
// returns the resulting new list. It is the single application site shared
// by the runtime (agent loop), replay (session projector) and validation
// tooling — one implementation, three consumers, so live and replayed
// surfaces cannot drift (docs/SURFACE_DESIGN.md §4.3).
//
// The input is never mutated; an empty ops returns the input as-is. The
// result is densely renumbered (Sequence = index+1), matching the runtime
// condenser's renumbering. Invariant violations are reported as errors,
// never silently repaired: invalid locations, duplicate masks on the same
// content, a result without a preceding call, or a dangling call after a
// structural op all fail the application.
//
// Message-level Validate runs only on messages the ops TOUCH (masked
// messages, the archive marker, every replacement message): legacy
// sessions may carry messages that predate current validation standards,
// and a mask-only pass must not fail on content it never touched. Tool
// pairing is validated over the whole resulting list regardless — that is
// a structural invariant, not a schema standard.
func ApplySurfaceOps(messages []Message, ops SurfaceOps) ([]Message, error) {
	if ops.Empty() {
		return messages, nil
	}
	out := cloneMessages(messages)
	touched := make(map[MessageID]struct{})
	var err error
	if ops.Masks != nil {
		if out, err = applyMasks(out, *ops.Masks); err != nil {
			return nil, fmt.Errorf("context.masked: %w", err)
		}
		for _, mask := range ops.Masks.Masks {
			touched[mask.MessageID] = struct{}{}
		}
		for _, prune := range ops.Masks.Prunes {
			touched[prune.MessageID] = struct{}{}
		}
	}
	if ops.Archive != nil {
		if out, err = applyArchive(out, *ops.Archive); err != nil {
			return nil, fmt.Errorf("context.archived: %w", err)
		}
		touched[ops.Archive.Marker.ID] = struct{}{}
	}
	if ops.Replacement != nil {
		if out, err = applyReplacement(out, ops.Replacement.Replacement); err != nil {
			return nil, fmt.Errorf("context.summarized: %w", err)
		}
	}
	for i := range out {
		out[i].Sequence = int64(i + 1)
	}
	for i := range out {
		if _, ok := touched[out[i].ID]; !ok {
			continue
		}
		if err := out[i].Validate(); err != nil {
			return nil, fmt.Errorf("result message %s: %w", out[i].ID, err)
		}
	}
	if err := validateToolPairing(out); err != nil {
		return nil, err
	}
	return out, nil
}

// ApplyMaskDirective applies one context.masked event's payload: like
// ApplySurfaceOps with only Masks set, but rejecting an empty directive —
// a directive event with nothing to apply is corrupt, not a no-op.
func ApplyMaskDirective(messages []Message, payload ContextMaskedPayload) ([]Message, error) {
	if payload.Empty() {
		return nil, fmt.Errorf("context.masked: empty directive")
	}
	return ApplySurfaceOps(messages, SurfaceOps{Masks: &payload})
}

// ApplyArchiveDirective applies one context.archived event's payload.
func ApplyArchiveDirective(messages []Message, archive ContextArchivedPayload) ([]Message, error) {
	return ApplySurfaceOps(messages, SurfaceOps{Archive: &archive})
}

// ApplyReplacementDirective applies one context.summarized event's payload.
func ApplyReplacementDirective(messages []Message, replacement []Message) ([]Message, error) {
	if len(replacement) == 0 {
		return nil, fmt.Errorf("context.summarized: empty directive")
	}
	return ApplySurfaceOps(messages, SurfaceOps{Replacement: &ContextSummarizedPayload{Replacement: replacement}})
}

// cloneMessages copies the slice, each message's Parts, any nested
// ToolResult.Content, and the Metadata map — the fields the ops mutate.
// Pointer payloads never written by application (ToolCall, Artifact,
// Reasoning, Image) are intentionally shared.
func cloneMessages(messages []Message) []Message {
	out := make([]Message, len(messages))
	for i, msg := range messages {
		cloned := msg
		cloned.Parts = make([]ContentPart, len(msg.Parts))
		for j, part := range msg.Parts {
			clonedPart := part
			if part.ToolResult != nil {
				tr := *part.ToolResult
				tr.Content = append([]ContentPart(nil), part.ToolResult.Content...)
				clonedPart.ToolResult = &tr
			}
			cloned.Parts[j] = clonedPart
		}
		if msg.Metadata != nil {
			cloned.Metadata = make(map[string]string, len(msg.Metadata))
			for k, v := range msg.Metadata {
				cloned.Metadata[k] = v
			}
		}
		out[i] = cloned
	}
	return out
}

// applyMasks applies one context.masked payload: inline prunes first,
// then externalized masks. Prunes hitting the same message in the same
// pass share one revision value with masks (the runtime bumps a message
// once per level); cross-level edits must strictly increase. Directives
// targeting the same content twice are rejected: the runtime generator
// can never produce them (the placeholder/marker guards skip edited
// content).
func applyMasks(messages []Message, payload ContextMaskedPayload) ([]Message, error) {
	// contentEdit is the shared core of a prune (inline replacement) and a
	// mask (replacement plus an appended artifact reference).
	type contentEdit struct {
		messageID    MessageID
		partIndex    int
		contentIndex int
		text         string
		artifact     *ArtifactRef // nil for prunes
		revision     int
	}
	edits := make([]contentEdit, 0, len(payload.Prunes)+len(payload.Masks))
	for _, prune := range payload.Prunes {
		edits = append(edits, contentEdit{
			messageID: prune.MessageID, partIndex: prune.PartIndex, contentIndex: prune.ContentIndex,
			text: prune.Replacement, revision: prune.Revision,
		})
	}
	for _, mask := range payload.Masks {
		artifact := mask.Artifact
		edits = append(edits, contentEdit{
			messageID: mask.MessageID, partIndex: mask.PartIndex, contentIndex: mask.ContentIndex,
			text: mask.Placeholder, artifact: &artifact, revision: mask.Revision,
		})
	}

	byID := make(map[MessageID]int, len(messages))
	for i, msg := range messages {
		byID[msg.ID] = i
	}
	type contentLoc struct {
		messageID    MessageID
		partIndex    int
		contentIndex int
	}
	seen := make(map[contentLoc]struct{}, len(edits))
	baselines := make(map[MessageID]int)
	for mi, edit := range edits {
		loc := contentLoc{edit.messageID, edit.partIndex, edit.contentIndex}
		if _, dup := seen[loc]; dup {
			return nil, fmt.Errorf("edit[%d]: duplicate directive for message %s part %d content %d", mi, edit.messageID, edit.partIndex, edit.contentIndex)
		}
		seen[loc] = struct{}{}
		idx, ok := byID[edit.messageID]
		if !ok {
			return nil, fmt.Errorf("edit[%d]: message %s not found", mi, edit.messageID)
		}
		msg := &messages[idx]
		baseline, editedBefore := baselines[edit.messageID]
		if !editedBefore {
			baseline = msg.Revision
			baselines[edit.messageID] = baseline
		}
		// Same-level edits on one message share one revision bump (equal
		// revisions allowed); a later level must strictly increase.
		if edit.revision <= baseline || edit.revision < msg.Revision {
			return nil, fmt.Errorf("edit[%d]: message %s revision must exceed pre-edit %d and not regress below %d (got %d)", mi, edit.messageID, baseline, msg.Revision, edit.revision)
		}
		if edit.partIndex < 0 || edit.partIndex >= len(msg.Parts) {
			return nil, fmt.Errorf("edit[%d]: message %s part_index %d out of range", mi, edit.messageID, edit.partIndex)
		}
		part := &msg.Parts[edit.partIndex]
		if part.Kind != PartToolResult || part.ToolResult == nil {
			return nil, fmt.Errorf("edit[%d]: message %s part %d is not a tool result", mi, edit.messageID, edit.partIndex)
		}
		if edit.contentIndex < 0 || edit.contentIndex >= len(part.ToolResult.Content) {
			return nil, fmt.Errorf("edit[%d]: message %s part %d content_index %d out of range", mi, edit.messageID, edit.partIndex, edit.contentIndex)
		}
		content := &part.ToolResult.Content[edit.contentIndex]
		if content.Kind != PartText {
			return nil, fmt.Errorf("edit[%d]: message %s part %d content %d is not text", mi, edit.messageID, edit.partIndex, edit.contentIndex)
		}
		if edit.text == "" {
			return nil, fmt.Errorf("edit[%d]: empty replacement", mi)
		}
		content.Text = edit.text
		if edit.artifact != nil {
			if err := edit.artifact.Validate(); err != nil {
				return nil, fmt.Errorf("edit[%d]: invalid artifact: %w", mi, err)
			}
			part.ToolResult.Content = append(part.ToolResult.Content, ContentPart{Kind: PartArtifact, Artifact: edit.artifact})
		}
		msg.Revision = edit.revision
	}
	return messages, nil
}

// applyArchive replaces the [FromSequence, ToSequence] span with the marker
// message. Pairing integrity of the resulting list is verified by the
// caller's final validation, so an archive that separates a tool call from
// its result fails application.
func applyArchive(messages []Message, archive ContextArchivedPayload) ([]Message, error) {
	if archive.FromSequence <= 0 || archive.ToSequence < archive.FromSequence {
		return nil, fmt.Errorf("invalid span [%d, %d]", archive.FromSequence, archive.ToSequence)
	}
	// A zero artifact is a drop without preservation (summary overflow
	// retry without an artifact store); anything else must be well-formed.
	if !archive.Artifact.ID.IsZero() {
		if err := archive.Artifact.Validate(); err != nil {
			return nil, fmt.Errorf("invalid artifact: %w", err)
		}
	}
	if err := archive.Marker.Validate(); err != nil {
		return nil, fmt.Errorf("invalid marker message: %w", err)
	}
	fromIdx, toIdx := -1, -1
	for i, msg := range messages {
		if msg.Sequence == archive.FromSequence {
			fromIdx = i
		}
		if msg.Sequence == archive.ToSequence {
			toIdx = i
		}
	}
	if fromIdx < 0 || toIdx < 0 {
		return nil, fmt.Errorf("span [%d, %d] not found in surface of %d messages", archive.FromSequence, archive.ToSequence, len(messages))
	}
	if fromIdx > toIdx {
		return nil, fmt.Errorf("span [%d, %d] resolves to inverted indices %d > %d", archive.FromSequence, archive.ToSequence, fromIdx, toIdx)
	}
	out := make([]Message, 0, len(messages)-(toIdx-fromIdx+1)+1)
	out = append(out, messages[:fromIdx]...)
	marker := cloneMessages([]Message{archive.Marker})[0]
	out = append(out, marker)
	out = append(out, messages[toIdx+1:]...)
	return out, nil
}

// applyReplacement swaps the whole surface for the summarized replacement.
func applyReplacement(_ []Message, replacement []Message) ([]Message, error) {
	if len(replacement) == 0 {
		return nil, fmt.Errorf("empty replacement")
	}
	out := cloneMessages(replacement)
	for i := range out {
		if err := out[i].Validate(); err != nil {
			return nil, fmt.Errorf("replacement message %d (%s): %w", i, out[i].ID, err)
		}
	}
	return out, nil
}

// validateToolPairing enforces the assistant tool_call ↔ tool result
// invariant over the final surface: every result answers an earlier call,
// and no call is left dangling. Compaction always runs at a phase boundary
// where the runtime transcript is fully paired, so a directive that breaks
// pairing is definitionally corrupt.
func validateToolPairing(messages []Message) error {
	open := make(map[ToolCallID]struct{})
	for i, msg := range messages {
		for _, part := range msg.Parts {
			switch part.Kind {
			case PartToolCall:
				if part.ToolCall == nil {
					continue
				}
				if _, dup := open[part.ToolCall.ID]; dup {
					return fmt.Errorf("messages[%d]: duplicate open tool call %s", i, part.ToolCall.ID)
				}
				open[part.ToolCall.ID] = struct{}{}
			case PartToolResult:
				if part.ToolResult == nil {
					continue
				}
				if _, ok := open[part.ToolResult.CallID]; !ok {
					return fmt.Errorf("messages[%d]: tool result for %s has no preceding call", i, part.ToolResult.CallID)
				}
				delete(open, part.ToolResult.CallID)
			}
		}
	}
	if len(open) > 0 {
		for id := range open {
			return fmt.Errorf("tool call %s has no recorded result after applying surface ops", id)
		}
	}
	return nil
}
