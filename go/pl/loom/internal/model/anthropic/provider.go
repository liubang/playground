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
// Created: 2026/07/27

// Package anthropic implements domain.Model against the Anthropic Messages
// API (POST /v1/messages with SSE streaming), including extended thinking:
// reasoning blocks are streamed as canonical reasoning events and replayed
// from the transcript with their signatures so tool-use continuations stay
// valid.
package anthropic

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/model/sse"
	"github.com/liubang/playground/go/pl/loom/internal/model/stream"
)

const (
	defaultBaseURL      = "https://api.anthropic.com"
	defaultVersion      = "2023-06-01"
	defaultMaxTokens    = 8192
	minThinkingBudget   = 1024
	thinkingTypeEnabled = "enabled"
)

// AuthType selects how the credential is presented on the wire. The
// official API authenticates with x-api-key; Anthropic-protocol gateways
// (and Claude Code OAuth setups) commonly expect a Bearer token instead.
type AuthType string

const (
	// AuthTypeAPIKey sends the credential as x-api-key (the default).
	AuthTypeAPIKey AuthType = "x-api-key"
	// AuthTypeBearer sends the credential as Authorization: Bearer.
	AuthTypeBearer AuthType = "bearer"
)

// Config controls how the Anthropic provider connects and retries.
type Config struct {
	BaseURL string
	APIKey  string
	// AuthType selects the credential header; empty selects x-api-key.
	AuthType AuthType
	// Version is the anthropic-version header; empty selects the pinned
	// default the provider was written against.
	Version        string
	HTTPClient     *http.Client
	MaxRetries     int
	InitialBackoff time.Duration
	MaxBackoff     time.Duration
}

// Provider implements domain.Model against the Anthropic Messages API.
type Provider struct {
	endpointURL string
	apiKey      string
	authType    AuthType
	version     string
	client      *httpc.Client
}

// New creates a new Anthropic provider.
func New(cfg Config) (*Provider, error) {
	baseURL := strings.TrimRight(strings.TrimSpace(cfg.BaseURL), "/")
	if baseURL == "" {
		baseURL = defaultBaseURL
	}
	endpointURL := baseURL
	switch {
	case strings.HasSuffix(endpointURL, "/v1/messages"):
		// Already complete.
	case strings.HasSuffix(endpointURL, "/v1"):
		// OpenAI-style base: avoid producing /v1/v1/messages.
		endpointURL += "/messages"
	default:
		endpointURL += "/v1/messages"
	}

	version := strings.TrimSpace(cfg.Version)
	if version == "" {
		version = defaultVersion
	}

	authType := cfg.AuthType
	switch authType {
	case "":
		authType = AuthTypeAPIKey
	case AuthTypeAPIKey, AuthTypeBearer:
	default:
		return nil, fmt.Errorf("anthropic provider: unsupported auth type %q", cfg.AuthType)
	}

	client, err := httpc.New(httpc.Config{
		HTTPClient:     cfg.HTTPClient,
		MaxRetries:     cfg.MaxRetries,
		InitialBackoff: cfg.InitialBackoff,
		MaxBackoff:     cfg.MaxBackoff,
	})
	if err != nil {
		return nil, fmt.Errorf("anthropic provider: %w", err)
	}

	return &Provider{
		endpointURL: endpointURL,
		apiKey:      cfg.APIKey,
		authType:    authType,
		version:     version,
		client:      client,
	}, nil
}

