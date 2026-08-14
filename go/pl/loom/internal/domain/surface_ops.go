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

// ContextMaskedPayload is the EventContextMasked payload.
type ContextMaskedPayload struct {
	Masks []MaskedPart `json:"masks"`
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
		if out, err = applyMasks(out, ops.Masks.Masks); err != nil {
			return nil, fmt.Errorf("context.masked: %w", err)
		}
		for _, mask := range ops.Masks.Masks {
			touched[mask.MessageID] = struct{}{}
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
func ApplyMaskDirective(messages []Message, masks []MaskedPart) ([]Message, error) {
	if len(masks) == 0 {
		return nil, fmt.Errorf("context.masked: empty directive")
	}
	return ApplySurfaceOps(messages, SurfaceOps{Masks: &ContextMaskedPayload{Masks: masks}})
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

// applyMasks externalizes tool outputs per directive. Masks hitting the same
// message in the same level share one revision value (the runtime bumps a
// message once per level); cross-level masks must strictly increase.
// Directives targeting the same content twice are rejected: the runtime
// generator can never produce them (the placeholder prefix guard skips
// masked content).
func applyMasks(messages []Message, masks []MaskedPart) ([]Message, error) {
	byID := make(map[MessageID]int, len(messages))
	for i, msg := range messages {
		byID[msg.ID] = i
	}
	type contentLoc struct {
		messageID    MessageID
		partIndex    int
		contentIndex int
	}
	seen := make(map[contentLoc]struct{}, len(masks))
	baselines := make(map[MessageID]int)
	for mi, mask := range masks {
		loc := contentLoc{mask.MessageID, mask.PartIndex, mask.ContentIndex}
		if _, dup := seen[loc]; dup {
			return nil, fmt.Errorf("mask[%d]: duplicate directive for message %s part %d content %d", mi, mask.MessageID, mask.PartIndex, mask.ContentIndex)
		}
		seen[loc] = struct{}{}
		idx, ok := byID[mask.MessageID]
		if !ok {
			return nil, fmt.Errorf("mask[%d]: message %s not found", mi, mask.MessageID)
		}
		msg := &messages[idx]
		baseline, maskedBefore := baselines[mask.MessageID]
		if !maskedBefore {
			baseline = msg.Revision
			baselines[mask.MessageID] = baseline
		}
		// Same-level masks on one message share one revision bump (equal
		// revisions allowed); a later level must strictly increase.
		if mask.Revision <= baseline || mask.Revision < msg.Revision {
			return nil, fmt.Errorf("mask[%d]: message %s revision must exceed pre-mask %d and not regress below %d (got %d)", mi, mask.MessageID, baseline, msg.Revision, mask.Revision)
		}
		if mask.PartIndex < 0 || mask.PartIndex >= len(msg.Parts) {
			return nil, fmt.Errorf("mask[%d]: message %s part_index %d out of range", mi, mask.MessageID, mask.PartIndex)
		}
		part := &msg.Parts[mask.PartIndex]
		if part.Kind != PartToolResult || part.ToolResult == nil {
			return nil, fmt.Errorf("mask[%d]: message %s part %d is not a tool result", mi, mask.MessageID, mask.PartIndex)
		}
		if mask.ContentIndex < 0 || mask.ContentIndex >= len(part.ToolResult.Content) {
			return nil, fmt.Errorf("mask[%d]: message %s part %d content_index %d out of range", mi, mask.MessageID, mask.PartIndex, mask.ContentIndex)
		}
		content := &part.ToolResult.Content[mask.ContentIndex]
		if content.Kind != PartText {
			return nil, fmt.Errorf("mask[%d]: message %s part %d content %d is not text", mi, mask.MessageID, mask.PartIndex, mask.ContentIndex)
		}
		if mask.Placeholder == "" {
			return nil, fmt.Errorf("mask[%d]: empty placeholder", mi)
		}
		if err := mask.Artifact.Validate(); err != nil {
			return nil, fmt.Errorf("mask[%d]: invalid artifact: %w", mi, err)
		}
		content.Text = mask.Placeholder
		artifactRef := mask.Artifact
		part.ToolResult.Content = append(part.ToolResult.Content, ContentPart{Kind: PartArtifact, Artifact: &artifactRef})
		msg.Revision = mask.Revision
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
