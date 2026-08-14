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
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/media"
)

// Condenser configuration defaults. The defaults are chosen so a zero
// Condenser is already safe and useful.
const (
	// defaultKeepRecentMessages protects the tail of the conversation: the
	// model needs its most recent observations to continue coherently.
	defaultKeepRecentMessages = 6
	// defaultMaskMinBytes only externalizes tool outputs large enough to be
	// worth an artifact round-trip. 16KB keeps medium outputs inline:
	// masking them bought little headroom but forced re-reads after
	// compaction (CONTEXT_DESIGN §4.5).
	defaultMaskMinBytes = 16 * 1024
	// summaryUserMessageBudgetRatio is the share of the compaction target
	// reserved for verbatim user messages in a summarized replacement
	// history (docs/CONTEXT_DESIGN.md §4.3.3).
	summaryUserMessageBudgetRatio = 0.20
	// bytesPerTokenEstimate is the rough text-bytes-per-token ratio used for
	// before/after reporting only; budget accounting always uses
	// provider-metered usage.
	bytesPerTokenEstimate = 4
)

// CompactionSummonPrompt asks the model to write a handoff summary of the
// current transcript for the model that resumes after compaction.
const CompactionSummonPrompt = `You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM that will resume the task.

Include:
- Current progress and key decisions made
- Important context, constraints, or user preferences
- What remains to be done (clear next steps)
- Any critical data, examples, or references needed to continue

Be concise, structured, and focused on helping the next LLM seamlessly continue the work.`

// CompactionSummaryPrefix introduces the handoff summary in the replacement
// history so the resumed model understands its provenance.
const CompactionSummaryPrefix = `Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:`

// compactedSummaryMeta is the Metadata["compacted"] value of the summary
// bridge message, so later passes never collect it as a real user message.
const compactedSummaryMeta = "summary"

// compactedPlaceholderMark prefixes replacement text so a second compaction
// pass never re-externalizes an already-masked output.
const compactedPlaceholderMark = "[tool output compacted]"

// archivedSpanMark prefixes the marker message that replaces an archived
// message span, so later passes skip it when computing archive boundaries.
const archivedSpanMark = "[earlier messages archived]"

// Condenser applies Level-1 context compaction ("observation masking"):
// oversized tool outputs outside the recent-message window are externalized
// into the artifact store and replaced with a pointer placeholder. Masking
// never deletes messages and never touches tool calls, so the assistant
// tool_call ↔ tool result pairing invariant is preserved by construction.
// Condenser configures context compaction. Compaction is intentionally
// unbounded in count: as long as each pass reduces occupancy, a cap only
// manufactures a death spiral for legitimately long tasks.
type Condenser struct {
	// Window carries the model-derived thresholds; its CompactTarget is
	// the Level-2 goal for the estimated transcript size. The transcript
	// is resent with every model call, so bounding it — not the
	// cumulative budget — is what actually keeps long sessions alive.
	Window WindowModel
	// KeepRecentMessages is the number of trailing messages never masked.
	KeepRecentMessages int
	// MaskMinBytes is the minimum tool-output text size externalized.
	MaskMinBytes int
}

func (c Condenser) withDefaults() Condenser {
	if c.KeepRecentMessages <= 0 {
		c.KeepRecentMessages = defaultKeepRecentMessages
	}
	if c.MaskMinBytes <= 0 {
		c.MaskMinBytes = defaultMaskMinBytes
	}
	return c
}

// target is the Level-2 goal for the estimated transcript size.
func (c Condenser) target() int { return int(c.Window.targetOrFallback()) }

// userMessageBudget bounds the verbatim user messages kept in a summarized
// replacement history: a share of the compaction target, converted to
// bytes via the transcript estimate ratio.
func (c Condenser) userMessageBudget() int {
	return int(float64(c.target()) * summaryUserMessageBudgetRatio * bytesPerTokenEstimate)
}

// maskedOutput records one externalized tool output for the audit event.
type maskedOutput struct {
	MessageID string `json:"message_id"`
	Bytes     int    `json:"bytes"`
	Artifact  string `json:"artifact"`
}

