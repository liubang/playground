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
	PartReasoning  PartKind = "reasoning"
	PartImage      PartKind = "image"
)

// MessageStatus identifies the lifecycle status of a logical message revision.
type MessageStatus string

const (
	MessageStatusDraft       MessageStatus = "draft"
	MessageStatusFinal       MessageStatus = "final"
	MessageStatusInterrupted MessageStatus = "interrupted"
)

// Metadata keys with cross-package meaning.
const (
	// MetadataPromptCache marks an ephemeral system-prompt message as safe
	// for provider-side prompt caching (value PromptCacheEphemeral). It is
	// set by the agent loop on the stable static part; providers that
	// support caching translate it (Anthropic cache_control), others
	// ignore it.
	MetadataPromptCache = "prompt_cache"
	// PromptCacheEphemeral requests short-lived (provider-default TTL)
	// caching of the marked block and everything before it.
	PromptCacheEphemeral = "ephemeral"
)

// ArtifactRef references a large content blob stored externally.
type ArtifactRef struct {
	ID   ArtifactID `json:"id"`
	Size int64      `json:"size"`
	// MediaType declares the blob's media type (e.g. image/png, text/plain)
	// so renderers can pick a presentation without fetching the bytes.
	// Optional: older records lack it; consumers should fall back to
	// server-side content sniffing.
	MediaType string `json:"media_type,omitempty"`
}

// Validate ensures the artifact reference is well-formed.
func (r ArtifactRef) Validate() error {
	if r.ID.IsZero() {
		return fmt.Errorf("artifact ID required")
	}
	if r.Size < 0 {
		return fmt.Errorf("artifact size must be non-negative")
	}
	return nil
}

// ReasoningContent carries a model's reasoning ("thinking") trace plus the
// provider proof required to replay it. Providers that authenticate thinking
// blocks (Anthropic's signature, or redacted-thinking data) reject a
// tool-use continuation whose preceding assistant message lacks the original
// signed blocks — so the reasoning that precedes tool calls is persisted in
// the transcript, not just rendered live.
type ReasoningContent struct {
	Text      string `json:"text,omitempty"`
	Signature string `json:"signature,omitempty"`
	// Redacted marks a provider-redacted thinking block: Text is empty and
	// Signature holds the opaque redacted payload to echo back.
	Redacted bool `json:"redacted,omitempty"`
}

// ImageContent carries one image for a vision-capable model: the media
// type (image/png, image/jpeg, image/gif, image/webp) and the base64
// payload. It appears in user messages (attached images) and inside tool
// results (view_image); providers map it onto their wire form (Anthropic
// image blocks, OpenAI image_url/input_image).
type ImageContent struct {
	MediaType string `json:"media_type"`
	Data      string `json:"data"`
}

// Validate ensures the image content is well-formed.
func (c ImageContent) Validate() error {
	switch c.MediaType {
	case "image/png", "image/jpeg", "image/gif", "image/webp":
	default:
		return fmt.Errorf("unsupported image media type %q", c.MediaType)
	}
	if c.Data == "" {
		return fmt.Errorf("image data required")
	}
	return nil
}

// ContentPart is a tagged union: exactly one field is populated based on Kind.
type ContentPart struct {
	PartIndex  int               `json:"part_index,omitempty"`
	Kind       PartKind          `json:"kind"`
	Text       string            `json:"text,omitempty"`
	ToolCall   *ToolCall         `json:"tool_call,omitempty"`
	ToolResult *ToolResult       `json:"tool_result,omitempty"`
	Artifact   *ArtifactRef      `json:"artifact,omitempty"`
	Reasoning  *ReasoningContent `json:"reasoning,omitempty"`
	Image      *ImageContent     `json:"image,omitempty"`
	// PresentOnly marks a display-only image artifact (present_image): the
	// UI renders it, but the egress materialization skips it — the model
	// never sees the image, so it costs no context and no tokens.
	PresentOnly bool `json:"present_only,omitempty"`
	// ModelOnly marks a model-bound image artifact (view_image): the egress
	// materialization resolves it for the model, but display channels must
	// not render the image itself — a compact text reference (path, media
	// type, dimensions from the accompanying header) is enough for the
	// user to audit what the model saw. Display is present_image's job.
	ModelOnly bool `json:"model_only,omitempty"`
}

// Validate ensures the ContentPart is well-formed.
func (p ContentPart) Validate() error {
	if p.PartIndex < 0 {
		return fmt.Errorf("part_index must be non-negative")
	}
	if p.PresentOnly && p.Kind != PartArtifact {
		return fmt.Errorf("present_only is only valid on artifact parts")
	}
	if p.ModelOnly && p.Kind != PartArtifact {
		return fmt.Errorf("model_only is only valid on artifact parts")
	}
	if p.PresentOnly && p.ModelOnly {
		return fmt.Errorf("present_only and model_only are mutually exclusive")
	}
	switch p.Kind {
	case PartText:
		if p.ToolCall != nil || p.ToolResult != nil || p.Artifact != nil || p.Reasoning != nil || p.Image != nil {
			return fmt.Errorf("text part must not have tool_call/tool_result/artifact/reasoning/image")
		}
	case PartReasoning:
		if p.Reasoning == nil {
			return fmt.Errorf("reasoning part must have Reasoning set")
		}
		if p.Reasoning.Redacted && p.Reasoning.Text != "" {
			return fmt.Errorf("redacted reasoning part must not carry text")
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
		if err := p.Artifact.Validate(); err != nil {
			return fmt.Errorf("invalid artifact reference: %w", err)
		}
	case PartImage:
		if p.Image == nil {
			return fmt.Errorf("image part must have Image set")
		}
		if err := p.Image.Validate(); err != nil {
			return fmt.Errorf("invalid image content: %w", err)
		}
	default:
		return fmt.Errorf("unknown part kind %q", p.Kind)
	}
	return nil
}

// MetadataCompactedArtifacts is the Message.Metadata key under which a
// compaction replacement message records the JSON-encoded []ArtifactRef
// list its payload depends on. Replacement messages (e.g. the archive
// marker) can only carry text parts, so the references they point at would
// otherwise be invisible to session persistence and garbage collection
// could reclaim them, leaving dead pointers in the transcript.
const MetadataCompactedArtifacts = "compacted_artifacts"

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

// ArtifactRefs returns every artifact reference carried by the message:
// direct part references and tool-result content references. Session
// persistence uses it to keep referenced artifacts alive for GC.
func (m Message) ArtifactRefs() []ArtifactRef {
	var refs []ArtifactRef
	for _, part := range m.Parts {
		if part.Artifact != nil {
			refs = append(refs, *part.Artifact)
		}
		if part.ToolResult != nil {
			for _, content := range part.ToolResult.Content {
				if content.Artifact != nil {
					refs = append(refs, *content.Artifact)
				}
			}
		}
	}
	return refs
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