// Stream starts a streaming Messages request.
func (p *Provider) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	body, err := marshalRequest(req)
	if err != nil {
		return nil, err
	}

	headers := http.Header{
		"Content-Type":      {"application/json"},
		"Accept":            {"text/event-stream"},
		"Cache-Control":     {"no-cache"},
		"anthropic-version": {p.version},
	}
	if p.apiKey != "" {
		if p.authType == AuthTypeBearer {
			headers.Set("Authorization", "Bearer "+p.apiKey)
		} else {
			headers.Set("x-api-key", p.apiKey)
		}
	}

	resp, err := p.client.Post(ctx, p.endpointURL, body, headers)
	if err != nil {
		// Classify the failure (rate limit / permission / transient) so the
		// agent loop can wait out retryable ones instead of killing the run.
		return nil, httpc.ToDomainError("anthropic provider", err)
	}

	if err := httpc.RequireEventStream(resp); err != nil {
		_ = resp.Body.Close()
		return nil, fmt.Errorf("anthropic provider: %w", err)
	}

	return stream.Start(ctx, resp.Body, func(streamCtx context.Context, body io.Reader, emit stream.Emitter) {
		pump(streamCtx, body, emit)
	}), nil
}

// --- request assembly ---

func marshalRequest(req domain.ModelRequest) ([]byte, error) {
	if req.ModelName == "" {
		return nil, fmt.Errorf("anthropic provider: model name required")
	}

	maxTokens := req.MaxTokens
	if maxTokens <= 0 {
		maxTokens = defaultMaxTokens
	}

	thinkingBudget, err := thinkingBudgetFor(req.Reasoning, maxTokens)
	if err != nil {
		return nil, err
	}

	systemBlocks, messages, err := toAnthropicMessages(req.Messages)
	if err != nil {
		return nil, err
	}
	if len(messages) == 0 {
		return nil, fmt.Errorf("anthropic provider: at least one non-system message required")
	}

	payload := map[string]any{
		"model":      req.ModelName,
		"max_tokens": maxTokens,
		"messages":   messages,
		"stream":     true,
	}
	switch {
	case len(systemBlocks) == 1 && systemBlocks[0]["cache_control"] == nil:
		// Single unmarked block keeps the plain-string form (back-compat).
		payload["system"] = systemBlocks[0]["text"]
	case len(systemBlocks) > 0:
		payload["system"] = systemBlocks
	}
	// Extended thinking pins temperature to 1; any caller-tuned value would
	// be rejected by the API, so it is only sent when thinking is off.
	if req.Temperature != 0 && thinkingBudget == 0 {
		payload["temperature"] = req.Temperature
	}
	if thinkingBudget > 0 {
		payload["thinking"] = map[string]any{
			"type":          thinkingTypeEnabled,
			"budget_tokens": thinkingBudget,
		}
	}
	if len(req.Tools) > 0 {
		tools, err := toAnthropicTools(req.Tools)
		if err != nil {
			return nil, err
		}
		payload["tools"] = tools
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return nil, fmt.Errorf("anthropic provider: marshal request: %w", err)
	}
	return body, nil
}

// thinkingBudgetFor resolves the wire budget from the vendor-neutral spec.
// An explicit BudgetTokens always wins; otherwise the effort level derives a
// fraction of the output budget. The API requires 1024 <= budget < max_tokens.
func thinkingBudgetFor(spec domain.ReasoningSpec, maxTokens int64) (int64, error) {
	if !spec.Enabled() {
		return 0, nil
	}
	budget := spec.BudgetTokens
	if budget <= 0 {
		switch spec.Effort {
		case domain.ReasoningEffortLow:
			budget = maxTokens / 8
		case domain.ReasoningEffortMedium:
			budget = maxTokens / 3
		default: // ReasoningEffortHigh
			budget = maxTokens * 2 / 3
		}
	}
	if budget < minThinkingBudget {
		budget = minThinkingBudget
	}
	if budget >= maxTokens {
		return 0, fmt.Errorf("anthropic provider: reasoning budget %d must be below max_tokens %d", budget, maxTokens)
	}
	return budget, nil
}

