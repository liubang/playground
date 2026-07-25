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
)

// Condenser configuration defaults. The defaults are chosen so a zero
// Condenser is already safe and useful.
const (
	// defaultKeepRecentMessages protects the tail of the conversation: the
	// model needs its most recent observations to continue coherently.
	defaultKeepRecentMessages = 6
	// defaultMaskMinBytes only externalizes tool outputs large enough to be
	// worth an artifact round-trip.
	defaultMaskMinBytes = 4096
	// defaultTargetTokens is the Level-2 goal: after a compaction pass the
	// estimated transcript size should be at most this many tokens. The
	// transcript is resent with every model call, so bounding it — not the
	// cumulative budget — is what actually keeps long sessions alive.
	defaultTargetTokens = 32_000
	// summaryUserMessageMaxBytes bounds the verbatim user messages kept in
	// a summarized replacement history (≈20k tokens at 4 bytes/token).
	summaryUserMessageMaxBytes = 80 * 1024
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
	// KeepRecentMessages is the number of trailing messages never masked.
	KeepRecentMessages int
	// MaskMinBytes is the minimum tool-output text size externalized.
	MaskMinBytes int
	// TargetTokens is the Level-2 goal for the estimated transcript size.
	TargetTokens int
}

func (c Condenser) withDefaults() Condenser {
	if c.KeepRecentMessages <= 0 {
		c.KeepRecentMessages = defaultKeepRecentMessages
	}
	if c.MaskMinBytes <= 0 {
		c.MaskMinBytes = defaultMaskMinBytes
	}
	if c.TargetTokens <= 0 {
		c.TargetTokens = defaultTargetTokens
	}
	return c
}

// maskedOutput records one externalized tool output for the audit event.
type maskedOutput struct {
	MessageID string `json:"message_id"`
	Bytes     int    `json:"bytes"`
	Artifact  string `json:"artifact"`
}

// contextCompactedPayload is the domain.EventContextCompacted payload.
// Token counts are byte-derived estimates, not provider-metered usage.
type contextCompactedPayload struct {
	MaskedOutputs    int            `json:"masked_outputs"`
	MaskedBytes      int            `json:"masked_bytes"`
	ArchivedMessages int            `json:"archived_messages,omitempty"`
	EstTokensBefore  int            `json:"est_tokens_before"`
	EstTokensAfter   int            `json:"est_tokens_after"`
	Summarized       bool           `json:"summarized,omitempty"`
	SummaryBytes     int            `json:"summary_bytes,omitempty"`
	Outputs          []maskedOutput `json:"outputs,omitempty"`
}

