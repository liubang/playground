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

package session

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestReplayBuildsCanonicalTranscript(t *testing.T) {
	sessionID := domain.NewSessionID()
	toolCallID := domain.NewToolCallID()
	userMsgID := domain.NewMessageID()
	assistantMsgID := domain.NewMessageID()
	toolMsgID := domain.NewMessageID()
	baseTime := time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC)

	userMsg := domain.Message{
		ID:        userMsgID,
		Sequence:  1,
		Role:      domain.RoleUser,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		CreatedAt: baseTime,
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartText, Text: "question"}},
	}
	assistantDraft := domain.Message{
		ID:        assistantMsgID,
		Sequence:  2,
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusDraft,
		Revision:  1,
		CreatedAt: baseTime.Add(time.Second),
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartText, Text: "looking"}},
	}
	assistantFinal := domain.Message{
		ID:        assistantMsgID,
		Sequence:  2,
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusFinal,
		Revision:  2,
		CreatedAt: baseTime.Add(2 * time.Second),
		Parts: []domain.ContentPart{
			{PartIndex: 0, Kind: domain.PartText, Text: "done"},
			{PartIndex: 1, Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: toolCallID, Name: "read_file", Arguments: json.RawMessage(`{"path":"a.txt"}`)}},
		},
	}
	toolMsg := domain.Message{
		ID:        toolMsgID,
		Sequence:  3,
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		CreatedAt: baseTime.Add(3 * time.Second),
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{CallID: toolCallID, Status: domain.ToolStatusSuccess, Content: []domain.ContentPart{{Kind: domain.PartText, Text: "file-body"}}}}},
	}

	events := []domain.Event{
		messageEvent(t, 4, sessionID, domain.EventModelResponseCompleted, assistantFinal),
		{ID: domain.NewEventID(), Sequence: 1, SessionID: sessionID, Type: domain.EventSessionCreated, Timestamp: baseTime},
		messageEvent(t, 2, sessionID, domain.EventUserMessageAdded, userMsg),
		messageEvent(t, 3, sessionID, domain.EventModelResponseCompleted, assistantDraft),
		messageEvent(t, 5, sessionID, domain.EventToolResultAdded, toolMsg),
	}

	transcript, err := Replay(events)
	if err != nil {
		t.Fatalf("Replay() error = %v", err)
	}
	if err := transcript.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	if len(transcript.Messages) != 3 {
		t.Fatalf("expected 3 messages, got %d", len(transcript.Messages))
	}
	if transcript.Messages[0].ID != userMsgID {
		t.Fatalf("unexpected first message: %s", transcript.Messages[0].ID)
	}
	if transcript.Messages[1].Revision != 2 || transcript.Messages[1].Status != domain.MessageStatusFinal {
		t.Fatalf("assistant revision/status mismatch: %+v", transcript.Messages[1])
	}
	if len(transcript.Messages[1].Parts) != 2 || transcript.Messages[1].Parts[1].PartIndex != 1 {
		t.Fatalf("assistant parts mismatch: %+v", transcript.Messages[1].Parts)
	}
	if transcript.Messages[2].Sequence != 3 {
		t.Fatalf("unexpected tool message sequence: %d", transcript.Messages[2].Sequence)
	}

	page, err := transcript.Page(1, 1)
	if err != nil {
		t.Fatalf("Page() error = %v", err)
	}
	if len(page.Messages) != 1 || page.Messages[0].ID != assistantMsgID {
		t.Fatalf("unexpected page: %+v", page)
	}
	if !page.HasMore || page.NextAfter != 2 {
		t.Fatalf("unexpected page metadata: %+v", page)
	}

	json1, err := transcript.CanonicalJSON()
	if err != nil {
		t.Fatalf("CanonicalJSON() error = %v", err)
	}
	json2, err := transcript.CanonicalJSON()
	if err != nil {
		t.Fatalf("CanonicalJSON() second error = %v", err)
	}
	if string(json1) != string(json2) {
		t.Fatalf("canonical transcript json mismatch:\n%s\n%s", json1, json2)
	}
}

