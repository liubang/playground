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
// Created: 2026/07/23

package ui

import (
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/render"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// BlockKind identifies the type of a transcript block.
type BlockKind string

const (
	BlockKindUser        BlockKind = "user"
	BlockKindAssistant   BlockKind = "assistant"
	BlockKindTool        BlockKind = "tool"
	BlockKindNotice      BlockKind = "notice"
	BlockKindInterrupted BlockKind = "interrupted"
)

// TranscriptBlock is a stable-ID unit in the transcript view.
type TranscriptBlock struct {
	ID      string
	Kind    BlockKind
	Title   string
	Content string
	Detail  string // secondary info (duration, lines, etc.)
	Status  string // running | success | error | cancelled
	Tool    string // tool name if applicable
	Risk    domain.RiskLevel

	// Tool call display: Target is the primary subject (path, pattern or
	// command); Preview is a bounded result excerpt shown when Expanded;
	// Diff is a compact argument diff for file-editing calls.
	Target   string
	Preview  string
	Diff     string
	Expanded bool

	// streaming state
	StreamText        string
	StreamReasoning   string
	ReasoningExpanded bool
	PreparingTool     string
	Done              bool

	// tool timing
	StartedAt  time.Time
	FinishedAt time.Time

	// tool call ID for indexing
	CallID domain.ToolCallID

	// Approval info (tool blocks)
	ApprovalID domain.EventID
	ArgsHash   string
}

// BlockIndex stores transcript blocks indexed by their stable IDs.
type BlockIndex struct {
	Order []string
	ByID  map[string]*TranscriptBlock
}

// NewBlockIndex creates an empty BlockIndex.
func NewBlockIndex() *BlockIndex {
	return &BlockIndex{
		Order: []string{},
		ByID:  make(map[string]*TranscriptBlock),
	}
}

// Add inserts or updates a block.
func (idx *BlockIndex) Add(b *TranscriptBlock) {
	if _, exists := idx.ByID[b.ID]; !exists {
		idx.Order = append(idx.Order, b.ID)
	}
	idx.ByID[b.ID] = b
}

// Remove deletes a block by ID. It is a no-op for unknown IDs.
func (idx *BlockIndex) Remove(id string) {
	if _, ok := idx.ByID[id]; !ok {
		return
	}
	delete(idx.ByID, id)
	for i, blockID := range idx.Order {
		if blockID == id {
			idx.Order = append(idx.Order[:i], idx.Order[i+1:]...)
			return
		}
	}
}

// Get retrieves a block by ID.
func (idx *BlockIndex) Get(id string) (*TranscriptBlock, bool) {
	b, ok := idx.ByID[id]
	return b, ok
}

// AddPendingUserBlock adds a local echo before the controller publishes turn.started.
func (idx *BlockIndex) AddPendingUserBlock(prompt string) string {
	block := &TranscriptBlock{
		ID:      fmt.Sprintf("pending-user-%d", time.Now().UnixNano()),
		Kind:    BlockKindUser,
		Title:   "You",
		Content: prompt,
		Status:  "pending",
		Done:    true,
	}
	idx.Add(block)
	return block.ID
}

func (idx *BlockIndex) confirmPendingUserBlock(prompt string) *TranscriptBlock {
	for i := len(idx.Order) - 1; i >= 0; i-- {
		block := idx.ByID[idx.Order[i]]
		if block.Kind == BlockKindUser && block.Status == "pending" && block.Content == prompt {
			block.Status = "success"
			return block
		}
	}
	return nil
}

// ApplyRuntimeEvent applies a runtime event to the block index, returning
// the affected block ID (if any).
func ApplyRuntimeEvent(idx *BlockIndex, evt runtimeevent.RuntimeEvent) string {
	switch evt.Kind {
	case runtimeevent.KindTurnStarted:
		var payload runtimeevent.TurnStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			if block := idx.confirmPendingUserBlock(payload.Prompt); block != nil {
				return block.ID
			}
			block := &TranscriptBlock{
				ID:      fmt.Sprintf("user-%d", evt.Sequence),
				Kind:    BlockKindUser,
				Title:   "You",
				Content: payload.Prompt,
				Done:    true,
			}
			idx.Add(block)
			return block.ID
		}

	case runtimeevent.KindModelReasoningDelta:
		block := ensureStreamBlock(idx, evt.Turn)
		var payload runtimeevent.ModelReasoningDeltaPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block.StreamReasoning += payload.Delta
		}
		block.Done = false
		return block.ID

	case runtimeevent.KindModelTextDelta:
		block := ensureStreamBlock(idx, evt.Turn)
		var payload runtimeevent.ModelTextDeltaPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block.StreamText += payload.Delta
			block.Content = block.StreamText
		}
		block.Done = false
		return block.ID

	case runtimeevent.KindModelToolCallDelta:
		var payload runtimeevent.ModelToolCallDeltaPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block := ensureStreamBlock(idx, evt.Turn)
			block.PreparingTool = payload.ToolName
			block.Done = false
			return block.ID
		}

	case runtimeevent.KindModelResponseCompleted:
		var payload runtimeevent.ModelResponseCompletedPayload
		_ = json.Unmarshal(evt.Payload, &payload)
		// The persisted canonical text corrects drafts assembled from deltas,
		// which may have been dropped under backpressure.
		canonical := payload.Text
		// Find the streaming block for this turn and finalize it.
		for i := len(idx.Order) - 1; i >= 0; i-- {
			b := idx.ByID[idx.Order[i]]
			if b.Kind == BlockKindAssistant && !b.Done && b.Tool == "" {
				b.Done = true
				b.PreparingTool = ""
				if canonical != "" {
					b.Content = render.SanitizeText(canonical)
				} else {
					b.Content = render.SanitizeText(b.StreamText)
				}
				b.StreamText = ""
				return b.ID
			}
		}
		// Every delta was lost: surface the canonical text as a final block
		// instead of leaving the turn blank.
		if canonical != "" {
			block := &TranscriptBlock{
				ID:      fmt.Sprintf("final-%d", evt.Sequence),
				Kind:    BlockKindAssistant,
				Title:   "Assistant",
				Content: render.SanitizeText(canonical),
				Done:    true,
			}
			idx.Add(block)
			return block.ID
		}

	case runtimeevent.KindModelRequestFailed:
		// Mark any in-progress stream as interrupted.
		for i := len(idx.Order) - 1; i >= 0; i-- {
			b := idx.ByID[idx.Order[i]]
			if b.Kind == BlockKindAssistant && !b.Done {
				b.Done = true
				b.Kind = BlockKindInterrupted
				b.PreparingTool = ""
				b.Content = render.SanitizeText(b.StreamText) + "\n[interrupted]"
				b.StreamText = ""
				b.Status = "error"
				return b.ID
			}
		}

	case runtimeevent.KindApprovalRequested:
		var payload runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
				block := &TranscriptBlock{
					ID:         fmt.Sprintf("tool-%s", payload.CallID),
					Kind:       BlockKindTool,
					Title:      payload.ToolName,
					Content:    payload.Description,
					Status:     "approval",
					Tool:       payload.ToolName,
					Risk:       payload.Risk,
					CallID:     payload.CallID,
					ApprovalID: payload.ApprovalID,
					ArgsHash:   payload.ArgsHash,
					Target:     approvalTarget(payload),
					Diff:       payload.Diff,
				}
			idx.Add(block)
			return block.ID
		}

	case runtimeevent.KindApprovalResolved:
		var payload runtimeevent.ApprovalResolvedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block, exists := idx.Get(fmt.Sprintf("tool-%s", payload.CallID))
			if exists {
				if payload.Decision == domain.DecisionAllow {
					block.Status = "running"
				} else {
					block.Status = "cancelled"
					block.Detail = "denied"
				}
				return block.ID
			}
		}

	case runtimeevent.KindToolStarted:
		var payload runtimeevent.ToolStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block, exists := idx.Get(fmt.Sprintf("tool-%s", payload.CallID))
			if exists {
				block.Status = "running"
				block.StartedAt = payload.StartedAt
				return block.ID
			}
		}

	case runtimeevent.KindToolCompleted:
		var payload runtimeevent.ToolCompletedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			block, exists := idx.Get(fmt.Sprintf("tool-%s", payload.CallID))
			if exists {
				block.Done = true
				var details []string
				if payload.Status == domain.ToolStatusSuccess {
					block.Status = "success"
				} else {
					block.Status = "error"
					if payload.Error != "" {
						details = append(details, payload.Error)
					}
				}
				block.FinishedAt = payload.FinishedAt
				if block.FinishedAt.IsZero() {
					block.FinishedAt = time.Now().UTC()
				}
				if payload.DurationMs > 0 {
					details = append(details, fmt.Sprintf("%dms", payload.DurationMs))
				}
				// Error context and duration coexist; neither overwrites the other.
				block.Detail = strings.Join(details, " · ")
				block.Preview = render.SanitizeText(payload.Preview)
				return block.ID
			}
		}

	case runtimeevent.KindToolPrepared:
		var payload runtimeevent.ToolPreparedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			blockID := fmt.Sprintf("tool-%s", payload.CallID)
			if _, exists := idx.Get(blockID); !exists {
				block := &TranscriptBlock{
					ID:     blockID,
					Kind:   BlockKindTool,
					Title:  payload.ToolName,
					Status: "prepared",
					Tool:   payload.ToolName,
					Risk:   payload.Risk,
					CallID: payload.CallID,
					Target: payload.Target,
					Diff:   payload.Diff,
				}
				idx.Add(block)
				return block.ID
			}
		}

	case runtimeevent.KindRunCancelRequested, runtimeevent.KindRunCompleted:
		// No transcript block: the status bar already reflects these states, and
		// a notice per turn would bury the conversation in lifecycle spam.

	case runtimeevent.KindTurnFinished:
		var payload runtimeevent.TurnFinishedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.Error != "" {
			block := &TranscriptBlock{
				ID:      fmt.Sprintf("notice-%d", evt.Sequence),
				Kind:    BlockKindNotice,
				Content: fmt.Sprintf("Turn ended with error: %s", payload.Error),
				Done:    true,
				Status:  "error",
			}
			idx.Add(block)
			return block.ID
		}

	case runtimeevent.KindRunCancelled:
		// Mark any non-done blocks as cancelled.
		for _, id := range idx.Order {
			b := idx.ByID[id]
			if !b.Done {
				b.Status = "cancelled"
				b.Done = true
			}
		}
		block := &TranscriptBlock{
			ID:      fmt.Sprintf("notice-%d", evt.Sequence),
			Kind:    BlockKindNotice,
			Content: "Turn cancelled",
			Done:    true,
			Status:  "cancelled",
		}
		idx.Add(block)
		return block.ID

	case runtimeevent.KindRuntimeFatal, runtimeevent.KindRuntimeWarning:
		var msg string
		if evt.Kind == runtimeevent.KindRuntimeFatal {
			var payload runtimeevent.RuntimeFatalPayload
			if err := json.Unmarshal(evt.Payload, &payload); err == nil {
				msg = payload.Message
			}
		} else {
			var payload runtimeevent.RuntimeWarningPayload
			if err := json.Unmarshal(evt.Payload, &payload); err == nil {
				msg = payload.Message
			}
		}
		if msg != "" {
			block := &TranscriptBlock{
				ID:      fmt.Sprintf("notice-%d", evt.Sequence),
				Kind:    BlockKindNotice,
				Content: msg,
				Done:    true,
				Status:  "error",
			}
			idx.Add(block)
			return block.ID
		}
	}
	return ""
}

