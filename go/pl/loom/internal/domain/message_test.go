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

func TestMessageValidation(t *testing.T) {
	tests := []struct {
		name    string
		msg     Message
		wantErr bool
	}{
		{
			"valid text message",
			Message{
				ID:        NewMessageID(),
				Role:      RoleUser,
				Parts:     []ContentPart{{Kind: PartText, Text: "hello"}},
				CreatedAt: time.Now(),
			},
			false,
		},
		{
			"empty ID",
			Message{
				Role:  RoleUser,
				Parts: []ContentPart{{Kind: PartText, Text: "hello"}},
			},
			true,
		},
		{
			"invalid role",
			Message{
				ID:    NewMessageID(),
				Role:  "unknown",
				Parts: []ContentPart{{Kind: PartText, Text: "hello"}},
			},
			true,
		},
		{
			"empty parts",
			Message{
				ID:   NewMessageID(),
				Role: RoleUser,
			},
			true,
		},
		{
			"invalid status",
			Message{
				ID:     NewMessageID(),
				Role:   RoleAssistant,
				Status: "bad",
				Parts:  []ContentPart{{Kind: PartText, Text: "hello"}},
			},
			true,
		},
		{
			"duplicate explicit part indexes",
			Message{
				ID:    NewMessageID(),
				Role:  RoleAssistant,
				Parts: []ContentPart{{PartIndex: 1, Kind: PartText, Text: "hello"}, {PartIndex: 1, Kind: PartText, Text: "world"}},
			},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.msg.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestContentPartValidation(t *testing.T) {
	tests := []struct {
		name    string
		part    ContentPart
		wantErr bool
	}{
		{
			"valid text part",
			ContentPart{Kind: PartText, Text: "hello"},
			false,
		},
		{
			"text with tool_call set",
			ContentPart{Kind: PartText, Text: "hello", ToolCall: &ToolCall{}},
			true,
		},
		{
			"tool_call without ToolCall",
			ContentPart{Kind: PartToolCall},
			true,
		},
		{
			"tool_result without ToolResult",
			ContentPart{Kind: PartToolResult},
			true,
		},
		{
			"artifact_ref without Artifact",
			ContentPart{Kind: PartArtifact},
			true,
		},
		{
			"artifact_ref with empty ID",
			ContentPart{Kind: PartArtifact, Artifact: &ArtifactRef{}},
			true,
		},
		{
			"artifact_ref with negative size",
			ContentPart{Kind: PartArtifact, Artifact: &ArtifactRef{ID: NewArtifactID(), Size: -1}},
			true,
		},
		{
			"valid artifact_ref",
			ContentPart{Kind: PartArtifact, Artifact: &ArtifactRef{ID: NewArtifactID(), Size: 10}},
			false,
		},
		{
			"unknown kind",
			ContentPart{Kind: "unknown"},
			true,
		},
		{
			"valid tool_call",
			ContentPart{Kind: PartToolCall, ToolCall: &ToolCall{ID: NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{}`)}},
			false,
		},
		{
			"valid tool_result",
			ContentPart{Kind: PartToolResult, ToolResult: &ToolResult{CallID: NewToolCallID(), Status: ToolStatusSuccess}},
			false,
		},
		{
			"negative part index",
			ContentPart{PartIndex: -1, Kind: PartText, Text: "hello"},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.part.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestMessageTextParts(t *testing.T) {
	msg := Message{
		ID:   NewMessageID(),
		Role: RoleAssistant,
		Parts: []ContentPart{
			{Kind: PartText, Text: "hello"},
			{Kind: PartText, Text: "world"},
			{Kind: PartToolCall, ToolCall: &ToolCall{ID: NewToolCallID(), Name: "echo"}},
		},
	}

	parts := msg.TextParts()
	if len(parts) != 2 {
		t.Fatalf("expected 2 text parts, got %d", len(parts))
	}
	if parts[0] != "hello" || parts[1] != "world" {
		t.Fatalf("unexpected text parts: %v", parts)
	}
}

func TestMessageToolCalls(t *testing.T) {
	tc1 := ToolCall{ID: NewToolCallID(), Name: "echo"}
	tc2 := ToolCall{ID: NewToolCallID(), Name: "read_file"}
	msg := Message{
		ID:   NewMessageID(),
		Role: RoleAssistant,
		Parts: []ContentPart{
			{Kind: PartText, Text: "let me check"},
			{Kind: PartToolCall, ToolCall: &tc1},
			{Kind: PartToolCall, ToolCall: &tc2},
		},
	}

	calls := msg.ToolCalls()
	if len(calls) != 2 {
		t.Fatalf("expected 2 tool calls, got %d", len(calls))
	}
	if calls[0].Name != "echo" || calls[1].Name != "read_file" {
		t.Fatalf("unexpected tool calls: %v", calls)
	}
}

func TestMessageJSONRoundTrip(t *testing.T) {
	msg := Message{
		ID:       NewMessageID(),
		Sequence: 7,
		Role:     RoleUser,
		Status:   MessageStatusFinal,
		Revision: 2,
		Parts: []ContentPart{
			{PartIndex: 0, Kind: PartText, Text: "hello"},
			{PartIndex: 1, Kind: PartToolCall, ToolCall: &ToolCall{ID: NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"hi"}`)}},
		},
		CreatedAt: time.Now().Truncate(time.Millisecond),
	}

	data, err := json.Marshal(msg)
	if err != nil {
		t.Fatalf("Marshal error: %v", err)
	}

	var decoded Message
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Unmarshal error: %v", err)
	}

	if decoded.ID != msg.ID {
		t.Errorf("ID mismatch: %s vs %s", decoded.ID, msg.ID)
	}
	if decoded.Role != msg.Role {
		t.Errorf("Role mismatch: %s vs %s", decoded.Role, msg.Role)
	}
	if len(decoded.Parts) != 2 {
		t.Fatalf("expected 2 parts, got %d", len(decoded.Parts))
	}
}
