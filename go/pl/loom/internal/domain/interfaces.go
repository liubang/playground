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
	"time"
)

// --- Tool interface (§9.1) ---

// Tool is the core tool abstraction. Prepare must be side-effect-free.
type Tool interface {
	Definition() ToolDefinition
	Prepare(ctx context.Context, call ToolCall) (PreparedCall, error)
	Execute(ctx context.Context, prepared PreparedCall) ToolResult
}

// --- Model interface (§7) ---

// ModelRequest is the unified input to a model provider.
type ModelRequest struct {
	ID              EventID
	ModelName       string
	Messages        []Message
	Tools           []ToolDefinition
	MaxTokens       int64
	Temperature     float64
	ContextManifest ContextManifest
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
	Kind         ModelEventKind `json:"kind"`
	TextDelta    string         `json:"text_delta,omitempty"`
	ToolIndex    int            `json:"tool_index,omitempty"`
	ToolID       string         `json:"tool_id,omitempty"`
	ToolName     string         `json:"tool_name,omitempty"`
	ToolArgs     string         `json:"tool_args,omitempty"`
	InputTokens  int64          `json:"input_tokens,omitempty"`
	OutputTokens int64          `json:"output_tokens,omitempty"`
	StopReason   StopReason     `json:"stop_reason,omitempty"`
	Error        string         `json:"error,omitempty"`
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
	CreatedAt time.Time    `json:"created_at"`
}

// SessionStore persists events and checkpoints.
type SessionStore interface {
	CreateSession(ctx context.Context, sessionID SessionID) error
	AppendEvents(ctx context.Context, sessionID SessionID, expectedVersion int64, events []Event) error
	AppendEventsAndCheckpoint(ctx context.Context, sessionID SessionID, expectedVersion int64, events []Event, checkpoint Checkpoint) error
	LoadEvents(ctx context.Context, sessionID SessionID, after int64) ([]Event, error)
	SaveCheckpoint(ctx context.Context, ckpt Checkpoint) error
	LoadLatestCheckpoint(ctx context.Context, sessionID SessionID) (Checkpoint, error)
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