// buildSummaryReplacement rebuilds the transcript after a model-written
// compaction summary: every real user message survives verbatim
// (newest-preferred within a byte budget, the oldest included one is
// truncated), followed by the summary bridge. Masked/archived system
// markers and prior summary bridges are dropped. The result contains only
// user-role messages, so no tool-call pairing invariant can be violated.
func buildSummaryReplacement(messages []domain.Message, summary string, now time.Time) []domain.Message {
	var collected []string
	remaining := summaryUserMessageMaxBytes
	for i := len(messages) - 1; i >= 0 && remaining > 0; i-- {
		msg := messages[i]
		if msg.Role != domain.RoleUser || msg.Metadata["compacted"] != "" {
			continue
		}
		text := strings.Join(msg.TextParts(), "\n")
		if strings.TrimSpace(text) == "" {
			continue
		}
		if len(text) > remaining {
			text = text[:remaining] + "\n[...earlier part of this message truncated...]"
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
	return out
}

// compactResult summarizes one Condense pass.
type compactResult struct {
	outputs     []maskedOutput
	bytesMasked int
	archived    int
}

// artifactPathResolver is implemented by the concrete artifact store; the
// condenser uses it to give the model a directly readable path instead of an
// opaque blob ID.
type artifactPathResolver interface {
	PathForRef(ref domain.ArtifactRef) (string, bool)
}

// Condense compacts the transcript in place through a cost-ordered pipeline:
//
// Level 1: mask oversized tool outputs outside the keep-recent window.
// Level 2a: if the estimate still exceeds TargetTokens, archive the oldest
// message span as one full-fidelity artifact, replaced by a marker.
// Level 2b: if the trailing window alone still exceeds TargetTokens, extend
// masking into the window, protecting only the final message.
//
// Masking never deletes messages and never touches tool calls; archival cuts
// at a tool-pairing-safe boundary, so the assistant tool_call ↔ tool result
// invariant holds throughout. A nil artifact store disables compaction.
func (c Condenser) Condense(ctx context.Context, messages *[]domain.Message, artifacts domain.ArtifactStore) compactResult {
	c = c.withDefaults()
	result := compactResult{}
	if artifacts == nil || len(*messages) == 0 {
		return result
	}

	cutoff := len(*messages) - c.KeepRecentMessages
	if cutoff > 0 {
		c.maskRange(ctx, *messages, artifacts, 0, cutoff, 0, &result)
	}
	if estTokens(*messages) > c.TargetTokens && cutoff > 0 {
		c.archiveOldestSpan(ctx, messages, artifacts, &result)
	}
	if est := estTokens(*messages); est > c.TargetTokens && len(*messages) > 1 {
		// The trailing window alone is too heavy: extend masking into it,
		// protecting only the final message, until the target is met.
		c.maskRange(ctx, *messages, artifacts, 0, len(*messages)-1, c.TargetTokens, &result)
	}
	return result
}

// maskRange externalizes eligible tool outputs in messages[from:to]. When
// stopAtTokens is positive the pass stops as soon as the transcript estimate
// drops to that target.
func (c Condenser) maskRange(ctx context.Context, messages []domain.Message, artifacts domain.ArtifactStore, from, to, stopAtTokens int, result *compactResult) {
	for i := from; i < to && i < len(messages); i++ {
		if err := ctx.Err(); err != nil {
			return
		}
		if stopAtTokens > 0 && estTokens(messages) <= stopAtTokens {
			return
		}
		c.maskMessageOutputs(ctx, &messages[i], artifacts, result)
	}
}

// maskMessageOutputs externalizes every eligible tool output of one message.
func (c Condenser) maskMessageOutputs(ctx context.Context, msg *domain.Message, artifacts domain.ArtifactStore, result *compactResult) {
	changed := false
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
			content.Text = maskPlaceholder(original, ref, artifacts)
			result.bytesMasked += original
			result.outputs = append(result.outputs, maskedOutput{
				MessageID: msg.ID.String(),
				Bytes:     original,
				Artifact:  ref.ID.String(),
			})
			changed = true
		}
	}
	if changed {
		msg.Revision++
	}
}

// archiveOldestSpan serializes the oldest messages into one full-fidelity
// artifact and replaces the span with a single marker message. The cut is
// minimal for meeting TargetTokens and always lands on a tool-pairing-safe
// boundary; it never eats into the keep-recent window.
func (c Condenser) archiveOldestSpan(ctx context.Context, messages *[]domain.Message, artifacts domain.ArtifactStore, result *compactResult) {
	msgs := *messages
	cutoff := len(msgs) - c.KeepRecentMessages
	if cutoff <= 0 {
		return
	}

	// Smallest drop that meets the target; zero means even the window alone
	// exceeds it, in which case archival cannot help (Level 2b takes over).
	drop := 0
	for cut := 1; cut <= cutoff; cut++ {
		if estTokens(msgs[cut:]) <= c.TargetTokens {
			drop = cut
			break
		}
	}
	if drop == 0 {
		return
	}
	cut := pairingSafeCut(msgs, drop, cutoff)
	if cut == 0 {
		return
	}

	data, err := encodeSpan(msgs[:cut])
	if err != nil {
		return
	}
	ref, err := externalize(ctx, artifacts, string(data))
	if err != nil {
		// Archival failure must not drop history: keep the span inline.
		return
	}

	marker := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleSystem,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: archiveMarkerText(cut, ref, artifacts)}},
		CreatedAt: time.Now().UTC(),
		Metadata:  map[string]string{"compacted": "archived"},
		// Inherit the sequence of the last archived message: the marker must
		// satisfy the transcript's positive-and-strictly-increasing invariant
		// (a zero sequence bricks session recovery with "must be positive").
		Sequence: msgs[cut-1].Sequence,
	}
	rest := append([]domain.Message{marker}, msgs[cut:]...)
	*messages = rest
	result.archived = cut
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
						if content.Kind == domain.PartText {
							total += len(content.Text)
						}
					}
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