func TestReplayFromCheckpointMatchesReplay(t *testing.T) {
	sessionID := domain.NewSessionID()
	baseTime := time.Date(2025, 1, 2, 0, 0, 0, 0, time.UTC)
	msg1 := domain.Message{
		ID:        domain.NewMessageID(),
		Sequence:  1,
		Role:      domain.RoleUser,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		CreatedAt: baseTime,
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartText, Text: "hi"}},
	}
	msg2 := domain.Message{
		ID:        domain.NewMessageID(),
		Sequence:  2,
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		CreatedAt: baseTime.Add(time.Second),
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartText, Text: "hello"}},
	}
	msg3 := domain.Message{
		ID:        domain.NewMessageID(),
		Sequence:  3,
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusInterrupted,
		Revision:  1,
		CreatedAt: baseTime.Add(2 * time.Second),
		Parts:     []domain.ContentPart{{PartIndex: 0, Kind: domain.PartText, Text: "partial"}},
	}
	allEvents := []domain.Event{
		{ID: domain.NewEventID(), Sequence: 1, SessionID: sessionID, Type: domain.EventSessionCreated, Timestamp: baseTime},
		messageEvent(t, 2, sessionID, domain.EventUserMessageAdded, msg1),
		messageEvent(t, 3, sessionID, domain.EventModelResponseCompleted, msg2),
		messageEvent(t, 4, sessionID, domain.EventModelResponseCompleted, msg3),
	}
	ckpt := domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: sessionID,
		Sequence:  3,
		Messages:  []domain.Message{msg1, msg2},
		CreatedAt: baseTime.Add(1500 * time.Millisecond),
	}

	fromReplay, err := Replay(allEvents)
	if err != nil {
		t.Fatalf("Replay() error = %v", err)
	}
	fromCheckpoint, err := ReplayFromCheckpoint(ckpt, allEvents)
	if err != nil {
		t.Fatalf("ReplayFromCheckpoint() error = %v", err)
	}
	json1, _ := fromReplay.CanonicalJSON()
	json2, _ := fromCheckpoint.CanonicalJSON()
	if string(json1) != string(json2) {
		t.Fatalf("checkpoint replay mismatch:\n%s\n%s", json1, json2)
	}
}

func TestReplayAcceptsToolCompletionAuditPayload(t *testing.T) {
	sessionID := domain.NewSessionID()
	payload := json.RawMessage(`{"call_id":"tc_test","status":"success","started_at":"2025-01-01T00:00:00Z","finished_at":"2025-01-01T00:00:01Z"}`)
	transcript, err := Replay([]domain.Event{{
		ID:        domain.NewEventID(),
		Sequence:  1,
		SessionID: sessionID,
		Type:      domain.EventToolExecutionCompleted,
		Timestamp: time.Now().UTC(),
		Payload:   payload,
	}})
	if err != nil {
		t.Fatalf("Replay() error = %v", err)
	}
	if len(transcript.Messages) != 0 {
		t.Fatalf("audit event unexpectedly added transcript message: %+v", transcript.Messages)
	}
}

func TestReplayRejectsInvalidPayload(t *testing.T) {
	_, err := Replay([]domain.Event{{
		ID:        domain.NewEventID(),
		Sequence:  1,
		SessionID: domain.NewSessionID(),
		Type:      domain.EventUserMessageAdded,
		Timestamp: time.Now().UTC(),
		Payload:   json.RawMessage(`{"broken":true}`),
	}})
	if err == nil {
		t.Fatal("expected payload validation error")
	}
}

func messageEvent(t *testing.T, sequence int64, sessionID domain.SessionID, typ domain.EventType, msg domain.Message) domain.Event {
	t.Helper()
	payload, err := domain.MarshalPayload(domain.MessageEventPayload{Message: msg})
	if err != nil {
		t.Fatalf("MarshalPayload() error = %v", err)
	}
	return domain.Event{
		ID:        domain.NewEventID(),
		Sequence:  sequence,
		SessionID: sessionID,
		Type:      typ,
		Timestamp: time.Now().UTC(),
		Payload:   payload,
	}
}
