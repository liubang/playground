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
// Created: 2026/07/22 21:10

package domain

import (
	"context"
	"fmt"
	"io"
	"time"
)

// --- Tool interface (§9.1) ---

// Tool is the core tool abstraction. Prepare must be side-effect-free.
type Tool interface {
	Definition() ToolDefinition
	Prepare(ctx context.Context, call ToolCall) (PreparedCall, error)
	Execute(ctx context.Context, prepared PreparedCall) ToolResult
}

// ConcurrentSafely is an optional opt-in interface: the tool's Execute
// is safe to run concurrently with other tool executions in the same
// batch (docs/SUBAGENT_DESIGN.md §11). Eligibility is a property of the
// implementation, not of a single call: the tool must share no mutable
// state across executions except mutex-protected infrastructure
// (file-state book, artifact store, response caches), and its side
// effects must be confined to its own call (reads, or — delegate_task —
// a brand-new isolated session). Tools that write the workspace, spawn
// foreground processes, or interact with the user must not opt in.
type ConcurrentSafely interface {
	ConcurrentSafe() bool
}

// ToolConcurrentSafe reports whether the tool opted into concurrent
// batch execution. Missing opt-in means serial — the safe default.
func ToolConcurrentSafe(t Tool) bool {
	cs, ok := t.(ConcurrentSafely)
	return ok && cs.ConcurrentSafe()
}

// --- Model interface (§7) ---

// ReasoningEffort expresses how much reasoning ("thinking") the model should
// perform before answering, in vendor-neutral levels. The empty value means
// "provider default" — the request carries no opinion and the provider
// behaves as if reasoning were never mentioned.
type ReasoningEffort string

const (
	// ReasoningEffortOff explicitly disables reasoning. Providers without a
	// portable off switch treat it as "no reasoning requested".
	ReasoningEffortOff    ReasoningEffort = "off"
	ReasoningEffortLow    ReasoningEffort = "low"
	ReasoningEffortMedium ReasoningEffort = "medium"
	ReasoningEffortHigh   ReasoningEffort = "high"
)

// ReasoningSpec is the vendor-neutral reasoning request carried by every
// model call. Each provider maps it onto its own wire representation:
// Anthropic derives thinking.budget_tokens from Effort (BudgetTokens wins
// when explicit); OpenAI-compatible providers map Effort onto
// reasoning_effort and ignore BudgetTokens.
type ReasoningSpec struct {
	Effort       ReasoningEffort `json:"effort,omitempty"`
	BudgetTokens int64           `json:"budget_tokens,omitempty"`
}

// IsZero reports whether the spec carries any opinion at all.
func (s ReasoningSpec) IsZero() bool {
	return s.Effort == "" && s.BudgetTokens == 0
}

// Label renders the spec for compact status display; "" means the provider
// decides. Single home for what used to be duplicated in app and ui
// (REVIEW R8).
func (s ReasoningSpec) Label() string {
	if s.Effort != "" {
		return string(s.Effort)
	}
	if s.BudgetTokens > 0 {
		return fmt.Sprintf("budget:%d", s.BudgetTokens)
	}
	return ""
}

// Enabled reports whether reasoning should be turned on for this call.
func (s ReasoningSpec) Enabled() bool {
	return s.BudgetTokens > 0 || (s.Effort != "" && s.Effort != ReasoningEffortOff)
}

// ResponseFormat constrains the model response to a JSON schema. Only
// providers whose wire API supports structured outputs honor it; the rest
// silently ignore it, so callers must keep a prompt-level description of
// the format and a lenient parse fallback.
type ResponseFormat struct {
	// Name is a short identifier for the schema (provider-side bookkeeping).
	Name string
	// Schema is a JSON Schema document (already decoded for easy building).
	Schema map[string]any
	// Strict requests exact schema conformance where the wire API supports it.
	Strict bool
}

// ModelRequest is the unified input to a model provider.
type ModelRequest struct {
	ID              EventID
	ModelName       string
	Messages        []Message
	Tools           []ToolDefinition
	MaxTokens       int64
	Temperature     float64
	Reasoning       ReasoningSpec
	ContextManifest ContextManifest
	// ResponseFormat optionally requests schema-constrained JSON output.
	ResponseFormat *ResponseFormat
}

// StopReason classifies why the model stopped generating.
type StopReason string