// toAnthropicMessages converts the canonical transcript. Leading system
// messages are hoisted into the top-level system parameter as text blocks
// (a system message anywhere after the first non-system message is
// downgraded to user text, mirroring the openai provider's GLM-class
// workaround); a block whose message carries domain.MetadataPromptCache gets
// cache_control so Anthropic caches the stable prompt prefix; tool results
// become tool_result blocks inside a user message; signed reasoning blocks
// are replayed as thinking blocks so tool-use continuations stay valid;
// consecutive same-role messages are merged because the API requires role
// alternation.
func toAnthropicMessages(in []domain.Message) ([]map[string]any, []map[string]any, error) {
	var systemBlocks []map[string]any
	sink := &messageSink{}

	leading := true
	for _, msg := range in {
		if leading && msg.Role == domain.RoleSystem {
			text, err := messageText(msg)
			if err != nil {
				return nil, nil, err
			}
			if text != "" {
				block := map[string]any{"type": "text", "text": text}
				if msg.Metadata[domain.MetadataPromptCache] == domain.PromptCacheEphemeral {
					block["cache_control"] = map[string]any{"type": "ephemeral"}
				}
				systemBlocks = append(systemBlocks, block)
			}
			continue
		}
		leading = false

		switch msg.Role {
		case domain.RoleSystem, domain.RoleUser:
			// A non-leading system message is downgraded to user text: the
			// Messages API accepts system only as the top-level parameter.
			blocks, err := userMessageBlocks(msg)
			if err != nil {
				return nil, nil, err
			}
			for _, block := range blocks {
				sink.append(string(domain.RoleUser), block)
			}
		case domain.RoleAssistant:
			if err := appendAssistant(msg, sink); err != nil {
				return nil, nil, err
			}
		default:
			return nil, nil, fmt.Errorf("anthropic provider: unsupported role %q", msg.Role)
		}
	}
	return systemBlocks, sink.out, nil
}

// messageSink accumulates wire messages, merging consecutive same-role
// messages because the API requires role alternation.
type messageSink struct {
	out []map[string]any
}

func (s *messageSink) append(role string, block map[string]any) {
	if n := len(s.out); n > 0 && s.out[n-1]["role"] == role {
		s.out[n-1]["content"] = append(s.out[n-1]["content"].([]map[string]any), block)
		return
	}
	s.out = append(s.out, map[string]any{
		"role":    role,
		"content": []map[string]any{block},
	})
}

// appendToolResult appends a tool_result block while preserving the API's
// ordering invariant: tool_result blocks must immediately follow the
// assistant message carrying their tool_use. Merging into the previous
// message is only valid when it is a user message that already consists
// solely of tool_result blocks (parallel results); merging into a
// text-carrying user message would put the tool_result ahead of its
// tool_use on the wire — a guaranteed API 400 (REVIEW M30).
func (s *messageSink) appendToolResult(block map[string]any) error {
	if n := len(s.out); n > 0 && s.out[n-1]["role"] == string(domain.RoleUser) {
		for _, existing := range s.out[n-1]["content"].([]map[string]any) {
			if existing["type"] != "tool_result" {
				return fmt.Errorf("anthropic provider: tool result %q would merge into a user text message (tool_result must follow its tool_use)", block["tool_use_id"])
			}
		}
		s.out[n-1]["content"] = append(s.out[n-1]["content"].([]map[string]any), block)
		return nil
	}
	s.out = append(s.out, map[string]any{
		"role":    string(domain.RoleUser),
		"content": []map[string]any{block},
	})
	return nil
}