// contextCompactedPayload is the domain.EventContextCompacted payload.
// Est token counts are byte-derived estimates; occupancy counts use the
// calibrated (provider-metered) scale when available.
type contextCompactedPayload struct {
	Trigger               string         `json:"trigger"`
	Phase                 string         `json:"phase"`
	MaskedOutputs         int            `json:"masked_outputs"`
	MaskedBytes           int            `json:"masked_bytes"`
	ArchivedMessages      int            `json:"archived_messages,omitempty"`
	EstTokensBefore       int            `json:"est_tokens_before"`
	EstTokensAfter        int            `json:"est_tokens_after"`
	OccupancyBefore       int64          `json:"occupancy_before,omitempty"`
	OccupancyAfter        int64          `json:"occupancy_after,omitempty"`
	Summarized            bool           `json:"summarized,omitempty"`
	SummaryBytes          int            `json:"summary_bytes,omitempty"`
	TruncatedUserMessages int            `json:"truncated_user_messages,omitempty"`
	Outputs               []maskedOutput `json:"outputs,omitempty"`
}

// buildSummaryReplacement rebuilds the transcript after a model-written
// compaction summary: every real user message survives verbatim
// (newest-preferred within a byte budget, the oldest included one is
// truncated), followed by the summary bridge. Masked/archived system
// markers and prior summary bridges are dropped. The result contains only
// user-role messages, so no tool-call pairing invariant can be violated.
// It returns the replacement plus the number of real user messages that
// did not fit the budget and were dropped.
func buildSummaryReplacement(messages []domain.Message, summary string, now time.Time, budgetBytes int) ([]domain.Message, int) {
	var collected []string
	dropped := 0
	remaining := budgetBytes
	for i := len(messages) - 1; i >= 0; i-- {
		msg := messages[i]
		if msg.Role != domain.RoleUser || msg.Metadata["compacted"] != "" {
			continue
		}
		text := strings.Join(msg.TextParts(), "\n")
		if strings.TrimSpace(text) == "" {
			continue
		}
		if remaining <= 0 {
			dropped++
			continue
		}
		if len(text) > remaining {
			// Byte-level cutting can split a multi-byte rune (invalid UTF-8
			// would be persisted and later sent to the provider).
			text = cutAtRuneBoundary(text, remaining) + "\n[...earlier part of this message truncated...]"
		}
		collected = append(collected, text)
		remaining -= len(text)
	}

	out := make([]domain.Message, 0, len(collected)+1)
	seq := int64(0)
	next := func(text string, meta map[string]string) domain.Message {
		seq++
		return domain.Message{
			ID: domain.NewMessageID(), Role: domain.RoleUser, Status: domain.MessageStatusFinal,
			Revision: 1, Sequence: seq,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
			CreatedAt: now, Metadata: meta,
		}
	}
	for i := len(collected) - 1; i >= 0; i-- {
		out = append(out, next(collected[i], nil))
	}
	if strings.TrimSpace(summary) == "" {
		summary = "(no summary available)"
	}
	out = append(out, next(CompactionSummaryPrefix+"\n"+summary, map[string]string{"compacted": compactedSummaryMeta}))
	return out, dropped
}

// artifactPathResolver is implemented by the concrete artifact store; the
// condenser uses it to give the model a directly readable path instead of an
// opaque blob ID.
type artifactPathResolver interface {
	PathForRef(ref domain.ArtifactRef) (string, bool)
}

// Plan computes one compaction pass as pure directives (docs/SURFACE_DESIGN.md
// §4.2) without touching the input:
//
// Level 1: mask oversized tool outputs outside the keep-recent window.
// Level 2a: if the estimate still exceeds the target, archive the oldest
// message span as one full-fidelity artifact, replaced by a marker.
// Level 2b: if the trailing window alone still exceeds the target, extend
// masking into the window, protecting only the final message.
//
// Level decisions are driven by a working view to which each stage's
// directives are applied immediately (via the shared domain application
// functions) — so the directives Plan emits are incrementally validated as
// they are produced, and the view the next level sees is exactly what
// replay will see. Audit counts are derived from the returned ops by the
// caller. A nil artifact store disables compaction.
func (c Condenser) Plan(ctx context.Context, messages []domain.Message, artifacts domain.ArtifactStore, now time.Time) domain.SurfaceOps {
	c = c.withDefaults()
	var ops domain.SurfaceOps
	if artifacts == nil || len(messages) == 0 {
		return ops
	}
	target := c.target()
	view := messages

	cutoff := len(view) - c.KeepRecentMessages
	if cutoff > 0 {
		var masks []domain.MaskedPart
		masks, view = c.planMaskRange(ctx, view, artifacts, 0, cutoff, 0)
		if len(masks) > 0 {
			ops.Masks = &domain.ContextMaskedPayload{Masks: masks}
		}
	}
	if estTokens(view) > target && cutoff > 0 {
		if archive, applied, ok := c.planArchiveOldestSpan(ctx, view, artifacts, now); ok {
			ops.Archive = &archive
			view = applied
		}
	}
	if est := estTokens(view); est > target && len(view) > 1 {
		// The trailing window alone is too heavy: extend masking into it,
		// protecting only the final message, until the target is met.
		masks, _ := c.planMaskRange(ctx, view, artifacts, 0, len(view)-1, target)
		if len(masks) > 0 {
			if ops.Masks == nil {
				ops.Masks = &domain.ContextMaskedPayload{}
			}
			ops.Masks.Masks = append(ops.Masks.Masks, masks...)
		}
	}
	return ops
}