const (
	StopEndTurn       StopReason = "end_turn"
	StopToolUse       StopReason = "tool_use"
	StopMaxOutput     StopReason = "max_output"
	StopContentFilter StopReason = "content_filter"
	StopCancelled     StopReason = "cancelled"
	StopProviderError StopReason = "provider_error"
	StopUnknown       StopReason = "unknown"
)

// ModelEventKind identifies the type of a streaming event from the model.
type ModelEventKind string

const (
	ModelEventResponseStart   ModelEventKind = "response_start"
	ModelEventTextStart       ModelEventKind = "text_start"
	ModelEventTextDelta       ModelEventKind = "text_delta"
	ModelEventTextEnd         ModelEventKind = "text_end"
	ModelEventReasoningStart  ModelEventKind = "reasoning_start"
	ModelEventReasoningDelta  ModelEventKind = "reasoning_delta"
	ModelEventReasoningEnd    ModelEventKind = "reasoning_end"
	ModelEventToolCallStart   ModelEventKind = "tool_call_start"
	ModelEventToolArgsDelta   ModelEventKind = "tool_arguments_delta"
	ModelEventToolCallEnd     ModelEventKind = "tool_call_end"
	ModelEventUsage           ModelEventKind = "usage"
	ModelEventResponseEnd     ModelEventKind = "response_end"
	ModelEventProviderWarning ModelEventKind = "provider_warning"
	ModelEventStreamError     ModelEventKind = "stream_error"
)

// ModelEvent is a tagged union for streaming model events.
type ModelEvent struct {
	Kind           ModelEventKind `json:"kind"`
	TextDelta      string         `json:"text_delta,omitempty"`
	ReasoningDelta string         `json:"reasoning_delta,omitempty"`
	// ReasoningSignature rides on the reasoning_end event: the provider proof
	// (Anthropic thinking signature / redacted payload) needed to replay the
	// reasoning block in later tool-use turns. ReasoningRedacted marks the
	// block as provider-redacted (no visible text, opaque payload only).
	ReasoningSignature string `json:"reasoning_signature,omitempty"`
	ReasoningRedacted  bool   `json:"reasoning_redacted,omitempty"`
	ToolIndex          int    `json:"tool_index,omitempty"`
	ToolID             string `json:"tool_id,omitempty"`
	ToolName           string `json:"tool_name,omitempty"`
	ToolArgs           string `json:"tool_args,omitempty"`
	InputTokens        int64  `json:"input_tokens,omitempty"`
	OutputTokens       int64  `json:"output_tokens,omitempty"`
	// CachedInputTokens reports provider-side prompt-cache hits (Anthropic
	// cache_read_input_tokens, OpenAI prompt_tokens_details.cached_tokens).
	// Observability only. Caveat: OpenAI's InputTokens already includes cached
	// tokens, Anthropic's does not — see Usage.CachedInputTokens.
	CachedInputTokens int64 `json:"cached_input_tokens,omitempty"`
	// ContextTokens is the provider-metered total footprint the request
	// occupied in the model's context window — the ground truth for
	// occupancy: Anthropic input_tokens + cache_read_input_tokens +
	// cache_creation_input_tokens, OpenAI prompt_tokens (already
	// cache-inclusive). Zero means the provider does not distinguish it
	// from InputTokens, which is then the occupancy measure.
	ContextTokens int64      `json:"context_tokens,omitempty"`
	StopReason    StopReason `json:"stop_reason,omitempty"`
	Error         string     `json:"error,omitempty"`
	// Retryable marks a stream_error as transient (truncated body, transport
	// drop): re-issuing the request is safe while nothing was delivered
	// yet. The agent loop only honors it for streams with no activity.
	Retryable bool `json:"retryable,omitempty"`
}

// ModelStream is a pull-based stream of model events.
type ModelStream interface {
	Recv() (ModelEvent, error)
	Close() error
}

// Model is the provider-agnostic model interface.
type Model interface {
	Stream(ctx context.Context, req ModelRequest) (ModelStream, error)
}

// --- Artifact interfaces (§8.4, §13.2) ---

// StagedArtifact incrementally captures a bounded immutable blob.
type StagedArtifact interface {
	io.Writer
	TotalBytes() int64
	StoredBytes() int64
	Truncated() bool
	Commit(context.Context) (ArtifactRef, error)
	Abort() error
}