// appendAssistant converts one assistant message: text and signed reasoning
// become assistant blocks in order; a tool result flushes into a user
// message carrying tool_result blocks (consecutive results share one user
// message, as the API requires for parallel tool use).
func appendAssistant(msg domain.Message, sink *messageSink) error {
	for _, part := range msg.Parts {
		switch part.Kind {
		case domain.PartText:
			if part.Text != "" {
				sink.append(string(domain.RoleAssistant), map[string]any{"type": "text", "text": part.Text})
			}
		case domain.PartReasoning:
			if part.Reasoning == nil {
				return fmt.Errorf("anthropic provider: reasoning part missing payload")
			}
			block, ok := thinkingBlock(*part.Reasoning)
			if ok {
				sink.append(string(domain.RoleAssistant), block)
			}
		case domain.PartToolCall:
			if part.ToolCall == nil {
				return fmt.Errorf("anthropic provider: assistant tool call part missing payload")
			}
			var input any
			if len(part.ToolCall.Arguments) > 0 {
				if err := json.Unmarshal(part.ToolCall.Arguments, &input); err != nil {
					return fmt.Errorf("anthropic provider: decode tool arguments for %q: %w", part.ToolCall.Name, err)
				}
			} else {
				input = map[string]any{}
			}
			sink.append(string(domain.RoleAssistant), map[string]any{
				"type":  "tool_use",
				"id":    part.ToolCall.ID.String(),
				"name":  part.ToolCall.Name,
				"input": input,
			})
		case domain.PartToolResult:
			if part.ToolResult == nil {
				return fmt.Errorf("anthropic provider: tool result part missing payload")
			}
			if err := sink.appendToolResult(toolResultBlock(*part.ToolResult)); err != nil {
				return err
			}
		default:
			return fmt.Errorf("anthropic provider: unsupported assistant part kind %q", part.Kind)
		}
	}
	return nil
}

// thinkingBlock renders a persisted reasoning part for replay. Blocks
// without a signature cannot be authenticated by the API and are dropped —
// an unsigned thinking block in a tool-use transcript is a hard 400.
func thinkingBlock(r domain.ReasoningContent) (map[string]any, bool) {
	if r.Signature == "" {
		return nil, false
	}
	if r.Redacted {
		return map[string]any{"type": "redacted_thinking", "data": r.Signature}, true
	}
	return map[string]any{"type": "thinking", "thinking": r.Text, "signature": r.Signature}, true
}

func toolResultBlock(result domain.ToolResult) map[string]any {
	return map[string]any{
		"type":        "tool_result",
		"tool_use_id": result.CallID.String(),
		"content":     toolResultContent(result),
		"is_error":    result.Status != domain.ToolStatusSuccess || result.Error != nil,
	}
}

// toolResultBlocks renders a tool result containing image parts as a block
// array (the tool_result content field accepts either a string or blocks):
// text coalesces into text blocks and images into image blocks, in order.
func toolResultBlocks(result domain.ToolResult) []map[string]any {
	var blocks []map[string]any
	var text strings.Builder
	flushText := func() {
		if text.Len() > 0 {
			blocks = append(blocks, map[string]any{"type": "text", "text": text.String()})
			text.Reset()
		}
	}
	for _, part := range result.Content {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartImage:
			if part.Image != nil {
				flushText()
				blocks = append(blocks, imageBlock(*part.Image))
			}
		case domain.PartArtifact:
			// See toolResultContent: refs stay in the canonical result only.
		}
	}
	flushText()
	return blocks
}

func messageText(msg domain.Message) (string, error) {
	var b strings.Builder
	for _, part := range msg.Parts {
		if part.Kind != domain.PartText {
			return "", fmt.Errorf("anthropic provider: role %q only supports text parts", msg.Role)
		}
		b.WriteString(part.Text)
	}
	return b.String(), nil
}

// userMessageBlocks converts a user (or downgraded system) message into
// content blocks: text parts coalesce into one text block, image parts
// become base64 image blocks in order. Pure-text messages produce at most
// one block, matching the previous wire shape.
func userMessageBlocks(msg domain.Message) ([]map[string]any, error) {
	var blocks []map[string]any
	var text strings.Builder
	flushText := func() {
		if text.Len() > 0 {
			blocks = append(blocks, map[string]any{"type": "text", "text": text.String()})
			text.Reset()
		}
	}
	for _, part := range msg.Parts {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartImage:
			if part.Image == nil {
				return nil, fmt.Errorf("anthropic provider: image part missing payload")
			}
			flushText()
			blocks = append(blocks, imageBlock(*part.Image))
		default:
			return nil, fmt.Errorf("anthropic provider: role %q only supports text and image parts", msg.Role)
		}
	}
	flushText()
	return blocks, nil
}

