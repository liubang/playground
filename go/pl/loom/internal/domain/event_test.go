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
	"testing"
	"time"
)

func TestEventValidation(t *testing.T) {
	sid := NewSessionID()

	tests := []struct {
		name    string
		event   Event
		wantErr bool
	}{
		{
			"valid event",
			Event{ID: NewEventID(), Sequence: 1, SessionID: sid, Type: EventSessionCreated, Timestamp: time.Now()},
			false,
		},
		{
			"empty ID",
			Event{Sequence: 1, SessionID: sid, Type: EventSessionCreated},
			true,
		},
		{
			"negative sequence",
			Event{ID: NewEventID(), Sequence: -1, SessionID: sid, Type: EventSessionCreated},
			true,
		},
		{
			"empty session ID",
			Event{ID: NewEventID(), Sequence: 1, Type: EventSessionCreated},
			true,
		},
		{
			"unknown type",
			Event{ID: NewEventID(), Sequence: 1, SessionID: sid, Type: "unknown"},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.event.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestAllEventTypes(t *testing.T) {
	types := []EventType{
		EventSessionCreated, EventRunCreated, EventRunStateChanged,
		EventUserMessageAdded, EventModelRequestStarted, EventModelResponseCompleted,
		EventModelRequestFailed, EventToolCallPrepared, EventPermissionRequested,
		EventPermissionResolved, EventToolExecutionStarted, EventToolExecutionCompleted,
		EventToolResultAdded, EventFileChanged, EventPlanRevised, EventContextCompacted,
		EventCheckpointCreated, EventBudgetUpdated, EventRunCompleted,
		EventRunFailed, EventRunCancelled,
	}

	sid := NewSessionID()
	for _, typ := range types {
		evt := Event{ID: NewEventID(), Sequence: 1, SessionID: sid, Type: typ, Timestamp: time.Now()}
		if err := evt.Validate(); err != nil {
			t.Errorf("event type %q failed validation: %v", typ, err)
		}
	}
}

func TestFakeClock(t *testing.T) {
	base := time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC)
	clock := NewFakeClock(base)

	if !clock.Now().Equal(base) {
		t.Fatalf("expected %v, got %v", base, clock.Now())
	}

	clock.Advance(5 * time.Second)
	if !clock.Now().Equal(base.Add(5 * time.Second)) {
		t.Fatalf("expected %v, got %v", base.Add(5*time.Second), clock.Now())
	}

	since := clock.Since(base)
	if since != 5*time.Second {
		t.Fatalf("expected 5s, got %v", since)
	}
}

func TestUnmarshalMessageEventPayload(t *testing.T) {
	msg := Message{
		ID:       NewMessageID(),
		Sequence: 1,
		Role:     RoleUser,
		Status:   MessageStatusFinal,
		Revision: 1,
		Parts:    []ContentPart{{PartIndex: 0, Kind: PartText, Text: "hello"}},
	}
	payload, err := MarshalPayload(MessageEventPayload{Message: msg})
	if err != nil {
		t.Fatalf("MarshalPayload() error = %v", err)
	}
	decoded, err := UnmarshalMessageEventPayload(payload)
	if err != nil {
		t.Fatalf("UnmarshalMessageEventPayload() error = %v", err)
	}
	if decoded.Message.ID != msg.ID {
		t.Fatalf("decoded message mismatch: %s vs %s", decoded.Message.ID, msg.ID)
	}
}

func TestEventValidateRejectsInvalidPayloadJSON(t *testing.T) {
	evt := Event{ID: NewEventID(), Sequence: 1, SessionID: NewSessionID(), Type: EventUserMessageAdded, Payload: json.RawMessage(`{`), Timestamp: time.Now()}
	if err := evt.Validate(); err == nil {
		t.Fatal("expected invalid payload error")
	}
}

func TestRealClock(t *testing.T) {
	clock := RealClock{}
	now := clock.Now()
	if now.IsZero() {
		t.Fatal("real clock should return non-zero time")
	}
}