// ToggleLatestReasoning changes whether the most recent assistant reasoning is visible.
func (idx *BlockIndex) ToggleLatestReasoning() bool {
	for i := len(idx.Order) - 1; i >= 0; i-- {
		block := idx.ByID[idx.Order[i]]
		if block.Kind == BlockKindAssistant && block.StreamReasoning != "" {
			block.ReasoningExpanded = !block.ReasoningExpanded
			return true
		}
	}
	return false
}

// ToggleLatestToolOutput flips the result preview of the most recent tool
// block that has one.
func (idx *BlockIndex) ToggleLatestToolOutput() bool {
	for i := len(idx.Order) - 1; i >= 0; i-- {
		block := idx.ByID[idx.Order[i]]
		if block.Kind == BlockKindTool && block.Preview != "" {
			block.Expanded = !block.Expanded
			return true
		}
	}
	return false
}

// ToggleAllToolOutputs expands every tool block that has displayable content
// (preview or diff), or collapses all of them when any is already expanded.
func (idx *BlockIndex) ToggleAllToolOutputs() bool {
	target := true
	for _, id := range idx.Order {
		b := idx.ByID[id]
		if b.Kind == BlockKindTool && b.Expanded {
			target = false
			break
		}
	}
	changed := false
	for _, id := range idx.Order {
		b := idx.ByID[id]
		if b.Kind != BlockKindTool || (b.Preview == "" && b.Diff == "") {
			continue
		}
		if b.Expanded != target {
			b.Expanded = target
			changed = true
		}
	}
	return changed
}

