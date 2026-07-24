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
	// Tool events
	KindToolPrepared  RuntimeEventKind = "tool.prepared"
	KindToolStarted   RuntimeEventKind = "tool.started"
	KindToolCompleted RuntimeEventKind = "tool.completed"
	KindToolProgress  RuntimeEventKind = "tool.progress"
	// Budget events
	KindBudgetUpdated RuntimeEventKind = "budget.updated"
	// Cancel events
	KindRunCancelRequested RuntimeEventKind = "run.cancel_requested"
	KindRunCancelled       RuntimeEventKind = "run.cancelled"
	KindRunCompleted       RuntimeEventKind = "run.completed"
	// Runtime status
	KindRuntimeWarning RuntimeEventKind = "runtime.warning"
	KindRuntimeFatal   RuntimeEventKind = "runtime.fatal"
	// Usage snapshot
	KindUsageUpdated RuntimeEventKind = "usage.updated"
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
		KindToolPrepared, KindToolStarted, KindToolCompleted, KindToolProgress,
		KindBudgetUpdated, KindUsageUpdated,
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
	// Diff is a compact line diff for file-editing calls, rendered in the
	// approval overlay to support the allow/deny decision.
	Diff string `json:"diff,omitempty"`
}

// ApprovalResolvedPayload describes an approval resolution.
type ApprovalResolvedPayload struct {
	ApprovalID domain.EventID    `json:"approval_id"`
	CallID     domain.ToolCallID `json:"call_id"`
	Decision   domain.Decision   `json:"decision"`
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
	FinishedAt time.Time         `json:"finished_at,omitempty"`
	// Preview is a bounded excerpt of the tool output for expandable display.
	Preview string `json:"preview,omitempty"`
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
