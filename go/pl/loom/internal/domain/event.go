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
	"encoding/json"
	"fmt"
	"time"
)

// EventType identifies the kind of event.
type EventType string

const (
	EventSessionCreated         EventType = "session.created"
	EventRunCreated             EventType = "run.created"
	EventRunStateChanged        EventType = "run.state_changed"
	EventUserMessageAdded       EventType = "user.message_added"
	EventModelRequestStarted    EventType = "model.request_started"
	EventModelResponseCompleted EventType = "model.response_completed"
	EventModelRequestFailed     EventType = "model.request_failed"
	// EventModelRequestRetrying marks a start-stage failure the loop is
	// about to retry after a bounded wait (rate limits, transient 5xx or
	// transport errors). It is the audit trail for retried attempts;
	// EventModelRequestFailed remains reserved for attempts the loop
	// gives up on.
	EventModelRequestRetrying   EventType = "model.request_retrying"
	EventToolCallPrepared       EventType = "tool.call_prepared"
	EventPermissionRequested    EventType = "permission.requested"
	EventPermissionResolved     EventType = "permission.resolved"
	EventToolExecutionStarted   EventType = "tool.execution_started"
	EventToolExecutionCompleted EventType = "tool.execution_completed"
	EventToolResultAdded        EventType = "tool.result_added"
	EventFileChanged            EventType = "file.changed"
	EventPlanRevised            EventType = "plan.revised"
	EventContextCompacted       EventType = "context.compacted"
	EventGoalUpdated            EventType = "goal.updated"
	EventCheckpointCreated      EventType = "checkpoint.created"
	EventBudgetUpdated          EventType = "budget.updated"
	EventBudgetNotice           EventType = "budget.notice"
	EventBudgetWrapupStarted    EventType = "budget.wrapup_started"
	EventRunCompleted           EventType = "run.completed"
	EventRunFailed              EventType = "run.failed"
	EventRunCancelled           EventType = "run.cancelled"
	// EventRunInterrupted closes out a crash-orphaned run (deepseek-
	// harness's interrupted turn marker): written by recovery when the
	// log's last run never reached a terminal event. Pure audit — no
	// projection derives state from it — so it is informational/ignorable.
	EventRunInterrupted EventType = "run.interrupted"
)

// Event is an immutable fact in the event log. Events are the source of truth
// for all state; projections are derived from events.
type Event struct {
	ID        EventID         `json:"id"`
	Sequence  int64           `json:"sequence"`
	SessionID SessionID       `json:"session_id"`
	Type      EventType       `json:"type"`
	Timestamp time.Time       `json:"timestamp"`
	Payload   json.RawMessage `json:"payload,omitempty"`
	// Ignorable marks a purely informational record: dropping it cannot
	// change any projection (deepseek-harness SessionEvent.ignorable).
	// Readers skip UNKNOWN types only when this is set — a writer marks
	// its new informational types (EventType.Informational), so a log
	// written by a newer loom still replays under an older one; an
	// unknown NON-ignorable type stays a hard error, because skipping it
	// could silently corrupt the reconstructed surface.
	Ignorable bool `json:"ignorable,omitempty"`
}

// Informational reports whether the event type is a pure audit/observability
// record: no PROJECTION or GC path derives state from it, so a reader that
// does not know the type may safely skip it (when the event is marked
// Ignorable). Lifecycle, transcript, and directive types are NEVER
// informational. (Recovery scans the raw event log directly — it never
// goes through the projection, so audit types it consumes, like
// tool.execution_*, may still be informational.)
func (t EventType) Informational() bool {
	switch t {
	case EventModelRequestStarted, EventModelRequestFailed, EventModelRequestRetrying,
		EventModelRequestHeader, EventToolCallPrepared, EventToolExecutionStarted,
		EventToolExecutionCompleted, EventPermissionRequested, EventPermissionResolved,
		EventFileChanged, EventContextCompacted, EventCheckpointCreated,
		EventRunInterrupted:
		return true
	}
	return false
}

// MessageEventPayload is the canonical payload envelope for transcript events.
type MessageEventPayload struct {
	Message Message `json:"message"`
}

// RequestUsage is the per-request metered token usage recorded on
// EventModelResponseCompleted. Event-log consumers (trace visualization,
// session inspection) use it to attribute tokens to individual model
// calls — the alternative, differencing cumulative budget.updated
// snapshots, misfires because compaction calls and turn-boundary updates
// also move those counters without producing a transcript step.
type RequestUsage struct {
	InputTokens  int64 `json:"input_tokens"`
	OutputTokens int64 `json:"output_tokens"`
	// CachedInputTokens is the request's prompt-cache hit count
	// (observability only; see Usage.CachedInputTokens for the divergent
	// provider semantics).
	CachedInputTokens int64 `json:"cached_input_tokens,omitempty"`
	// ContextTokens is the provider-metered context-window footprint of
	// the request (cache-inclusive; see ModelEvent.ContextTokens).
	ContextTokens int64 `json:"context_tokens,omitempty"`
	// ReasoningTokens is the provider-metered reasoning/thinking share of
	// OutputTokens (0 when the provider does not split it out).
	ReasoningTokens int64 `json:"reasoning_tokens,omitempty"`
}