// ArtifactStore starts independent staged artifact writes and reads back
// committed blobs by content-derived reference.
type ArtifactStore interface {
	Begin(context.Context) (StagedArtifact, error)
	// Read returns the full content of a committed artifact, verifying
	// its content hash against the reference. It is used by transport
	// adapters to serve artifact bytes to clients (e.g. inline images
	// stored during tool execution).
	Read(ctx context.Context, ref ArtifactRef) ([]byte, error)
}

// --- Session store types (§13.2) ---

// SessionSummary is a lightweight persisted session view for listing sessions.
type SessionSummary struct {
	ID        SessionID `json:"id"`
	Version   int64     `json:"version"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
	// WorkspaceID is the owning workspace (docs/WORKSPACE_DESIGN.md W1);
	// zero only for pre-v5 rows not yet backfilled.
	WorkspaceID WorkspaceID `json:"workspace_id"`
	// ParentSessionID is non-zero for delegated sub-agent sessions: the
	// delegation edge persisted in the child's run.created event
	// (docs/SUBAGENT_DESIGN.md §6.1), surfaced for hierarchical pickers.
	ParentSessionID SessionID `json:"parent_session_id"`
}

// SessionTranscript is the recovered message history of a session.
type SessionTranscript struct {
	SessionID         SessionID `json:"session_id"`
	Messages          []Message `json:"messages"`
	LastEventSequence int64     `json:"last_event_sequence"`
}

// SessionInspection is a consistent read-only view of persisted session data.
type SessionInspection struct {
	Session    SessionSummary    `json:"session"`
	Checkpoint *Checkpoint       `json:"checkpoint,omitempty"`
	Transcript SessionTranscript `json:"transcript"`
	Events     []Event           `json:"events"`
}

// --- SessionStore interface (§13.2) ---

// Checkpoint is a snapshot of a session's state for efficient recovery.
type Checkpoint struct {
	ID        CheckpointID `json:"id"`
	SessionID SessionID    `json:"session_id"`
	Sequence  int64        `json:"sequence"` // last event sequence covered
	State     RunState     `json:"state"`
	Messages  []Message    `json:"messages"`
	Plan      Plan         `json:"plan"`
	Usage     Usage        `json:"usage"`
	// Goal carries the cross-turn objective (nil when none is active); it
	// survives prompt boundaries through the checkpoint like Plan does.
	Goal      *Goal     `json:"goal,omitempty"`
	CreatedAt time.Time `json:"created_at"`
}

// SessionStore persists events and checkpoints.
type SessionStore interface {
	// CreateSession creates an empty session bound to workspaceID
	// (docs/WORKSPACE_DESIGN.md W1); workspaceID must be non-zero.
	CreateSession(ctx context.Context, sessionID SessionID, workspaceID WorkspaceID) error
	AppendEvents(ctx context.Context, sessionID SessionID, expectedVersion int64, events []Event) error
	AppendEventsAndCheckpoint(ctx context.Context, sessionID SessionID, expectedVersion int64, events []Event, checkpoint Checkpoint) error
	LoadEvents(ctx context.Context, sessionID SessionID, after int64) ([]Event, error)
	SaveCheckpoint(ctx context.Context, ckpt Checkpoint) error
	LoadLatestCheckpoint(ctx context.Context, sessionID SessionID) (Checkpoint, error)
	// RecordFileChange appends one file mutation to the session's change
	// ledger for checkpoint/rewind support. beforeContent may be nil (file
	// did not exist, or was not captured); beforeExisted distinguishes the
	// two cases.
	RecordFileChange(ctx context.Context, sessionID SessionID, path string, beforeExisted bool, beforeHash string, beforeContent []byte, afterHash string) error
	// InspectSession returns session metadata, its latest checkpoint, the
	// recovered transcript, and the complete event timeline from one
	// consistent read snapshot.
	InspectSession(ctx context.Context, sessionID SessionID) (SessionInspection, error)
}

// --- Approver interface (§12.2) ---

// Decision represents the outcome of an approval request.
type Decision string

const (
	DecisionAllow Decision = "allow"
	DecisionDeny  Decision = "deny"
	DecisionAsk   Decision = "ask"
)

// ApprovalRequest represents a request for user approval.
type ApprovalRequest struct {
	ID          EventID
	Call        PreparedCall
	Description string
}

// Approver handles permission decisions.
type Approver interface {
	RequestApproval(ctx context.Context, req ApprovalRequest) (Decision, error)
}
