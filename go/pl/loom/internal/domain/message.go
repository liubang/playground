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

// Role identifies the sender of a Message.
type Role string

const (
	RoleSystem    Role = "system"
	RoleUser      Role = "user"
	RoleAssistant Role = "assistant"
)

// PartKind identifies the type of a ContentPart.
type PartKind string

const (
	PartText       PartKind = "text"
	PartToolCall   PartKind = "tool_call"
	PartToolResult PartKind = "tool_result"
	PartArtifact   PartKind = "artifact_ref"
)

// MessageStatus identifies the lifecycle status of a logical message revision.
type MessageStatus string

const (
	MessageStatusDraft       MessageStatus = "draft"
	MessageStatusFinal       MessageStatus = "final"
	MessageStatusInterrupted MessageStatus = "interrupted"
)

// ArtifactRef references a large content blob stored externally.
type ArtifactRef struct {
	ID   ArtifactID
	Size int64
}

// ContentPart is a tagged union: exactly one field is populated based on Kind.
type ContentPart struct {
	PartIndex  int          `json:"part_index,omitempty"`
	Kind       PartKind     `json:"kind"`
	Text       string       `json:"text,omitempty"`
	ToolCall   *ToolCall    `json:"tool_call,omitempty"`
	ToolResult *ToolResult  `json:"tool_result,omitempty"`
	Artifact   *ArtifactRef `json:"artifact,omitempty"`
}

// Validate ensures the ContentPart is well-formed.
func (p ContentPart) Validate() error {
	if p.PartIndex < 0 {
		return fmt.Errorf("part_index must be non-negative")
	}
	switch p.Kind {
	case PartText:
		if p.ToolCall != nil || p.ToolResult != nil || p.Artifact != nil {
			return fmt.Errorf("text part must not have tool_call/tool_result/artifact")
		}
	case PartToolCall:
		if p.ToolCall == nil {
			return fmt.Errorf("tool_call part must have ToolCall set")
		}
	case PartToolResult:
		if p.ToolResult == nil {
			return fmt.Errorf("tool_result part must have ToolResult set")
		}
	case PartArtifact:
		if p.Artifact == nil {
			return fmt.Errorf("artifact_ref part must have Artifact set")
		}
	default:
		return fmt.Errorf("unknown part kind %q", p.Kind)
	}
	return nil
}

// Message represents a single message in a conversation.
type Message struct {
	ID        MessageID         `json:"id"`
	Sequence  int64             `json:"sequence,omitempty"`
	Role      Role              `json:"role"`
	Status    MessageStatus     `json:"status,omitempty"`
	Revision  int               `json:"revision,omitempty"`
	Parts     []ContentPart     `json:"parts"`
	CreatedAt time.Time         `json:"created_at"`
	Metadata  map[string]string `json:"metadata,omitempty"`
}

// Validate checks the message is well-formed.
func (m Message) Validate() error {
	if m.ID.IsZero() {
		return fmt.Errorf("message ID required")
	}
	if m.Sequence < 0 {
		return fmt.Errorf("message sequence must be non-negative")
	}
	switch m.Role {
	case RoleSystem, RoleUser, RoleAssistant:
	default:
		return fmt.Errorf("invalid role %q", m.Role)
	}
	if m.Status != "" {
		switch m.Status {
		case MessageStatusDraft, MessageStatusFinal, MessageStatusInterrupted:
		default:
			return fmt.Errorf("invalid message status %q", m.Status)
		}
	}
	if m.Revision < 0 {
		return fmt.Errorf("message revision must be non-negative")
	}
	if len(m.Parts) == 0 {
		return fmt.Errorf("message must have at least one part")
	}
	allImplicitIndexes := len(m.Parts) > 0
	for i, p := range m.Parts {
		if err := p.Validate(); err != nil {
			return fmt.Errorf("part[%d]: %w", i, err)
		}
		if p.PartIndex != 0 {
			allImplicitIndexes = false
		}
	}
	if allImplicitIndexes {
		return nil
	}
	seenPartIndexes := make(map[int]struct{}, len(m.Parts))
	for i, p := range m.Parts {
		if _, ok := seenPartIndexes[p.PartIndex]; ok {
			return fmt.Errorf("part[%d]: duplicate part_index %d", i, p.PartIndex)
		}
		seenPartIndexes[p.PartIndex] = struct{}{}
	}
	return nil
}

// TextParts returns all text content from the message.
func (m Message) TextParts() []string {
	var out []string
	for _, p := range m.Parts {
		if p.Kind == PartText {
			out = append(out, p.Text)
		}
	}
	return out
}

// ToolCalls returns all tool calls from the message.
func (m Message) ToolCalls() []ToolCall {
	var out []ToolCall
	for _, p := range m.Parts {
		if p.Kind == PartToolCall && p.ToolCall != nil {
			out = append(out, *p.ToolCall)
		}
	}
	return out
}

// MarshalJSON implements custom serialization.
func (m Message) MarshalJSON() ([]byte, error) {
	type alias Message
	b, err := json.Marshal(alias(m))
	if err != nil {
		return nil, err
	}
	return b, nil
}