// LatestFinalAssistantText returns the content of the most recent finalized
// assistant block, used by copy-to-clipboard.
func (idx *BlockIndex) LatestFinalAssistantText() string {
	for i := len(idx.Order) - 1; i >= 0; i-- {
		b := idx.ByID[idx.Order[i]]
		if b.Kind == BlockKindAssistant && b.Done && strings.TrimSpace(b.Content) != "" {
			return b.Content
		}
	}
	return ""
}

// approvalTarget picks the display target for an approval-requested call.
func approvalTarget(payload runtimeevent.ApprovalRequestedPayload) string {
	if len(payload.WritePaths) > 0 {
		return payload.WritePaths[0]
	}
	if len(payload.ReadPaths) > 0 {
		return payload.ReadPaths[0]
	}
	return ""
}

func ensureStreamBlock(idx *BlockIndex, turn int) *TranscriptBlock {
	streamID := fmt.Sprintf("stream-%d", turn)
	if block, exists := idx.Get(streamID); exists {
		return block
	}
	block := &TranscriptBlock{ID: streamID, Kind: BlockKindAssistant, Title: "Assistant"}
	idx.Add(block)
	return block
}

// RebuildTranscript creates display blocks from a persisted transcript.
// Besides visible text it also restores the tool-call history, marking calls
// without a recorded result as needing manual verification (they may have
// produced side effects before an interruption).
func RebuildTranscript(messages []domain.Message) *BlockIndex {
	idx := NewBlockIndex()

	// Tool results live in separate tool messages; index them by call first so
	// assistant tool-call parts can be annotated with their outcome.
	results := make(map[domain.ToolCallID]domain.ToolResult)
	for _, message := range messages {
		for _, part := range message.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				results[part.ToolResult.CallID] = *part.ToolResult
			}
		}
	}

	for _, message := range messages {
		var kind BlockKind
		var title string
		switch message.Role {
		case domain.RoleUser:
			kind, title = BlockKindUser, "You"
		case domain.RoleAssistant:
			kind, title = BlockKindAssistant, "Assistant"
		default:
			continue
		}

		var text strings.Builder
		for _, part := range message.Parts {
			if part.Kind == domain.PartText {
				text.WriteString(part.Text)
			}
		}
		if text.Len() > 0 {
			block := &TranscriptBlock{
				ID:      fmt.Sprintf("message-%s", message.ID),
				Kind:    kind,
				Title:   title,
				Content: render.SanitizeText(text.String()),
				Done:    true,
			}
			if message.Status == domain.MessageStatusInterrupted {
				block.Kind = BlockKindInterrupted
				block.Status = "error"
			}
			idx.Add(block)
		}

		for _, part := range message.Parts {
			if part.Kind != domain.PartToolCall || part.ToolCall == nil {
				continue
			}
			call := part.ToolCall
			toolBlock := &TranscriptBlock{
				ID:     fmt.Sprintf("tool-%s", call.ID),
				Kind:   BlockKindTool,
				Title:  call.Name,
				Tool:   call.Name,
				CallID: call.ID,
				Done:   true,
				Status: "success",
				Target: toolTargetFromArgs(call.Arguments),
				Diff:   render.DiffForToolCall(call.Name, call.Arguments, toolDiffMaxLines),
			}
			result, ok := results[call.ID]
			if !ok {
				// Intent without a recorded outcome must not look successful.
				toolBlock.Status = "cancelled"
				toolBlock.Detail = "no recorded result — verify side effects manually"
				idx.Add(toolBlock)
				continue
			}
			toolBlock.StartedAt = result.StartedAt
			toolBlock.FinishedAt = result.FinishedAt
			var details []string
			if result.Status != domain.ToolStatusSuccess {
				toolBlock.Status = "error"
				if result.Error != nil && result.Error.Code != "" {
					details = append(details, result.Error.Code)
				}
			}
			if d := result.FinishedAt.Sub(result.StartedAt); d > 0 {
				details = append(details, fmt.Sprintf("%dms", d.Milliseconds()))
			}
			toolBlock.Detail = strings.Join(details, " · ")
			toolBlock.Preview = render.SanitizeText(toolResultPreviewText(result))
			idx.Add(toolBlock)
		}
	}
	return idx
}