// ResponseCompletedPayload is the EventModelResponseCompleted payload: the
// canonical assistant message plus its per-request usage. The wire shape
// is a strict superset of MessageEventPayload, so existing readers that
// unmarshal only the message keep working unchanged; Usage is nil for
// events written before per-request usage was recorded.
type ResponseCompletedPayload struct {
	MessageEventPayload
	Usage *RequestUsage `json:"usage,omitempty"`
}

// BudgetNoticePayload is the EventBudgetNotice payload: one graduated
// budget reminder injected into the transcript (docs/CONTEXT_DESIGN.md
// §4.4.1). The message is the full transcript entry; the dimension fields
// make the notice auditable without parsing text.
type BudgetNoticePayload struct {
	Message   Message `json:"message"`
	Dimension string  `json:"dimension"`
	Level     int     `json:"level"`
	Usage     int64   `json:"usage"`
	Limit     int64   `json:"limit"`
}

// BudgetWrapupPayload is the EventBudgetWrapupStarted payload: the run
// entered its soft-landing wrap-up turn. RecoverRun uses the event to
// re-arm the wrap-up state after a crash.
type BudgetWrapupPayload struct {
	Dimension string `json:"dimension"`
	Usage     int64  `json:"usage"`
	Limit     int64  `json:"limit"`
}

// Validate checks the event is well-formed.
func (e Event) Validate() error {
	if e.ID.IsZero() {
		return fmt.Errorf("event ID required")
	}
	if e.Sequence <= 0 {
		return fmt.Errorf("event sequence must be positive")
	}
	if e.SessionID.IsZero() {
		return fmt.Errorf("session ID required")
	}
	switch e.Type {
	case EventSessionCreated, EventRunCreated, EventRunStateChanged,
		EventUserMessageAdded, EventModelRequestStarted, EventModelResponseCompleted,
		EventModelRequestFailed, EventModelRequestRetrying, EventToolCallPrepared, EventPermissionRequested,
		EventPermissionResolved, EventToolExecutionStarted, EventToolExecutionCompleted,
		EventToolResultAdded, EventFileChanged, EventPlanRevised, EventContextCompacted,
		EventGoalUpdated, EventCheckpointCreated, EventBudgetUpdated, EventBudgetNotice,
		EventBudgetWrapupStarted, EventRunCompleted, EventRunFailed, EventRunCancelled,
		EventRunInterrupted,
		EventContextMasked, EventContextArchived, EventContextSummarized,
		EventModelRequestHeader:
	default:
		// An unknown type is only acceptable when the writer marked it
		// informational: skipping it cannot corrupt any projection.
		if !e.Ignorable {
			return fmt.Errorf("unknown event type %q", e.Type)
		}
	}
	if len(e.Payload) > 0 && !json.Valid(e.Payload) {
		return fmt.Errorf("invalid event payload JSON")
	}
	return nil
}

// MarshalPayload serializes an event payload using canonical JSON encoding.
func MarshalPayload(payload any) (json.RawMessage, error) {
	if payload == nil {
		return nil, nil
	}
	data, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	return json.RawMessage(data), nil
}

// UnmarshalMessageEventPayload decodes a canonical transcript message payload.
func UnmarshalMessageEventPayload(payload json.RawMessage) (MessageEventPayload, error) {
	if len(payload) == 0 {
		return MessageEventPayload{}, fmt.Errorf("message payload required")
	}
	var decoded MessageEventPayload
	if err := json.Unmarshal(payload, &decoded); err != nil {
		return MessageEventPayload{}, err
	}
	if err := decoded.Message.Validate(); err != nil {
		return MessageEventPayload{}, fmt.Errorf("message payload: %w", err)
	}
	return decoded, nil
}

// Clock provides injectable time, enabling deterministic tests.
type Clock interface {
	Now() time.Time
	Since(time.Time) time.Duration
}

// RealClock uses the system clock.
type RealClock struct{}

func (RealClock) Now() time.Time                  { return time.Now().UTC() }
func (RealClock) Since(t time.Time) time.Duration { return time.Since(t) }

// FakeClock is a controllable clock for tests.
type FakeClock struct {
	now time.Time
}

func NewFakeClock(t time.Time) *FakeClock { return &FakeClock{now: t} }

func (c *FakeClock) Now() time.Time { return c.now }

func (c *FakeClock) Since(t time.Time) time.Duration { return c.now.Sub(t) }

func (c *FakeClock) Advance(d time.Duration) { c.now = c.now.Add(d) }
