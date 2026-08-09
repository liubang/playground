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

// Package runtimeevent defines the real-time event protocol between the
// agent runtime and frontends (TUI, Linear renderer, JSONL renderer).
//
// RuntimeEvent is distinct from domain.Event: domain events are durable
// facts persisted to the event store, while runtime events include
// ephemeral streaming deltas, progress, and UI state that should not
// pollute the persistent log.
package runtimeevent

import (
	"encoding/json"
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// RuntimeEventVersion is the protocol version for runtime events.
const RuntimeEventVersion = 1

// RuntimeEventKind identifies the type of a runtime event.
type RuntimeEventKind string

const (
	// Session lifecycle
	KindSessionOpened RuntimeEventKind = "session.opened"
	KindSessionClosed RuntimeEventKind = "session.closed"
	// Turn lifecycle
	KindTurnStarted  RuntimeEventKind = "turn.started"
	KindTurnFinished RuntimeEventKind = "turn.finished"
	// Run phase
	KindRunPhaseChanged RuntimeEventKind = "run.phase_changed"
	// Model events
	KindModelRequestStarted    RuntimeEventKind = "model.request_started"
	KindModelTextDelta         RuntimeEventKind = "model.text_delta"
	KindModelReasoningDelta    RuntimeEventKind = "model.reasoning_delta"
	KindModelToolCallDelta     RuntimeEventKind = "model.tool_call_delta"
	KindModelResponseCompleted RuntimeEventKind = "model.response_completed"
	KindModelRequestFailed     RuntimeEventKind = "model.request_failed"
	// Approval events
	KindApprovalRequested RuntimeEventKind = "approval.requested"
	KindApprovalResolved  RuntimeEventKind = "approval.resolved"
	// Question events (model asks the user mid-execution)
	KindQuestionAsked    RuntimeEventKind = "question.asked"
	KindQuestionAnswered RuntimeEventKind = "question.answered"
	// Tool events
	KindToolPrepared  RuntimeEventKind = "tool.prepared"
	KindToolStarted   RuntimeEventKind = "tool.started"
	KindToolCompleted RuntimeEventKind = "tool.completed"
	KindToolProgress  RuntimeEventKind = "tool.progress"
	// Budget events
	KindBudgetUpdated    RuntimeEventKind = "budget.updated"
	KindBudgetNotice     RuntimeEventKind = "budget.notice"
	KindContextCompacted RuntimeEventKind = "context.compacted"
	// Plan events
	KindPlanUpdated RuntimeEventKind = "plan.updated"
	// Steer events (user input submitted while a turn is busy)
	KindSteerQueued   RuntimeEventKind = "steer.queued"
	KindSteerInjected RuntimeEventKind = "steer.injected"
	// Sub-agent lifecycle (delegate_task child runs). All ephemeral: the
	// durable facts live in the child's own session (docs/SUBAGENT_DESIGN.md).
	KindSubagentStarted  RuntimeEventKind = "subagent.started"
	KindSubagentProgress RuntimeEventKind = "subagent.progress"
	KindSubagentFinished RuntimeEventKind = "subagent.finished"
	// Cancel events
	KindRunCancelRequested RuntimeEventKind = "run.cancel_requested"
	KindRunCancelled       RuntimeEventKind = "run.cancelled"
	KindRunCompleted       RuntimeEventKind = "run.completed"
	// Runtime status
	KindRuntimeWarning RuntimeEventKind = "runtime.warning"
	KindRuntimeFatal   RuntimeEventKind = "runtime.fatal"
	// Usage snapshot
	KindUsageUpdated RuntimeEventKind = "usage.updated"
	// Context occupancy snapshot (estimated transcript size for the next call)
	KindContextUsage RuntimeEventKind = "context.usage"
)

// RuntimeEvent is the versioned envelope for real-time communication
// between the agent runtime and frontends.
type RuntimeEvent struct {
	Version   int              `json:"version"`
	Sequence  uint64           `json:"sequence"`
	SessionID domain.SessionID `json:"session_id"`
	RunID     domain.RunID     `json:"run_id,omitempty"`
	Turn      int              `json:"turn,omitempty"`
	Kind      RuntimeEventKind `json:"kind"`
	Time      time.Time        `json:"time"`
	Durable   bool             `json:"durable"`
	Payload   json.RawMessage  `json:"payload,omitempty"`
}

// Validate checks the runtime event is well-formed.
func (e RuntimeEvent) Validate() error {
	if e.Version != RuntimeEventVersion {
		return fmt.Errorf("unsupported runtime event version %d", e.Version)
	}
	if e.Sequence == 0 {
		return fmt.Errorf("runtime event sequence must be positive")
	}
	if e.SessionID.IsZero() {
		return fmt.Errorf("runtime event session ID required")
	}
	switch e.Kind {
	case KindSessionOpened, KindSessionClosed,
		KindTurnStarted, KindTurnFinished,
		KindRunPhaseChanged,
		KindModelRequestStarted, KindModelTextDelta, KindModelReasoningDelta, KindModelToolCallDelta,
		KindModelResponseCompleted, KindModelRequestFailed,
		KindApprovalRequested, KindApprovalResolved,
		KindQuestionAsked, KindQuestionAnswered,
		KindToolPrepared, KindToolStarted, KindToolCompleted, KindToolProgress,
		KindBudgetUpdated, KindUsageUpdated, KindBudgetNotice, KindContextCompacted, KindContextUsage, KindPlanUpdated,
		KindSteerQueued, KindSteerInjected,
		KindSubagentStarted, KindSubagentProgress, KindSubagentFinished,
		KindRunCancelRequested, KindRunCancelled, KindRunCompleted,
		KindRuntimeWarning, KindRuntimeFatal:
	default:
		return fmt.Errorf("unknown runtime event kind %q", e.Kind)
	}
	if len(e.Payload) > 0 && !json.Valid(e.Payload) {
		return fmt.Errorf("invalid runtime event payload JSON")
	}
	return nil
}

// --- Payload DTOs (sanitized for UI display) ---

// SessionOpenedPayload describes a session that was opened or resumed.
type SessionOpenedPayload struct {
	Model        string `json:"model"`
	Workspace    string `json:"workspace"`
	Resumed      bool   `json:"resumed"`
	MessageCount int    `json:"message_count,omitempty"`
}

// TurnStartedPayload describes the start of a new turn.
type TurnStartedPayload struct {
	TurnIndex int    `json:"turn_index"`
	Prompt    string `json:"prompt,omitempty"`
}

// RunPhasePayload describes a phase change.
type RunPhasePayload struct {
	Phase domain.Phase `json:"phase"`
}

// ModelRequestStartedPayload describes the start of a model request.
type ModelRequestStartedPayload struct {
	RequestID domain.EventID `json:"request_id"`
	ModelName string         `json:"model_name"`
	Turn      int            `json:"turn"`
}

// ModelTextDeltaPayload carries a text delta for streaming display.
type ModelTextDeltaPayload struct {
	RequestID domain.EventID `json:"request_id"`
	Delta     string         `json:"delta"`
}

// ModelReasoningDeltaPayload carries provider-supplied reasoning for display.
type ModelReasoningDeltaPayload struct {
	RequestID domain.EventID `json:"request_id"`
	Delta     string         `json:"delta"`
}

// ModelToolCallDeltaPayload carries tool call progress.
type ModelToolCallDeltaPayload struct {
	RequestID  domain.EventID `json:"request_id"`
	ToolIndex  int            `json:"tool_index"`
	ToolName   string         `json:"tool_name,omitempty"`
	ToolID     string         `json:"tool_id,omitempty"`
	Arguments  string         `json:"arguments,omitempty"`
	DeltaBytes int            `json:"delta_bytes"`
}

// ModelResponseCompletedPayload marks the completion of a model response.
type ModelResponseCompletedPayload struct {
	RequestID    domain.EventID    `json:"request_id"`
	StopReason   domain.StopReason `json:"stop_reason"`
	InputTokens  int64             `json:"input_tokens"`
	OutputTokens int64             `json:"output_tokens"`
	HasToolCalls bool              `json:"has_tool_calls"`
	// Text is the canonical visible text of the persisted assistant message.
	// Frontends use it to correct drafts assembled from lossy deltas.
	Text string `json:"text,omitempty"`
}

// ModelRequestFailedPayload describes a model request failure.
type ModelRequestFailedPayload struct {
	RequestID domain.EventID `json:"request_id"`
	Stage     string         `json:"stage"`
	Code      string         `json:"code"`
	// Message carries the underlying error text (rate limit, network
	// failure, ...) so clients can show a diagnosable reason.
	Message string `json:"message,omitempty"`
}

// ApprovalRequestedPayload describes an approval request.
type ApprovalRequestedPayload struct {
	ApprovalID  domain.EventID    `json:"approval_id"`
	CallID      domain.ToolCallID `json:"call_id"`
	ToolName    string            `json:"tool_name"`
	Risk        domain.RiskLevel  `json:"risk"`
	Description string            `json:"description"`
	ArgsHash    string            `json:"args_hash"`
	ReadPaths   []string          `json:"read_paths,omitempty"`
	WritePaths  []string          `json:"write_paths,omitempty"`
	// The argument diff is NOT part of this payload: it already travels
	// with tool.prepared (single source), and frontends render it on the
	// tool block. Duplicating it here sent the same diff twice per call.
	// Arguments carries the raw call arguments so an "allow always" decision
	// can derive a categorical approval rule. It is display-safe (the
	// approval description already shows the same information).
	Arguments json.RawMessage `json:"arguments,omitempty"`
	// RulePreview renders what "allow always" would remember for this call
	// (an argv prefix, an exact host, or the bare tool name). Empty means
	// the call cannot be remembered; frontends should hide the "allow
	// always" affordance rather than offer a no-op.
	RulePreview string `json:"rule_preview,omitempty"`
}

// ApprovalResolvedPayload describes an approval resolution.
type ApprovalResolvedPayload struct {
	ApprovalID domain.EventID    `json:"approval_id"`
	CallID     domain.ToolCallID `json:"call_id"`
	Decision   domain.Decision   `json:"decision"`
	// Actor identifies who resolved the approval (e.g. a named serve client,
	// or "system:timeout"); empty for the local interactive frontend.
	Actor string `json:"actor,omitempty"`
}

// QuestionAskedPayload carries a model question to the interactive
// frontend. The frontend resolves it by id via the controller.
type QuestionAskedPayload struct {
	QuestionID    domain.EventID          `json:"question_id"`
	Text          string                  `json:"text"`
	Options       []domain.QuestionOption `json:"options"`
	AllowMultiple bool                    `json:"allow_multiple,omitempty"`
}

// QuestionAnsweredPayload reports the resolution of a model question.
type QuestionAnsweredPayload struct {
	QuestionID domain.EventID `json:"question_id"`
	Skipped    bool           `json:"skipped,omitempty"`
}

// ToolPreparedPayload describes a prepared tool call.
type ToolPreparedPayload struct {
	CallID   domain.ToolCallID `json:"call_id"`
	ToolName string            `json:"tool_name"`
	Risk     domain.RiskLevel  `json:"risk"`
	// Target is the primary subject of the call (path, pattern or command)
	// for one-line display.
	Target string `json:"target,omitempty"`
	// Diff is a compact line diff for file-editing calls, shown when the
	// tool block is expanded.
	Diff string `json:"diff,omitempty"`
}

// ToolStartedPayload describes the start of tool execution.
type ToolStartedPayload struct {
	CallID    domain.ToolCallID `json:"call_id"`
	ToolName  string            `json:"tool_name"`
	StartedAt time.Time         `json:"started_at"`
}

// ToolCompletedPayload describes the completion of tool execution.
type ToolCompletedPayload struct {
	CallID     domain.ToolCallID `json:"call_id"`
	ToolName   string            `json:"tool_name"`
	Status     domain.ToolStatus `json:"status"`
	DurationMs int64             `json:"duration_ms"`
	Error      string            `json:"error,omitempty"`
	// ErrorMessage is the human-readable failure reason (e.g. "request
	// failed with status 418"), shown inline next to the error code.
	ErrorMessage string    `json:"error_message,omitempty"`
	FinishedAt   time.Time `json:"finished_at,omitempty"`
	// Preview is a bounded excerpt of the tool output for expandable display.
	Preview string `json:"preview,omitempty"`
	// Artifacts carries the artifact references found in the tool result
	// content (e.g. generate_image output), so live clients can render them
	// without waiting for a snapshot rebuild. References are tiny (id + size);
	// the blob bytes never travel on the event stream.
	Artifacts []domain.ArtifactRef `json:"artifacts,omitempty"`
	// Images carries the inline image content parts found in the tool result
	// (e.g. view_image output), so live clients can render them without waiting
	// for a snapshot rebuild. Each entry carries a base64 payload; only tools
	// that produce inline images (view_image, generate_image when under the
	// inline size limit) populate this field.
	Images []domain.ImageContent `json:"images,omitempty"`
}

// ToolProgressPayload describes bounded progress.
type ToolProgressPayload struct {
	CallID   domain.ToolCallID `json:"call_id"`
	Stage    string            `json:"stage"`
	Progress float64           `json:"progress,omitempty"`
}

// BudgetUpdatedPayload describes a budget update.
type BudgetUpdatedPayload struct {
	Turns        int   `json:"turns"`
	InputTokens  int64 `json:"input_tokens"`
	OutputTokens int64 `json:"output_tokens"`
	ToolCalls    int   `json:"tool_calls"`
}

// UsageUpdatedPayload is a lightweight usage snapshot.
type UsageUpdatedPayload struct {
	InputTokens  int64 `json:"input_tokens"`
	OutputTokens int64 `json:"output_tokens"`
	Turns        int   `json:"turns"`
}

// ContextCompactedPayload summarizes one context compaction pass. Token
// counts are estimates (bytes/4), not provider-metered usage. Summarized
// marks a model-written handoff compaction (history rebuilt around a
// summary), which frontends flag as accuracy-relevant.
type ContextCompactedPayload struct {
	Trigger          string `json:"trigger,omitempty"`
	Phase            string `json:"phase,omitempty"`
	MaskedOutputs    int    `json:"masked_outputs"`
	MaskedBytes      int    `json:"masked_bytes"`
	ArchivedMessages int    `json:"archived_messages,omitempty"`
	EstTokensBefore  int    `json:"est_tokens_before"`
	EstTokensAfter   int    `json:"est_tokens_after"`
	Summarized       bool   `json:"summarized,omitempty"`
}

// BudgetNoticePayload describes one graduated budget reminder injected
// into the transcript (budget notices, runaway warnings) and the
// soft-landing wrap-up entry (level 0 with the wrap-up dimension).
type BudgetNoticePayload struct {
	Text      string `json:"text"`
	Dimension string `json:"dimension"`
	Level     int    `json:"level"`
	WrapUp    bool   `json:"wrap_up,omitempty"`
}

// ContextUsagePayload reports the estimated size of the transcript the next
// model request would carry, plus the provider-metered input tokens of the
// last completed call when known. Estimates are byte/4 approximations.
type ContextUsagePayload struct {
	EstTokens           int   `json:"est_tokens"`
	LastCallInputTokens int64 `json:"last_call_input_tokens,omitempty"`
}

// SteerQueuedPayload reports a user message accepted into the pending-steer
// queue while a turn is busy. Ephemeral: the pending panel rebuilds from
// Snapshot.PendingSteers after a resubscribe.
type SteerQueuedPayload struct {
	Text     string `json:"text"`
	QueueLen int    `json:"queue_len"`
}

// SteerInjectedPayload reports a queued message drained by the agent loop
// and persisted as a regular user message before a model call. Durable: it
// drives the pending-panel removal and the transcript's user block.
type SteerInjectedPayload struct {
	Text string `json:"text"`
}

// SubagentStartedPayload reports that a delegate_task call spawned its
// child run. Frontends bind it to the delegate tool block via CallID and
// use ChildSessionID for the read-only drill-in view.
type SubagentStartedPayload struct {
	CallID         domain.ToolCallID `json:"call_id"`
	ChildSessionID domain.SessionID  `json:"child_session_id"`
	Task           string            `json:"task"`
}

// SubagentProgressPayload is a periodic counters snapshot of a running
// child, pulled from the child checkpoint (the child loop publishes no
// events of its own). ElapsedMs is wall time since the child started.
type SubagentProgressPayload struct {
	CallID         domain.ToolCallID `json:"call_id"`
	ChildSessionID domain.SessionID  `json:"child_session_id"`
	ToolCalls      int               `json:"tool_calls"`
	InputTokens    int64             `json:"input_tokens"`
	OutputTokens   int64             `json:"output_tokens"`
	ElapsedMs      int64             `json:"elapsed_ms"`
}

// SubagentFinishedPayload reports the child run's terminal outcome with
// its final counters.
type SubagentFinishedPayload struct {
	CallID         domain.ToolCallID `json:"call_id"`
	ChildSessionID domain.SessionID  `json:"child_session_id"`
	Outcome        string            `json:"outcome"`
	ToolCalls      int               `json:"tool_calls"`
	InputTokens    int64             `json:"input_tokens"`
	OutputTokens   int64             `json:"output_tokens"`
}

// RunCancelledPayload describes a cancellation.
type RunCancelledPayload struct {
	Reason string `json:"reason"`
}

// TurnFinishedPayload describes the end of a turn. Error is empty for a clean
// finish and otherwise carries the user-visible failure summary.
type TurnFinishedPayload struct {
	Error string `json:"error,omitempty"`
}

// RuntimeWarningPayload describes a non-fatal runtime warning.
type RuntimeWarningPayload struct {
	Message string `json:"message"`
}

// RuntimeFatalPayload describes a fatal runtime error.
type RuntimeFatalPayload struct {
	Message string `json:"message"`
}