// planMaskRange is the directive-producing form of the former maskRange:
// it externalizes eligible tool outputs in view[from:to], applying each
// message's masks to the working view as it goes so the stopAtTokens
// estimate tracks the masked state exactly like the mutating runtime did.
// It returns the directives (Level order preserved) and the final view.
func (c Condenser) planMaskRange(ctx context.Context, view []domain.Message, artifacts domain.ArtifactStore, from, to, stopAtTokens int) ([]domain.MaskedPart, []domain.Message) {
	var all []domain.MaskedPart
	for i := from; i < to && i < len(view); i++ {
		if err := ctx.Err(); err != nil {
			break
		}
		if stopAtTokens > 0 && estTokens(view) <= stopAtTokens {
			break
		}
		masks := c.planMaskMessageOutputs(ctx, &view[i], artifacts)
		if len(masks) == 0 {
			continue
		}
		applied, err := domain.ApplyMaskDirective(view, masks)
		if err != nil {
			// Self-generated directives must apply; a failure here is a
			// generator bug. Drop this message's masks and keep going —
			// compaction degrades, it never corrupts.
			continue
		}
		view = applied
		all = append(all, masks...)
	}
	return all, view
}

// planMaskMessageOutputs computes the mask directives for every eligible
// tool output of one message. All masks of one message share one revision
// bump (msg.Revision+1), mirroring the runtime's once-per-message-per-level
// revision semantics. It never mutates msg.
func (c Condenser) planMaskMessageOutputs(ctx context.Context, msg *domain.Message, artifacts domain.ArtifactStore) []domain.MaskedPart {
	var masks []domain.MaskedPart
	for pi := range msg.Parts {
		part := &msg.Parts[pi]
		if part.Kind != domain.PartToolResult || part.ToolResult == nil {
			continue
		}
		for ci := range part.ToolResult.Content {
			content := &part.ToolResult.Content[ci]
			if content.Kind != domain.PartText {
				continue
			}
			if len(content.Text) < c.MaskMinBytes || strings.HasPrefix(content.Text, compactedPlaceholderMark) {
				continue
			}
			ref, err := externalize(ctx, artifacts, content.Text)
			if err != nil {
				// Externalization failure must not lose data: keep the
				// original text and try the next output.
				continue
			}
			original := len(content.Text)
			// Register the reference in its FINAL appended form (MediaType
			// overridden): replay applies it verbatim and consumers such as
			// estTokens / media classification read MediaType.
			artifactRef := ref
			artifactRef.MediaType = "text/plain" // masked tool output is text
			masks = append(masks, domain.MaskedPart{
				MessageID:     msg.ID,
				PartIndex:     pi,
				ContentIndex:  ci,
				OriginalBytes: original,
				Artifact:      artifactRef,
				Placeholder:   maskPlaceholder(original, ref, artifacts),
				Revision:      msg.Revision + 1,
			})
		}
	}
	return masks
}

// archiveSpan externalizes span as one full-fidelity artifact and builds
// the archive directive (marker included). ok=false means preservation
// failed and the caller decides whether to degrade or skip. Shared by
// Level-2a archival (Condenser.Plan) and the summary-overflow retry
// (Loop.archiveOldestForSummaryRetry).
func archiveSpan(ctx context.Context, span []domain.Message, artifacts domain.ArtifactStore, now time.Time) (domain.ContextArchivedPayload, bool) {
	data, err := encodeSpan(span)
	if err != nil {
		return domain.ContextArchivedPayload{}, false
	}
	ref, err := externalize(ctx, artifacts, string(data))
	if err != nil {
		return domain.ContextArchivedPayload{}, false
	}

	// The marker replaces the whole span, so it inherits every artifact
	// reference the span carried (masked outputs, run_cmd overflow
	// artifacts) plus the archive artifact itself. A marker message can
	// only carry text parts, so the references travel in metadata where
	// checkpointArtifactRefs picks them up for GC tracking.
	var refs []domain.ArtifactRef
	for _, msg := range span {
		refs = append(refs, msg.ArtifactRefs()...)
	}
	refs = append(refs, ref)
	metadata := map[string]string{"compacted": "archived"}
	if encoded, err := json.Marshal(refs); err == nil {
		metadata[domain.MetadataCompactedArtifacts] = string(encoded)
	}

	marker := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleSystem,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: archiveMarkerText(len(span), ref, artifacts)}},
		CreatedAt: now,
		Metadata:  metadata,
		// Sequence is assigned by dense renumbering at application time.
	}
	return domain.ContextArchivedPayload{
		FromSequence: span[0].Sequence,
		ToSequence:   span[len(span)-1].Sequence,
		Artifact:     ref,
		Marker:       marker,
	}, true
}