// imageBlock renders one image part as a Messages API base64 image block.
func imageBlock(img domain.ImageContent) map[string]any {
	return map[string]any{
		"type": "image",
		"source": map[string]any{
			"type":       "base64",
			"media_type": img.MediaType,
			"data":       img.Data,
		},
	}
}

func toAnthropicTools(defs []domain.ToolDefinition) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(defs))
	for _, def := range defs {
		var schema any
		if len(def.InputSchema) == 0 {
			schema = map[string]any{"type": "object"}
		} else if err := json.Unmarshal(def.InputSchema, &schema); err != nil {
			return nil, fmt.Errorf("anthropic provider: decode tool schema for %q: %w", def.Name, err)
		}
		out = append(out, map[string]any{
			"name":         def.Name,
			"description":  def.Description,
			"input_schema": schema,
		})
	}
	return out, nil
}

// toolResultContent mirrors the openai provider: plain text when the result
// is purely textual, otherwise a structured JSON envelope — the model reads
// either form.
func toolResultContent(result domain.ToolResult) any {
	if result.Error != nil {
		payload, err := json.Marshal(map[string]any{
			"status": result.Status,
			"error":  result.Error,
		})
		if err == nil {
			return string(payload)
		}
		return result.Error.Message
	}

	textAndArtifactRefs := true
	hasImages := false
	var text strings.Builder
	for _, part := range result.Content {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartArtifact:
			// Artifact references are persisted in the canonical ToolResult;
			// tools include model-safe reference metadata in their bounded
			// text payload, so refs are not duplicated here.
		case domain.PartImage:
			hasImages = true
		default:
			textAndArtifactRefs = false
		}
	}
	if hasImages {
		// Images must ride as content blocks; the API accepts either a
		// string or a block array in tool_result content.
		return toolResultBlocks(result)
	}
	if textAndArtifactRefs && text.Len() > 0 {
		return text.String()
	}

	payload, err := json.Marshal(map[string]any{
		"status":  result.Status,
		"content": result.Content,
		"meta":    result.Metadata,
	})
	if err == nil {
		return string(payload)
	}
	return string(result.Status)
}

// --- stream mapping ---

// blockState tracks one open content block until its stop event.
type blockState struct {
	kind      string // "text" | "thinking" | "tool_use" | "redacted_thinking"
	signature string
	toolID    string
	toolName  string
}

type streamState struct {
	responseStarted bool
	stop            domain.StopReason
	inputTokens     int64
	outputTokens    int64
	// cachedInputTokens tracks cache_read_input_tokens (prompt-cache hits;
	// observability only, already included in inputTokens).
	cachedInputTokens int64
	blocks            map[int]*blockState
}

// wire event payloads (subset of the Messages streaming schema).

type messageStartEvent struct {
	Message struct {
		Usage *usagePayload `json:"usage,omitempty"`
	} `json:"message"`
}

type contentBlockStartEvent struct {
	Index        int `json:"index"`
	ContentBlock struct {
		Type      string `json:"type"`
		ID        string `json:"id,omitempty"`
		Name      string `json:"name,omitempty"`
		Signature string `json:"signature,omitempty"`
		Data      string `json:"data,omitempty"`
	} `json:"content_block"`
}

type contentBlockDeltaEvent struct {
	Index int `json:"index"`
	Delta struct {
		Type        string `json:"type"`
		Text        string `json:"text,omitempty"`
		Thinking    string `json:"thinking,omitempty"`
		PartialJSON string `json:"partial_json,omitempty"`
		Signature   string `json:"signature,omitempty"`
	} `json:"delta"`
}

type contentBlockStopEvent struct {
	Index int `json:"index"`
}

type messageDeltaEvent struct {
	Delta struct {
		StopReason string `json:"stop_reason,omitempty"`
	} `json:"delta"`
	Usage *usagePayload `json:"usage,omitempty"`
}

type errorEvent struct {
	Error struct {
		Type    string `json:"type,omitempty"`
		Message string `json:"message,omitempty"`
	} `json:"error"`
}

