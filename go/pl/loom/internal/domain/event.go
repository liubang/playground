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
	EventToolCallPrepared       EventType = "tool.call_prepared"
	EventPermissionRequested    EventType = "permission.requested"
	EventPermissionResolved     EventType = "permission.resolved"
	EventToolExecutionStarted   EventType = "tool.execution_started"
	EventToolExecutionCompleted EventType = "tool.execution_completed"
	EventFileChanged            EventType = "file.changed"
	EventPlanRevised            EventType = "plan.revised"
	EventContextCompacted       EventType = "context.compacted"
	EventCheckpointCreated      EventType = "checkpoint.created"
	EventBudgetUpdated          EventType = "budget.updated"
	EventRunCompleted           EventType = "run.completed"
	EventRunFailed              EventType = "run.failed"
	EventRunCancelled           EventType = "run.cancelled"
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
}

// MessageEventPayload is the canonical payload envelope for transcript events.
type MessageEventPayload struct {
	Message Message `json:"message"`
}

// Validate checks the event is well-formed.
func (e Event) Validate() error {
	if e.ID.IsZero() {
		return fmt.Errorf("event ID required")
	}
	if e.Sequence < 0 {
		return fmt.Errorf("event sequence must be non-negative")
	}
	if e.SessionID.IsZero() {
		return fmt.Errorf("session ID required")
	}
	switch e.Type {
	case EventSessionCreated, EventRunCreated, EventRunStateChanged,
		EventUserMessageAdded, EventModelRequestStarted, EventModelResponseCompleted,
		EventModelRequestFailed, EventToolCallPrepared, EventPermissionRequested,
		EventPermissionResolved, EventToolExecutionStarted, EventToolExecutionCompleted,
		EventFileChanged, EventPlanRevised, EventContextCompacted,
		EventCheckpointCreated, EventBudgetUpdated, EventRunCompleted,
		EventRunFailed, EventRunCancelled:
	default:
		return fmt.Errorf("unknown event type %q", e.Type)
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