// planArchiveOldestSpan is the directive-producing form of the former
// archiveOldestSpan: it archives the oldest message span via archiveSpan
// and returns the directive plus the post-archive view for the next
// level's decisions. The cut is minimal for meeting the target and always
// lands on a tool-pairing-safe boundary; it never eats into the keep-recent
// window.
func (c Condenser) planArchiveOldestSpan(ctx context.Context, view []domain.Message, artifacts domain.ArtifactStore, now time.Time) (domain.ContextArchivedPayload, []domain.Message, bool) {
	cutoff := len(view) - c.KeepRecentMessages
	if cutoff <= 0 {
		return domain.ContextArchivedPayload{}, view, false
	}

	// Smallest drop that meets the target; zero means even the window alone
	// exceeds it, in which case archival cannot help (Level 2b takes over).
	drop := 0
	for cut := 1; cut <= cutoff; cut++ {
		if estTokens(view[cut:]) <= c.target() {
			drop = cut
			break
		}
	}
	if drop == 0 {
		return domain.ContextArchivedPayload{}, view, false
	}
	cut := pairingSafeCut(view, drop, cutoff)
	if cut == 0 {
		return domain.ContextArchivedPayload{}, view, false
	}

	archive, ok := archiveSpan(ctx, view[:cut], artifacts, now)
	if !ok {
		// Archival failure must not drop history: keep the span inline.
		return domain.ContextArchivedPayload{}, view, false
	}
	applied, err := domain.ApplyArchiveDirective(view, archive)
	if err != nil {
		// Self-generated directive must apply; on failure drop the archive
		// (the span stays inline) rather than emit a corrupt event.
		return domain.ContextArchivedPayload{}, view, false
	}
	return archive, applied, true
}

// pairingSafeCut finds the smallest cut index in [want, limit] that does not
// separate an assistant tool call from its result. It returns 0 when no safe
// boundary exists in range.
func pairingSafeCut(messages []domain.Message, want, limit int) int {
	for cut := want; cut <= limit && cut < len(messages); cut++ {
		if isPairingSafe(messages, cut) {
			return cut
		}
	}
	return 0
}

// isPairingSafe reports whether splitting before index cut preserves every
// tool_call ↔ tool_result pair.
func isPairingSafe(messages []domain.Message, cut int) bool {
	open := make(map[domain.ToolCallID]struct{})
	for _, msg := range messages[:cut] {
		for _, call := range msg.ToolCalls() {
			open[call.ID] = struct{}{}
		}
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				delete(open, part.ToolResult.CallID)
			}
		}
	}
	if len(open) == 0 {
		return true
	}
	for _, msg := range messages[cut:] {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				if _, waiting := open[part.ToolResult.CallID]; waiting {
					return false
				}
			}
		}
	}
	return true
}

// encodeSpan serializes archived messages as JSON lines, one message per line.
func encodeSpan(messages []domain.Message) ([]byte, error) {
	var b strings.Builder
	enc := json.NewEncoder(&b)
	for _, msg := range messages {
		if err := enc.Encode(msg); err != nil {
			return nil, err
		}
	}
	return []byte(b.String()), nil
}

// archiveMarkerText renders the marker replacing an archived span, pointing
// at the full-fidelity transcript artifact.
func archiveMarkerText(count int, ref domain.ArtifactRef, artifacts domain.ArtifactStore) string {
	locator := ref.ID.String()
	if resolver, ok := artifacts.(artifactPathResolver); ok {
		if path, found := resolver.PathForRef(ref); found {
			locator = path
		}
	}
	return fmt.Sprintf("%s %d messages (full-fidelity JSON transcript) externalized to %s — read specific parts with run_cmd (cat/sed/grep/jq) if needed.]",
		archivedSpanMark, count, locator)
}