type usagePayload struct {
	InputTokens  int64 `json:"input_tokens,omitempty"`
	OutputTokens int64 `json:"output_tokens,omitempty"`
	// Prompt-cache accounting, present when the request used cache_control.
	// Read hits are what we track for observability; creation writes are
	// reported for completeness.
	CacheReadInputTokens     int64 `json:"cache_read_input_tokens,omitempty"`
	CacheCreationInputTokens int64 `json:"cache_creation_input_tokens,omitempty"`
}

// pump converts one Messages SSE body into canonical events; it runs inside
// the shared stream runner (model/stream).
func pump(ctx context.Context, body io.Reader, emit stream.Emitter) {
	parser := sse.NewParser(body)
	state := &streamState{blocks: make(map[int]*blockState)}

	for {
		evt, err := parser.Next()
		if err != nil {
			finishReadError(ctx, state, err, emit)
			return
		}

		switch evt.Name {
		case "message_start":
			if err := state.onMessageStart(evt.Data, emit); err != nil {
				finishWithError(state, err, emit)
				return
			}
		case "content_block_start":
			if err := state.onBlockStart(evt.Data, emit); err != nil {
				finishWithError(state, err, emit)
				return
			}
		case "content_block_delta":
			if err := state.onBlockDelta(evt.Data, emit); err != nil {
				finishWithError(state, err, emit)
				return
			}
		case "content_block_stop":
			if err := state.onBlockStop(evt.Data, emit); err != nil {
				finishWithError(state, err, emit)
				return
			}
		case "message_delta":
			if err := state.onMessageDelta(evt.Data); err != nil {
				finishWithError(state, err, emit)
				return
			}
		case "message_stop":
			state.finish(emit)
			return
		case "ping":
			// Keep-alive; no canonical content.
		case "error":
			state.onStreamError(evt.Data, emit)
			return
		default:
			// Forward compatibility: ignore event types we do not know yet
			// (Anthropic adds stream event kinds over time) instead of
			// killing the whole stream.
		}
	}
}

func (s *streamState) requireStarted(event string) error {
	if !s.responseStarted {
		return fmt.Errorf("anthropic provider: received %q before message_start", event)
	}
	return nil
}

func (s *streamState) onMessageStart(data string, emit stream.Emitter) error {
	if s.responseStarted {
		return fmt.Errorf("anthropic provider: duplicate message_start")
	}
	var evt messageStartEvent
	if err := json.Unmarshal([]byte(data), &evt); err != nil {
		return fmt.Errorf("anthropic provider: malformed message_start: %w", err)
	}
	if evt.Message.Usage != nil {
		s.inputTokens = evt.Message.Usage.InputTokens
		s.outputTokens = evt.Message.Usage.OutputTokens
		s.cachedInputTokens = evt.Message.Usage.CacheReadInputTokens
	}
	s.responseStarted = true
	emit(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	return nil
}

func (s *streamState) onBlockStart(data string, emit stream.Emitter) error {
	if err := s.requireStarted("content_block_start"); err != nil {
		return err
	}
	var evt contentBlockStartEvent
	if err := json.Unmarshal([]byte(data), &evt); err != nil {
		return fmt.Errorf("anthropic provider: malformed content_block_start: %w", err)
	}
	if _, exists := s.blocks[evt.Index]; exists {
		return fmt.Errorf("anthropic provider: duplicate content block index %d", evt.Index)
	}
	block := &blockState{kind: evt.ContentBlock.Type}
	s.blocks[evt.Index] = block

	switch evt.ContentBlock.Type {
	case "text":
		emit(domain.ModelEvent{Kind: domain.ModelEventTextStart})
	case "thinking":
		block.signature = evt.ContentBlock.Signature
		emit(domain.ModelEvent{Kind: domain.ModelEventReasoningStart})
	case "redacted_thinking":
		// A redacted block carries no deltas: open and seal it immediately.
		emit(domain.ModelEvent{Kind: domain.ModelEventReasoningStart})
		emit(domain.ModelEvent{
			Kind:               domain.ModelEventReasoningEnd,
			ReasoningSignature: evt.ContentBlock.Data,
			ReasoningRedacted:  true,
		})
	case "tool_use":
		if evt.ContentBlock.ID == "" || evt.ContentBlock.Name == "" {
			return fmt.Errorf("anthropic provider: tool_use block at index %d missing id or name", evt.Index)
		}
		block.toolID = evt.ContentBlock.ID
		block.toolName = evt.ContentBlock.Name
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventToolCallStart,
			ToolIndex: evt.Index,
			ToolID:    block.toolID,
			ToolName:  block.toolName,
		})
	default:
		return fmt.Errorf("anthropic provider: unsupported content block type %q", evt.ContentBlock.Type)
	}
	return nil
}