// Bounds for the persisted-result preview shown by expandable tool blocks.
const (
	toolPreviewMaxLines = 12
	toolPreviewMaxBytes = 1200
)

// toolDiffMaxLines bounds the argument diff rendered for edit/write calls.
const toolDiffMaxLines = 40

// toolTargetFromArgs extracts the primary display target (path, command or
// pattern) from raw tool call arguments.
func toolTargetFromArgs(args json.RawMessage) string {
	if len(args) == 0 {
		return ""
	}
	var parsed map[string]any
	if err := json.Unmarshal(args, &parsed); err != nil {
		return ""
	}
	for _, key := range []string{"path", "command", "cmd", "pattern", "query", "url", "file", "dir", "directory"} {
		value, ok := parsed[key]
		if !ok {
			continue
		}
		if s, ok := value.(string); ok && s != "" {
			return s
		}
	}
	return ""
}

// toolResultPreviewText extracts a bounded text excerpt from a persisted
// tool result: joined text parts, falling back to the error message.
func toolResultPreviewText(result domain.ToolResult) string {
	var b strings.Builder
	for _, cp := range result.Content {
		if cp.Kind == domain.PartText {
			b.WriteString(cp.Text)
		}
	}
	text := b.String()
	if strings.TrimSpace(text) == "" && result.Error != nil {
		text = result.Error.Message
	}
	text = strings.TrimSpace(text)
	if text == "" {
		return ""
	}
	truncated := false
	lines := strings.Split(text, "\n")
	if len(lines) > toolPreviewMaxLines {
		lines = lines[:toolPreviewMaxLines]
		truncated = true
	}
	out := strings.Join(lines, "\n")
	if len(out) > toolPreviewMaxBytes {
		out = out[:toolPreviewMaxBytes]
		truncated = true
	}
	if truncated {
		out += "\n…"
	}
	return out
}

// The one-line summary of a tool block is rendered by the view layer
// (see renderToolSummary), which adds animation and live elapsed time.