// externalize stores one tool output through the generic staged-artifact
// interface so any ArtifactStore implementation works.
func externalize(ctx context.Context, artifacts domain.ArtifactStore, text string) (domain.ArtifactRef, error) {
	stage, err := artifacts.Begin(ctx)
	if err != nil {
		return domain.ArtifactRef{}, err
	}
	if _, err := stage.Write([]byte(text)); err != nil {
		_ = stage.Abort()
		return domain.ArtifactRef{}, err
	}
	return stage.Commit(ctx)
}

// maskPlaceholder renders the replacement text pointing at the externalized
// blob. The absolute path is preferred because the sandboxed process runner
// may read it back with run_cmd; the bare artifact ID is the fallback.
func maskPlaceholder(originalBytes int, ref domain.ArtifactRef, artifacts domain.ArtifactStore) string {
	locator := ref.ID.String()
	if resolver, ok := artifacts.(artifactPathResolver); ok {
		if path, found := resolver.PathForRef(ref); found {
			locator = path
		}
	}
	human := humanBytes(originalBytes)
	return fmt.Sprintf("%s %s externalized to %s — retrieve specific parts with run_cmd (cat/sed/grep) if needed]",
		compactedPlaceholderMark, human, locator)
}

// imageTokenFootprint is the floor byte footprint of one image in
// estTokens' pre-division total (1500 tokens x 4 bytes/token).
const imageTokenFootprint = 1500 * 4

// imageWireFootprint returns the conservative pre-division byte footprint
// of one model-bound image: the flat floor, or the base64 wire size when
// the actual blob size is known. Some gateways meter raw prompt length
// including inline base64 (observed: aigc rejected a nominally ~57k-token
// request carrying ~680KB of images with "Prompt exceeds max length"), so
// the flat floor alone can undercount a wire form by an order of magnitude.
func imageWireFootprint(sizeBytes int64) int {
	wire := sizeBytes * 4 / 3 // base64 inflation
	if wire < imageTokenFootprint {
		return imageTokenFootprint
	}
	return int(wire)
}

// inlineImageFootprint is imageWireFootprint for parts whose Data is
// already base64-encoded: no inflation needed, just the payload length.
func inlineImageFootprint(dataLen int) int {
	if dataLen < imageTokenFootprint {
		return imageTokenFootprint
	}
	return dataLen
}

// EstimateTokens exposes estTokens for read-only projections outside the
// agent package (e.g. the session snapshot's occupancy field).
func EstimateTokens(messages []domain.Message) int { return estTokens(messages) }

// estTokens approximates the token size of the transcript for before/after
// reporting. It is not used for budget accounting.
func estTokens(messages []domain.Message) int {
	total := 0
	for _, msg := range messages {
		for _, part := range msg.Parts {
			switch part.Kind {
			case domain.PartText:
				total += len(part.Text)
			case domain.PartToolCall:
				if part.ToolCall != nil {
					total += len(part.ToolCall.Arguments)
				}
			case domain.PartToolResult:
				if part.ToolResult != nil {
					for _, content := range part.ToolResult.Content {
						switch content.Kind {
						case domain.PartText:
							total += len(content.Text)
						case domain.PartImage:
							if content.Image != nil {
								total += inlineImageFootprint(len(content.Image.Data))
							} else {
								total += imageTokenFootprint
							}
						case domain.PartArtifact:
							// Image references bound for the model are materialized
							// into inline images on the wire (media.Materialize);
							// count their derived footprint. Present-only artifacts
							// are excluded — the model never sees them.
							if media.IsModelImage(content) {
								total += imageWireFootprint(content.Artifact.Size)
							}
						}
					}
				}
			case domain.PartImage:
				// A mid-size photo costs roughly 1.1-1.6k tokens with mainstream
				// vision pricings; the wire-form footprint keeps the estimate
				// conservative for gateways metering raw prompt length.
				if part.Image != nil {
					total += inlineImageFootprint(len(part.Image.Data))
				} else {
					total += imageTokenFootprint
				}
			case domain.PartArtifact:
				// Image attachments bound for the model are persisted as
				// artifact references and materialized at the egress; they cost
				// the same footprint. Present-only artifacts are excluded.
				if media.IsModelImage(part) {
					total += imageWireFootprint(part.Artifact.Size)
				}
			case domain.PartReasoning:
				// Reasoning replayed upstream consumes input budget; count it
				// conservatively so occupancy never underestimates.
				if part.Reasoning != nil {
					total += len(part.Reasoning.Text)
				}
			}
		}
	}
	return total / bytesPerTokenEstimate
}

func humanBytes(n int) string {
	switch {
	case n >= 1<<20:
		return fmt.Sprintf("%.1fMB", float64(n)/float64(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1fKB", float64(n)/float64(1<<10))
	default:
		return fmt.Sprintf("%dB", n)
	}
}