func (s *streamState) onBlockDelta(data string, emit stream.Emitter) error {
	if err := s.requireStarted("content_block_delta"); err != nil {
		return err
	}
	var evt contentBlockDeltaEvent
	if err := json.Unmarshal([]byte(data), &evt); err != nil {
		return fmt.Errorf("anthropic provider: malformed content_block_delta: %w", err)
	}
	block, ok := s.blocks[evt.Index]
	if !ok {
		return fmt.Errorf("anthropic provider: delta for unknown content block index %d", evt.Index)
	}

	switch evt.Delta.Type {
	case "text_delta":
		if block.kind != "text" {
			return fmt.Errorf("anthropic provider: text_delta against %q block at index %d", block.kind, evt.Index)
		}
		emit(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: evt.Delta.Text})
	case "thinking_delta":
		if block.kind != "thinking" {
			return fmt.Errorf("anthropic provider: thinking_delta against %q block at index %d", block.kind, evt.Index)
		}
		emit(domain.ModelEvent{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: evt.Delta.Thinking})
	case "signature_delta":
		if block.kind != "thinking" {
			return fmt.Errorf("anthropic provider: signature_delta against %q block at index %d", block.kind, evt.Index)
		}
		block.signature += evt.Delta.Signature
	case "input_json_delta":
		if block.kind != "tool_use" {
			return fmt.Errorf("anthropic provider: input_json_delta against %q block at index %d", block.kind, evt.Index)
		}
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventToolArgsDelta,
			ToolIndex: evt.Index,
			ToolID:    block.toolID,
			ToolName:  block.toolName,
			ToolArgs:  evt.Delta.PartialJSON,
		})
	default:
		return fmt.Errorf("anthropic provider: unsupported delta type %q", evt.Delta.Type)
	}
	return nil
}

func (s *streamState) onBlockStop(data string, emit stream.Emitter) error {
	if err := s.requireStarted("content_block_stop"); err != nil {
		return err
	}
	var evt contentBlockStopEvent
	if err := json.Unmarshal([]byte(data), &evt); err != nil {
		return fmt.Errorf("anthropic provider: malformed content_block_stop: %w", err)
	}
	block, ok := s.blocks[evt.Index]
	if !ok {
		return fmt.Errorf("anthropic provider: stop for unknown content block index %d", evt.Index)
	}
	delete(s.blocks, evt.Index)

	switch block.kind {
	case "text":
		emit(domain.ModelEvent{Kind: domain.ModelEventTextEnd})
	case "thinking":
		emit(domain.ModelEvent{
			Kind:               domain.ModelEventReasoningEnd,
			ReasoningSignature: block.signature,
		})
	case "redacted_thinking":
		// Already sealed at block start.
	case "tool_use":
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventToolCallEnd,
			ToolIndex: evt.Index,
			ToolID:    block.toolID,
			ToolName:  block.toolName,
		})
	}
	return nil
}

func (s *streamState) onMessageDelta(data string) error {
	if err := s.requireStarted("message_delta"); err != nil {
		return err
	}
	var evt messageDeltaEvent
	if err := json.Unmarshal([]byte(data), &evt); err != nil {
		return fmt.Errorf("anthropic provider: malformed message_delta: %w", err)
	}
	if evt.Delta.StopReason != "" {
		s.stop = mapStopReason(evt.Delta.StopReason)
	}
	if evt.Usage != nil {
		s.outputTokens = evt.Usage.OutputTokens
		if evt.Usage.InputTokens > 0 {
			s.inputTokens = evt.Usage.InputTokens
		}
		if evt.Usage.CacheReadInputTokens > 0 {
			s.cachedInputTokens = evt.Usage.CacheReadInputTokens
		}
	}
	return nil
}

// finish seals the stream at message_stop: any unclosed blocks are closed
// defensively before usage and the terminal event are emitted.
func (s *streamState) finish(emit stream.Emitter) {
	s.closeOpenBlocks(emit)
	if s.inputTokens > 0 || s.outputTokens > 0 {
		emit(domain.ModelEvent{
			Kind:              domain.ModelEventUsage,
			InputTokens:       s.inputTokens,
			OutputTokens:      s.outputTokens,
			CachedInputTokens: s.cachedInputTokens,
		})
	}
	stop := s.stop
	if stop == "" {
		stop = domain.StopUnknown
	}
	emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: stop})
}

// onStreamError handles the protocol-level error event: the stream is
// finished with a provider error after surfacing the message.
func (s *streamState) onStreamError(data string, emit stream.Emitter) {
	message := "anthropic provider: stream error"
	var evt errorEvent
	if err := json.Unmarshal([]byte(data), &evt); err == nil && evt.Error.Message != "" {
		message = fmt.Sprintf("anthropic provider: %s: %s", evt.Error.Type, evt.Error.Message)
	}
	s.closeOpenBlocks(emit)
	emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: message})
	emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopProviderError})
}

// closeOpenBlocks ends any blocks left open by a truncated stream so the
// aggregator never sees a dangling start.
func (s *streamState) closeOpenBlocks(emit stream.Emitter) {
	for index, block := range s.blocks {
		switch block.kind {
		case "text":
			emit(domain.ModelEvent{Kind: domain.ModelEventTextEnd})
		case "thinking":
			emit(domain.ModelEvent{
				Kind:               domain.ModelEventReasoningEnd,
				ReasoningSignature: block.signature,
			})
		case "tool_use":
			emit(domain.ModelEvent{
				Kind:      domain.ModelEventToolCallEnd,
				ToolIndex: index,
				ToolID:    block.toolID,
				ToolName:  block.toolName,
			})
		}
		delete(s.blocks, index)
	}
}

func finishReadError(ctx context.Context, state *streamState, err error, emit stream.Emitter) {
	switch {
	case ctx.Err() != nil:
		state.closeOpenBlocks(emit)
		emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: ctx.Err().Error()})
		emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopCancelled})
	case errors.Is(err, io.EOF):
		finishStreamError(state, fmt.Errorf("anthropic provider: stream closed before message_stop"), true, emit)
	default:
		finishStreamError(state, fmt.Errorf("anthropic provider: stream read failed: %w", err), true, emit)
	}
}

func finishWithError(state *streamState, err error, emit stream.Emitter) {
	finishStreamError(state, err, false, emit)
}

// finishStreamError ends the stream on a failure; retryable marks the
// transient read failures (truncated body, transport drop) so the agent
// loop can re-issue the request while nothing was delivered yet.
func finishStreamError(state *streamState, err error, retryable bool, emit stream.Emitter) {
	state.closeOpenBlocks(emit)
	emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: err.Error(), Retryable: retryable})
	emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopProviderError})
}

func mapStopReason(reason string) domain.StopReason {
	switch reason {
	case "end_turn", "stop_sequence":
		return domain.StopEndTurn
	case "tool_use":
		return domain.StopToolUse
	case "max_tokens":
		return domain.StopMaxOutput
	case "refusal":
		return domain.StopContentFilter
	case "pause_turn":
		// pause_turn ends the model's turn exactly like end_turn from the
		// agent's perspective; loom registers no server-side tools that
		// would resume it.
		return domain.StopEndTurn
	default:
		return domain.StopUnknown
	}
}
