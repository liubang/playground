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

package openai

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/model/sse"
	"github.com/liubang/playground/go/pl/loom/internal/model/stream"
	"github.com/liubang/playground/go/pl/loom/internal/model/wireutil"
)

const defaultBaseURL = "https://api.openai.com/v1"

// WireAPI selects which OpenAI-compatible wire protocol the provider speaks.
type WireAPI string

const (
	WireAPIChatCompletions WireAPI = "chat_completions"
	WireAPIResponses       WireAPI = "responses"
)

// Config controls how the OpenAI-compatible provider connects and retries.
type Config struct {
	BaseURL        string
	APIKey         string
	HTTPClient     *http.Client
	WireAPI        WireAPI
	MaxRetries     int
	InitialBackoff time.Duration
	MaxBackoff     time.Duration
}

// Provider implements domain.Model against OpenAI-compatible streaming APIs.
type Provider struct {
	endpointURL string
	apiKey      string
	client      *httpc.Client
	wireAPI     WireAPI
}

// New creates a new OpenAI-compatible provider.
func New(cfg Config) (*Provider, error) {
	baseURL := strings.TrimSpace(cfg.BaseURL)
	if baseURL == "" {
		baseURL = defaultBaseURL
	}

	wireAPI, err := normalizeWireAPI(cfg.WireAPI)
	if err != nil {
		return nil, err
	}

	endpointURL := providerEndpointURL(baseURL, wireAPI)
	if endpointURL == "" {
		return nil, fmt.Errorf("openai provider: invalid base URL")
	}

	client, err := wireutil.NewHTTPClient("openai", httpc.Config{
		HTTPClient:     cfg.HTTPClient,
		MaxRetries:     cfg.MaxRetries,
		InitialBackoff: cfg.InitialBackoff,
		MaxBackoff:     cfg.MaxBackoff,
	})
	if err != nil {
		return nil, err
	}

	return &Provider{
		endpointURL: endpointURL,
		apiKey:      cfg.APIKey,
		client:      client,
		wireAPI:     wireAPI,
	}, nil
}

// Stream starts a streaming request against the configured wire API.
func (p *Provider) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	body, err := marshalRequest(req, p.wireAPI)
	if err != nil {
		return nil, err
	}

	headers := wireutil.StreamHeaders()
	if p.apiKey != "" {
		headers.Set("Authorization", "Bearer "+p.apiKey)
	}

	return wireutil.StartStream(ctx, p.client, p.endpointURL, body, headers, "openai", p.pump)
}

func normalizeWireAPI(wireAPI WireAPI) (WireAPI, error) {
	switch strings.TrimSpace(string(wireAPI)) {
	case "":
		return WireAPIChatCompletions, nil
	case string(WireAPIChatCompletions):
		return WireAPIChatCompletions, nil
	case string(WireAPIResponses):
		return WireAPIResponses, nil
	default:
		return "", fmt.Errorf("openai provider: unsupported wire API %q", wireAPI)
	}
}

func providerEndpointURL(baseURL string, wireAPI WireAPI) string {
	switch wireAPI {
	case WireAPIResponses:
		return responsesURL(baseURL)
	case WireAPIChatCompletions:
		return chatCompletionsURL(baseURL)
	default:
		return ""
	}
}

func marshalRequest(req domain.ModelRequest, wireAPI WireAPI) ([]byte, error) {
	if req.ModelName == "" {
		return nil, fmt.Errorf("openai provider: model name required")
	}

	switch wireAPI {
	case WireAPIResponses:
		return marshalResponsesRequest(req)
	case WireAPIChatCompletions:
		return marshalChatCompletionsRequest(req)
	default:
		return nil, fmt.Errorf("openai provider: unsupported wire API %q", wireAPI)
	}
}

func marshalChatCompletionsRequest(req domain.ModelRequest) ([]byte, error) {
	messages, err := toOpenAIMessages(req.Messages)
	if err != nil {
		return nil, err
	}

	payload := map[string]any{
		"model":          req.ModelName,
		"messages":       messages,
		"stream":         true,
		"stream_options": map[string]bool{"include_usage": true},
	}
	if req.MaxTokens > 0 {
		payload["max_tokens"] = req.MaxTokens
	}
	if req.Temperature != 0 {
		payload["temperature"] = req.Temperature
	}
	// Chat Completions has a reasoning_effort knob but no token-budget form;
	// BudgetTokens is silently out of scope for this wire API.
	if effort := reasoningEffortParam(req.Reasoning); effort != "" {
		payload["reasoning_effort"] = effort
	}
	if len(req.Tools) > 0 {
		tools, err := toOpenAITools(req.Tools)
		if err != nil {
			return nil, err
		}
		payload["tools"] = tools
	}
	if rf := req.ResponseFormat; rf != nil {
		payload["response_format"] = map[string]any{
			"type": "json_schema",
			"json_schema": map[string]any{
				"name":   rf.Name,
				"schema": rf.Schema,
				"strict": rf.Strict,
			},
		}
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return nil, fmt.Errorf("openai provider: marshal request: %w", err)
	}
	return body, nil
}

func marshalResponsesRequest(req domain.ModelRequest) ([]byte, error) {
	input, err := toResponsesInput(req.Messages)
	if err != nil {
		return nil, err
	}

	payload := map[string]any{
		"model":  req.ModelName,
		"input":  input,
		"stream": true,
	}
	if req.MaxTokens > 0 {
		payload["max_output_tokens"] = req.MaxTokens
	}
	if req.Temperature != 0 {
		payload["temperature"] = req.Temperature
	}
	if effort := reasoningEffortParam(req.Reasoning); effort != "" {
		payload["reasoning"] = map[string]any{"effort": effort}
	}
	if len(req.Tools) > 0 {
		tools, err := toResponsesTools(req.Tools)
		if err != nil {
			return nil, err
		}
		payload["tools"] = tools
	}
	if rf := req.ResponseFormat; rf != nil {
		payload["text"] = map[string]any{
			"format": map[string]any{
				"type":   "json_schema",
				"name":   rf.Name,
				"schema": rf.Schema,
				"strict": rf.Strict,
			},
		}
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return nil, fmt.Errorf("openai provider: marshal request: %w", err)
	}
	return body, nil
}

func toOpenAITools(defs []domain.ToolDefinition) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(defs))
	for _, def := range defs {
		parameters, err := wireutil.ToolInputSchema("openai", def)
		if err != nil {
			return nil, err
		}

		out = append(out, map[string]any{
			"type": "function",
			"function": map[string]any{
				"name":        def.Name,
				"description": def.Description,
				"parameters":  parameters,
			},
		})
	}
	return out, nil
}

func toResponsesTools(defs []domain.ToolDefinition) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(defs))
	for _, def := range defs {
		parameters, err := wireutil.ToolInputSchema("openai", def)
		if err != nil {
			return nil, err
		}

		out = append(out, map[string]any{
			"type":        "function",
			"name":        def.Name,
			"description": def.Description,
			"parameters":  parameters,
		})
	}
	return out, nil
}

// reasoningEffortParam maps the vendor-neutral spec onto the OpenAI effort
// parameter. "off" has no portable representation across compatible vendors
// (some reason unconditionally), so it falls back to omitting the knob.
func reasoningEffortParam(spec domain.ReasoningSpec) string {
	switch spec.Effort {
	case domain.ReasoningEffortLow, domain.ReasoningEffortMedium, domain.ReasoningEffortHigh:
		return string(spec.Effort)
	default:
		return ""
	}
}

// apiRole normalizes a message role for OpenAI-compatible vendors: a system
// message anywhere but the head is downgraded to user. GLM-class vendors
// accept a single leading system message and reject later ones — observed in
// production as GLM error 1214 "messages 格式有误" after a context-compaction
// marker (role=system) entered the transcript behind the runtime-injected
// system prompt.
func apiRole(msg domain.Message, index int) domain.Role {
	if index > 0 && msg.Role == domain.RoleSystem {
		return domain.RoleUser
	}
	return msg.Role
}

func toOpenAIMessages(messages []domain.Message) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(messages))
	for i, msg := range messages {
		switch role := apiRole(msg, i); role {
		case domain.RoleSystem, domain.RoleUser:
			content, err := chatUserContent(msg)
			if err != nil {
				return nil, err
			}
			out = append(out, map[string]any{
				"role":    string(role),
				"content": content,
			})
		case domain.RoleAssistant:
			assistantParts := newAssistantMessageParts()
			flushAssistant := func() {
				if !assistantParts.empty() {
					out = append(out, assistantParts.toMap())
					assistantParts = newAssistantMessageParts()
				}
			}

			for _, part := range msg.Parts {
				switch part.Kind {
				case domain.PartText:
					assistantParts.addText(part.Text)
				case domain.PartToolCall:
					if part.ToolCall == nil {
						return nil, fmt.Errorf("openai provider: assistant tool call part missing payload")
					}
					assistantParts.addToolCall(*part.ToolCall)
				case domain.PartToolResult:
					if part.ToolResult == nil {
						return nil, fmt.Errorf("openai provider: tool result part missing payload")
					}
					flushAssistant()
					out = append(out, toolResultMessage(*part.ToolResult))
					if extra := toolResultImageMessage(*part.ToolResult); extra != nil {
						out = append(out, extra)
					}
				case domain.PartReasoning:
					// Reasoning traces are transcript-local for OpenAI-compatible
					// vendors: their APIs neither require nor accept prior
					// reasoning content being sent back.
				default:
					return nil, fmt.Errorf("openai provider: unsupported assistant part kind %q", part.Kind)
				}
			}
			flushAssistant()
		default:
			return nil, fmt.Errorf("openai provider: unsupported role %q", msg.Role)
		}
	}
	return out, nil
}

// chatUserContent renders a user (or downgraded system) message for the
// chat wire: a plain string when purely textual (the common case), or a
// content-part array mixing text and image_url parts when the message
// carries images.
func chatUserContent(msg domain.Message) (any, error) {
	hasImages := false
	for _, part := range msg.Parts {
		if part.Kind == domain.PartImage {
			hasImages = true
			break
		}
	}
	if !hasImages {
		return wireutil.MessageText("openai", msg)
	}

	parts := make([]map[string]any, 0, len(msg.Parts))
	var text strings.Builder
	flushText := func() {
		if text.Len() > 0 {
			parts = append(parts, map[string]any{"type": "text", "text": text.String()})
			text.Reset()
		}
	}
	for _, part := range msg.Parts {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartImage:
			if part.Image == nil {
				return nil, fmt.Errorf("openai provider: image part missing payload")
			}
			flushText()
			parts = append(parts, imageURLPart(*part.Image))
		default:
			return nil, fmt.Errorf("openai provider: role %q only supports text and image parts", msg.Role)
		}
	}
	flushText()
	return parts, nil
}

// imageURLPart renders one image as a chat-completions image_url part using
// a data URL.
func imageURLPart(img domain.ImageContent) map[string]any {
	return map[string]any{
		"type": "image_url",
		"image_url": map[string]any{
			"url": "data:" + img.MediaType + ";base64," + img.Data,
		},
	}
}

// toolResultImageMessage handles images inside a tool result on the chat
// wire: a tool message's content cannot carry images, so the image is
// re-homed into a following user message (the same workaround codex uses
// for chat-completions vendors), with a pointer back to the tool call.
func toolResultImageMessage(result domain.ToolResult) map[string]any {
	var images []domain.ImageContent
	for _, part := range result.Content {
		if part.Kind == domain.PartImage && part.Image != nil {
			images = append(images, *part.Image)
		}
	}
	if len(images) == 0 {
		return nil
	}
	parts := make([]map[string]any, 0, len(images)+1)
	parts = append(parts, map[string]any{
		"type": "text",
		"text": "[image output from the tool call above]",
	})
	for _, img := range images {
		parts = append(parts, imageURLPart(img))
	}
	return map[string]any{
		"role":    string(domain.RoleUser),
		"content": parts,
	}
}

func toResponsesInput(messages []domain.Message) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(messages))
	for i, msg := range messages {
		switch role := apiRole(msg, i); role {
		case domain.RoleSystem, domain.RoleUser:
			hasImages := false
			for _, part := range msg.Parts {
				if part.Kind == domain.PartImage {
					hasImages = true
					break
				}
			}
			if hasImages {
				item, err := responseUserImageItem(role, msg)
				if err != nil {
					return nil, err
				}
				out = append(out, item)
				break
			}
			text, err := wireutil.MessageText("openai", msg)
			if err != nil {
				return nil, err
			}
			out = append(out, responseMessageItem(role, "input_text", text))
		case domain.RoleAssistant:
			var text strings.Builder
			flushText := func() {
				if text.Len() == 0 {
					return
				}
				out = append(out, responseMessageItem(msg.Role, "output_text", text.String()))
				text.Reset()
			}

			for _, part := range msg.Parts {
				switch part.Kind {
				case domain.PartText:
					text.WriteString(part.Text)
				case domain.PartToolCall:
					if part.ToolCall == nil {
						return nil, fmt.Errorf("openai provider: assistant tool call part missing payload")
					}
					flushText()
					out = append(out, responseFunctionCallItem(*part.ToolCall))
				case domain.PartToolResult:
					if part.ToolResult == nil {
						return nil, fmt.Errorf("openai provider: tool result part missing payload")
					}
					flushText()
					out = append(out, responseFunctionCallOutputItem(*part.ToolResult))
				case domain.PartReasoning:
					// See toOpenAIMessages: reasoning is not replayed upstream.
				default:
					return nil, fmt.Errorf("openai provider: unsupported assistant part kind %q", part.Kind)
				}
			}
			flushText()
		default:
			return nil, fmt.Errorf("openai provider: unsupported role %q", msg.Role)
		}
	}
	return out, nil
}

func responseMessageItem(role domain.Role, contentType, text string) map[string]any {
	return map[string]any{
		"type": "message",
		"role": string(role),
		"content": []map[string]any{{
			"type": contentType,
			"text": text,
		}},
	}
}

// responseUserImageItem renders a user message carrying images for the
// responses wire: input_text and input_image parts in order.
func responseUserImageItem(role domain.Role, msg domain.Message) (map[string]any, error) {
	parts := make([]map[string]any, 0, len(msg.Parts))
	var text strings.Builder
	flushText := func() {
		if text.Len() > 0 {
			parts = append(parts, map[string]any{"type": "input_text", "text": text.String()})
			text.Reset()
		}
	}
	for _, part := range msg.Parts {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartImage:
			if part.Image == nil {
				return nil, fmt.Errorf("openai provider: image part missing payload")
			}
			flushText()
			parts = append(parts, map[string]any{
				"type":      "input_image",
				"image_url": "data:" + part.Image.MediaType + ";base64," + part.Image.Data,
			})
		default:
			return nil, fmt.Errorf("openai provider: role %q only supports text and image parts", msg.Role)
		}
	}
	flushText()
	return map[string]any{
		"type":    "message",
		"role":    string(role),
		"content": parts,
	}, nil
}

func responseFunctionCallItem(call domain.ToolCall) map[string]any {
	return map[string]any{
		"type":      "function_call",
		"call_id":   call.ID.String(),
		"name":      call.Name,
		"arguments": string(call.Arguments),
	}
}

func responseFunctionCallOutputItem(result domain.ToolResult) map[string]any {
	return map[string]any{
		"type":    "function_call_output",
		"call_id": result.CallID.String(),
		"output":  responseFunctionCallOutput(result),
	}
}

// responseFunctionCallOutput renders a tool result for the responses wire:
// a plain string when textual, or a content-part array mixing input_text
// and input_image parts when the result carries images.
func responseFunctionCallOutput(result domain.ToolResult) any {
	hasImages := false
	for _, part := range result.Content {
		if part.Kind == domain.PartImage {
			hasImages = true
			break
		}
	}
	if !hasImages {
		return wireutil.ToolResultText(result)
	}
	parts := make([]map[string]any, 0, len(result.Content))
	var text strings.Builder
	flushText := func() {
		if text.Len() > 0 {
			parts = append(parts, map[string]any{"type": "input_text", "text": text.String()})
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
				parts = append(parts, map[string]any{
					"type":      "input_image",
					"image_url": "data:" + part.Image.MediaType + ";base64," + part.Image.Data,
				})
			}
		case domain.PartArtifact:
			// See toolResultContent: refs stay in the canonical result only.
		}
	}
	flushText()
	return parts
}

type assistantMessageParts struct {
	text      strings.Builder
	toolCalls []map[string]any
}

func newAssistantMessageParts() *assistantMessageParts {
	return &assistantMessageParts{}
}

func (p *assistantMessageParts) addText(text string) {
	p.text.WriteString(text)
}

func (p *assistantMessageParts) addToolCall(call domain.ToolCall) {
	p.toolCalls = append(p.toolCalls, map[string]any{
		"id":   call.ID.String(),
		"type": "function",
		"function": map[string]any{
			"name":      call.Name,
			"arguments": string(call.Arguments),
		},
	})
}

func (p *assistantMessageParts) empty() bool {
	return p.text.Len() == 0 && len(p.toolCalls) == 0
}

func (p *assistantMessageParts) toMap() map[string]any {
	text := p.text.String()
	msg := map[string]any{"role": string(domain.RoleAssistant)}
	if text != "" {
		msg["content"] = text
	} else if len(p.toolCalls) > 0 {
		msg["content"] = nil
	} else {
		msg["content"] = ""
	}
	if len(p.toolCalls) > 0 {
		msg["tool_calls"] = p.toolCalls
	}
	return msg
}

func toolResultMessage(result domain.ToolResult) map[string]any {
	return map[string]any{
		"role":         "tool",
		"tool_call_id": result.CallID.String(),
		"content":      wireutil.ToolResultText(result),
	}
}

func chatCompletionsURL(baseURL string) string {
	trimmed := strings.TrimRight(strings.TrimSpace(baseURL), "/")
	switch {
	case trimmed == "":
		return ""
	case strings.HasSuffix(trimmed, "/chat/completions"):
		return trimmed
	case strings.HasSuffix(trimmed, "/v1"):
		return trimmed + "/chat/completions"
	default:
		return trimmed + "/v1/chat/completions"
	}
}

func responsesURL(baseURL string) string {
	trimmed := strings.TrimRight(strings.TrimSpace(baseURL), "/")
	switch {
	case trimmed == "":
		return ""
	case strings.HasSuffix(trimmed, "/responses"):
		return trimmed
	case strings.HasSuffix(trimmed, "/v1"):
		return trimmed + "/responses"
	default:
		return trimmed + "/v1/responses"
	}
}

// pump converts one SSE response body into canonical events; it runs inside
// the shared stream runner (model/stream).
func (p *Provider) pump(ctx context.Context, body io.Reader, emit stream.Emitter) {
	parser := sse.NewParser(body)
	state := newCanonicalState()

	switch p.wireAPI {
	case WireAPIResponses:
		runResponses(ctx, parser, state, emit)
	default:
		runChatCompletions(ctx, parser, state, emit)
	}
}

func runChatCompletions(ctx context.Context, parser *sse.Parser, state *canonicalState, emit stream.Emitter) {
	if !state.emitResponseStart(emit) {
		return
	}

	for {
		evt, err := parser.Next()
		if err != nil {
			finishChatReadError(ctx, state, err, emit)
			return
		}

		if evt.Data == "[DONE]" {
			finishChatDone(state, emit)
			return
		}

		var chunk chatCompletionChunk
		if err := json.Unmarshal([]byte(evt.Data), &chunk); err != nil {
			finishWithError(state, fmt.Errorf("openai provider: malformed chunk JSON: %w", err), domain.StopProviderError, emit)
			return
		}

		// Some compatible gateways report failures as an in-band error frame
		// (HTTP 200, then {"error": {...}}) instead of a status code. Once a
		// finish_reason chunk landed the generation is complete and paid for:
		// a trailing error frame must not turn it into a failure.
		if chunk.Error != nil && !state.finishSeen {
			code, message := chunk.Error.Code, strings.TrimSpace(chunk.Error.Message)
			if message == "" {
				message = "openai provider: stream error frame"
			}
			wireutil.EmitStreamFailure(emit, errors.New(message), domain.StopProviderError,
				isTransientProviderError(code, message), func() { _ = state.closeOpen(emit) })
			return
		}

		if err := state.applyChatChunk(chunk, emit); err != nil {
			finishWithError(state, err, domain.StopProviderError, emit)
			return
		}
	}
}

func runResponses(ctx context.Context, parser *sse.Parser, state *canonicalState, emit stream.Emitter) {
	for {
		evt, err := parser.Next()
		if err != nil {
			finishResponsesReadError(ctx, state, err, emit)
			return
		}

		if evt.Data == "[DONE]" {
			if !state.finishSeen {
				finishWithError(state, fmt.Errorf("openai provider: responses stream missing terminal event before [DONE]"), domain.StopProviderError, emit)
				return
			}
			state.flushBufferedTerminal(emit)
			return
		}

		var envelope responsesEventEnvelope
		if err := json.Unmarshal([]byte(evt.Data), &envelope); err != nil {
			finishWithError(state, fmt.Errorf("openai provider: malformed chunk JSON: %w", err), domain.StopProviderError, emit)
			return
		}

		eventName := strings.TrimSpace(evt.Name)
		if eventName == "" {
			eventName = strings.TrimSpace(envelope.Type)
		}
		if eventName == "" {
			finishWithError(state, fmt.Errorf("openai provider: missing responses SSE event name"), domain.StopProviderError, emit)
			return
		}
		if isReasoningEventName(eventName) {
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			if strings.HasSuffix(eventName, ".delta") {
				state.emitReasoningDelta(envelope.Delta, emit)
			} else if strings.HasSuffix(eventName, ".done") {
				state.closeReasoning(emit)
			}
			continue
		}
		if state.finishSeen {
			finishWithError(state, fmt.Errorf("openai provider: received event %q after terminal event", eventName), domain.StopProviderError, emit)
			return
		}

		switch eventName {
		case "response.created":
			if state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: duplicate response.created"), domain.StopProviderError, emit)
				return
			}
			if !state.emitResponseStart(emit) {
				return
			}
		case "response.in_progress", "response.content_part.added", "response.content_part.done":
			// Lifecycle/structural events carry no canonical visible content.
		case "response.output_text.delta":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			state.emitTextDelta(envelope.Delta, emit)
		case "response.output_text.done":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			state.closeText(emit)
		case "response.output_item.added", "response.output_item.done":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			if envelope.Item == nil {
				finishWithError(state, fmt.Errorf("openai provider: event %q missing item payload", eventName), domain.StopProviderError, emit)
				return
			}
			if isReasoningItemType(envelope.Item.Type) {
				continue
			}
			if err := state.applyResponseToolItem(*envelope.Item, envelope.outputIndex(), strings.HasSuffix(eventName, ".done"), emit); err != nil {
				finishWithError(state, err, domain.StopProviderError, emit)
				return
			}
		case "response.function_call_arguments.delta":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			state.applyResponseToolArgsDelta(envelope.outputIndex(), envelope.Delta, emit)
		case "response.function_call_arguments.done":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			if err := state.applyResponseToolArgsDone(envelope.outputIndex(), envelope.Arguments, emit); err != nil {
				finishWithError(state, err, domain.StopProviderError, emit)
				return
			}
		case "response.completed":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			if err := state.prepareBufferedTerminal(state.responsesCompletedStop(), responseUsage(envelope.Response), "", false, emit); err != nil {
				finishWithError(state, err, domain.StopProviderError, emit)
				return
			}
		case "response.incomplete":
			if !state.responseStarted {
				finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError, emit)
				return
			}
			reason := incompleteReason(envelope.Response)
			stop := mapIncompleteStopReason(reason)
			if stop == domain.StopMaxOutput {
				// An output-cap truncation is NOT a stream failure: the buffered
				// text survives, usage still counts, and the loop decides whether
				// to continue or salvage. Emitting a stream error here would kill
				// the run after the full generation cost was already paid — the
				// failure mode that wiped out whole sub-agent explorations
				// (docs/SUBAGENT_DESIGN.md §12).
				if err := state.prepareBufferedTerminal(stop, responseUsage(envelope.Response), "", false, emit); err != nil {
					finishWithError(state, err, domain.StopProviderError, emit)
					return
				}
			} else if err := state.prepareBufferedTerminal(stop, nil, incompleteMessage(reason), isTransientProviderError("", reason), emit); err != nil {
				finishWithError(state, err, domain.StopProviderError, emit)
				return
			}
		case "response.failed", "error":
			code, message := responseFailureError(eventName, envelope)
			// A server-side failure frame (overload, scheduler capacity, internal
			// hiccup) is as transient as a dropped connection: classify it so the
			// agent loop waits and retries instead of killing the run.
			if err := state.prepareBufferedTerminal(domain.StopProviderError, nil, message, isTransientProviderError(code, message), emit); err != nil {
				finishWithError(state, err, domain.StopProviderError, emit)
				return
			}
		default:
			finishWithError(state, fmt.Errorf("openai provider: unsupported responses event %q", eventName), domain.StopProviderError, emit)
			return
		}
	}
}

func finishChatDone(state *canonicalState, emit stream.Emitter) {
	if !state.finishSeen {
		state.finalStop = domain.StopUnknown
	}
	_ = state.closeOpen(emit)
	emit(domain.ModelEvent{
		Kind:       domain.ModelEventResponseEnd,
		StopReason: state.finalStop,
	})
}

// finishReadFailure binds the provider's close-and-emit failure finisher;
// retryable marks transient read failures (truncated body, transport drop)
// so the agent loop can re-issue the request while nothing was delivered
// yet.
func finishReadFailure(state *canonicalState, emit stream.Emitter) func(error, domain.StopReason, bool) {
	return func(err error, stop domain.StopReason, retryable bool) {
		wireutil.EmitStreamFailure(emit, err, stop, retryable, func() { _ = state.closeOpen(emit) })
	}
}

func finishChatReadError(ctx context.Context, state *canonicalState, err error, emit stream.Emitter) {
	wireutil.FinishReadError(ctx, err, "openai", "[DONE]",
		func() bool {
			if !state.finishSeen {
				return false
			}
			// Some compatible gateways close the connection right after the final
			// finish_reason chunk instead of sending the [DONE] sentinel. The
			// generation is already complete and paid for — finish gracefully the
			// same way the responses path does instead of discarding the reply.
			finishChatDone(state, emit)
			return true
		}, finishReadFailure(state, emit))
}

func finishResponsesReadError(ctx context.Context, state *canonicalState, err error, emit stream.Emitter) {
	wireutil.FinishReadError(ctx, err, "openai", "terminal event",
		func() bool {
			if !state.finishSeen {
				return false
			}
			// Some compatible gateways close immediately after response.completed
			// instead of sending the optional [DONE] sentinel.
			state.flushBufferedTerminal(emit)
			return true
		}, finishReadFailure(state, emit))
}

func finishWithError(state *canonicalState, err error, stop domain.StopReason, emit stream.Emitter) {
	wireutil.EmitStreamFailure(emit, err, stop, false, func() { _ = state.closeOpen(emit) })
}

type canonicalState struct {
	textOpen               bool
	reasoningOpen          bool
	responseStarted        bool
	finishSeen             bool
	finalStop              domain.StopReason
	bufferedUsage          *usageInfo
	bufferedError          string
	bufferedErrorRetryable bool
	toolUse                bool
	tools                  map[int]*toolState
}

func newCanonicalState() *canonicalState {
	return &canonicalState{tools: make(map[int]*toolState)}
}

func (s *canonicalState) emitResponseStart(emit func(domain.ModelEvent) bool) bool {
	if s.responseStarted {
		return true
	}
	s.responseStarted = true
	return emit(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
}

func (s *canonicalState) applyChatChunk(chunk chatCompletionChunk, emit func(domain.ModelEvent) bool) error {
	if len(chunk.Choices) > 1 {
		return fmt.Errorf("openai provider: multiple choices are not supported")
	}

	if len(chunk.Choices) == 1 {
		choice := chunk.Choices[0]
		if choice.Index != 0 {
			return fmt.Errorf("openai provider: unsupported choice index %d", choice.Index)
		}
		if s.finishSeen {
			return fmt.Errorf("openai provider: received delta after finish_reason")
		}

		text, hasText, err := decodeDeltaText(choice.Delta.Content)
		if err != nil {
			return err
		}
		if hasText {
			s.emitTextDelta(text, emit)
		}
		reasoning, hasReasoning, err := decodeDeltaText(choice.Delta.ReasoningContent)
		if err != nil {
			return err
		}
		if !hasReasoning {
			reasoning, hasReasoning, err = decodeDeltaText(choice.Delta.Thinking)
			if err != nil {
				return err
			}
		}
		if hasReasoning {
			s.emitReasoningDelta(reasoning, emit)
		}

		for _, delta := range choice.Delta.ToolCalls {
			if err := s.applyToolDelta(delta, emit); err != nil {
				return err
			}
		}

		if choice.FinishReason != "" {
			s.finishSeen = true
			s.finalStop = mapStopReason(choice.FinishReason)
			if err := s.closeOpen(emit); err != nil {
				return err
			}
		}
	}

	if chunk.Usage != nil {
		emit(domain.ModelEvent{
			Kind:              domain.ModelEventUsage,
			InputTokens:       chunk.Usage.PromptTokens,
			OutputTokens:      chunk.Usage.CompletionTokens,
			CachedInputTokens: chunk.Usage.cachedInputTokens(),
			ReasoningTokens:   chunk.Usage.reasoningTokens(),
			// prompt_tokens is already cache-inclusive, so it is the exact
			// context-window footprint of the request.
			ContextTokens: chunk.Usage.PromptTokens,
		})
	}

	return nil
}

func (s *canonicalState) emitTextDelta(text string, emit func(domain.ModelEvent) bool) {
	if !s.textOpen {
		emit(domain.ModelEvent{Kind: domain.ModelEventTextStart})
		s.textOpen = true
	}
	if text != "" {
		emit(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: text})
	}
}

func (s *canonicalState) closeText(emit func(domain.ModelEvent) bool) {
	if !s.textOpen {
		return
	}
	emit(domain.ModelEvent{Kind: domain.ModelEventTextEnd})
	s.textOpen = false
}

func (s *canonicalState) emitReasoningDelta(text string, emit func(domain.ModelEvent) bool) {
	if !s.reasoningOpen {
		emit(domain.ModelEvent{Kind: domain.ModelEventReasoningStart})
		s.reasoningOpen = true
	}
	if text != "" {
		emit(domain.ModelEvent{Kind: domain.ModelEventReasoningDelta, ReasoningDelta: text})
	}
}

func (s *canonicalState) closeReasoning(emit func(domain.ModelEvent) bool) {
	if !s.reasoningOpen {
		return
	}
	emit(domain.ModelEvent{Kind: domain.ModelEventReasoningEnd})
	s.reasoningOpen = false
}

func (s *canonicalState) applyToolDelta(delta toolCallDelta, emit func(domain.ModelEvent) bool) error {
	index := 0
	if delta.Index != nil {
		index = *delta.Index
	}
	tool := s.ensureTool(index)
	if delta.ID != "" {
		tool.id = delta.ID
	}
	if delta.Function.Name != "" {
		tool.name = delta.Function.Name
	}
	if err := s.startToolIfReady(tool, emit); err != nil {
		return err
	}
	s.emitToolArgs(tool, delta.Function.Arguments, emit)
	return nil
}

func (s *canonicalState) applyResponseToolItem(item responsesOutputItem, index int, done bool, emit func(domain.ModelEvent) bool) error {
	if item.Type != "function_call" {
		return nil
	}
	tool := s.ensureTool(index)
	if item.CallID != "" {
		tool.id = item.CallID
	}
	if item.Name != "" {
		tool.name = item.Name
	}
	if item.Arguments != "" {
		tool.itemArguments = item.Arguments
	}
	if err := s.startToolIfReady(tool, emit); err != nil {
		return err
	}
	if done {
		return s.closeTool(tool, emit)
	}
	return nil
}

func (s *canonicalState) applyResponseToolArgsDelta(index int, delta string, emit func(domain.ModelEvent) bool) {
	tool := s.ensureTool(index)
	s.emitToolArgs(tool, delta, emit)
}

func (s *canonicalState) applyResponseToolArgsDone(index int, arguments string, emit func(domain.ModelEvent) bool) error {
	tool := s.ensureTool(index)
	return s.emitToolArgumentSnapshot(tool, arguments, emit)
}

func (s *canonicalState) prepareBufferedTerminal(stop domain.StopReason, usage *usageInfo, streamErr string, retryable bool, emit func(domain.ModelEvent) bool) error {
	if s.finishSeen {
		return fmt.Errorf("openai provider: duplicate terminal event")
	}
	if err := s.closeOpen(emit); err != nil {
		return err
	}
	s.finishSeen = true
	s.finalStop = stop
	s.bufferedError = streamErr
	s.bufferedErrorRetryable = retryable
	s.bufferedUsage = usage
	if streamErr != "" {
		s.bufferedUsage = nil
	}
	return nil
}

func (s *canonicalState) flushBufferedTerminal(emit func(domain.ModelEvent) bool) {
	if s.bufferedError != "" {
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventStreamError,
			Error:     s.bufferedError,
			Retryable: s.bufferedErrorRetryable,
		})
	}
	if s.bufferedUsage != nil {
		emit(domain.ModelEvent{
			Kind:              domain.ModelEventUsage,
			InputTokens:       s.bufferedUsage.PromptTokens,
			OutputTokens:      s.bufferedUsage.CompletionTokens,
			CachedInputTokens: s.bufferedUsage.cachedInputTokens(),
			ReasoningTokens:   s.bufferedUsage.reasoningTokens(),
			// prompt_tokens is already cache-inclusive, so it is the exact
			// context-window footprint of the request.
			ContextTokens: s.bufferedUsage.PromptTokens,
		})
	}
	emit(domain.ModelEvent{
		Kind:       domain.ModelEventResponseEnd,
		StopReason: s.finalStop,
	})
}

func (s *canonicalState) responsesCompletedStop() domain.StopReason {
	if s.toolUse {
		return domain.StopToolUse
	}
	return domain.StopEndTurn
}

func (s *canonicalState) ensureTool(index int) *toolState {
	tool, ok := s.tools[index]
	if ok {
		return tool
	}
	tool = &toolState{index: index}
	s.tools[index] = tool
	return tool
}

func (s *canonicalState) startToolIfReady(tool *toolState, emit func(domain.ModelEvent) bool) error {
	if tool.started || tool.ended {
		return nil
	}
	if tool.id == "" || tool.name == "" {
		return nil
	}
	emit(domain.ModelEvent{
		Kind:      domain.ModelEventToolCallStart,
		ToolIndex: tool.index,
		ToolID:    tool.id,
		ToolName:  tool.name,
	})
	tool.started = true
	s.toolUse = true
	if tool.pendingArgs != "" {
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventToolArgsDelta,
			ToolIndex: tool.index,
			ToolID:    tool.id,
			ToolName:  tool.name,
			ToolArgs:  tool.pendingArgs,
		})
		tool.pendingArgs = ""
	}
	return nil
}

func (s *canonicalState) emitToolArgs(tool *toolState, args string, emit func(domain.ModelEvent) bool) {
	if args == "" {
		return
	}
	tool.assembledArgs += args
	if tool.started {
		emit(domain.ModelEvent{
			Kind:      domain.ModelEventToolArgsDelta,
			ToolIndex: tool.index,
			ToolID:    tool.id,
			ToolName:  tool.name,
			ToolArgs:  args,
		})
		return
	}
	tool.pendingArgs += args
}

func (s *canonicalState) emitToolArgumentSnapshot(tool *toolState, arguments string, emit func(domain.ModelEvent) bool) error {
	if arguments == "" {
		return nil
	}
	if arguments == tool.assembledArgs {
		return nil
	}
	if !strings.HasPrefix(arguments, tool.assembledArgs) {
		return fmt.Errorf("openai provider: tool arguments mismatch at index %d", tool.index)
	}
	s.emitToolArgs(tool, arguments[len(tool.assembledArgs):], emit)
	return nil
}

func (s *canonicalState) closeTool(tool *toolState, emit func(domain.ModelEvent) bool) error {
	if tool.ended {
		return nil
	}
	if tool.itemArguments != "" {
		if err := s.emitToolArgumentSnapshot(tool, tool.itemArguments, emit); err != nil {
			return err
		}
	}
	if tool.id == "" || tool.name == "" {
		return fmt.Errorf("openai provider: incomplete tool call at index %d", tool.index)
	}
	if err := s.startToolIfReady(tool, emit); err != nil {
		return err
	}
	emit(domain.ModelEvent{
		Kind:      domain.ModelEventToolCallEnd,
		ToolIndex: tool.index,
		ToolID:    tool.id,
		ToolName:  tool.name,
	})
	tool.ended = true
	return nil
}

func (s *canonicalState) closeOpen(emit func(domain.ModelEvent) bool) error {
	s.closeText(emit)
	s.closeReasoning(emit)
	if len(s.tools) == 0 {
		return nil
	}

	indexes := make([]int, 0, len(s.tools))
	for index := range s.tools {
		indexes = append(indexes, index)
	}
	sort.Ints(indexes)
	for _, index := range indexes {
		tool := s.tools[index]
		if err := s.closeTool(tool, emit); err != nil {
			return err
		}
		delete(s.tools, index)
	}
	return nil
}

type toolState struct {
	index         int
	id            string
	name          string
	assembledArgs string
	pendingArgs   string
	itemArguments string
	started       bool
	ended         bool
}

type chatCompletionChunk struct {
	Choices []chatChoice `json:"choices"`
	Usage   *usageInfo   `json:"usage,omitempty"`
	// Error carries in-band failure frames ({"error": {...}}) some
	// compatible gateways emit mid-stream instead of an HTTP error status.
	Error *openAIError `json:"error,omitempty"`
}

type chatChoice struct {
	Index        int       `json:"index"`
	Delta        chatDelta `json:"delta"`
	FinishReason string    `json:"finish_reason"`
}

type chatDelta struct {
	Content          json.RawMessage `json:"content"`
	ReasoningContent json.RawMessage `json:"reasoning_content,omitempty"`
	Thinking         json.RawMessage `json:"thinking,omitempty"`
	ToolCalls        []toolCallDelta `json:"tool_calls,omitempty"`
}

type toolCallDelta struct {
	Index    *int             `json:"index,omitempty"`
	ID       string           `json:"id,omitempty"`
	Function toolCallFunction `json:"function,omitempty"`
}

type toolCallFunction struct {
	Name      string `json:"name,omitempty"`
	Arguments string `json:"arguments,omitempty"`
}

type usageInfo struct {
	PromptTokens     int64 `json:"prompt_tokens,omitempty"`
	CompletionTokens int64 `json:"completion_tokens,omitempty"`
	InputTokens      int64 `json:"input_tokens,omitempty"`
	OutputTokens     int64 `json:"output_tokens,omitempty"`
	// Chat Completions nests cache hits under prompt_tokens_details; the
	// Responses API nests them under input_tokens_details.
	PromptTokensDetails *cachedTokenDetails `json:"prompt_tokens_details,omitempty"`
	InputTokensDetails  *cachedTokenDetails `json:"input_tokens_details,omitempty"`
	// Chat Completions nests the reasoning share under
	// completion_tokens_details; the Responses API under
	// output_tokens_details.
	CompletionTokensDetails *reasoningTokenDetails `json:"completion_tokens_details,omitempty"`
	OutputTokensDetails     *reasoningTokenDetails `json:"output_tokens_details,omitempty"`
}

type cachedTokenDetails struct {
	CachedTokens int64 `json:"cached_tokens,omitempty"`
}

type reasoningTokenDetails struct {
	ReasoningTokens int64 `json:"reasoning_tokens,omitempty"`
}

// cachedInputTokens extracts the prompt-cache hit count (0 when absent).
func (u *usageInfo) cachedInputTokens() int64 {
	if u == nil {
		return 0
	}
	if u.PromptTokensDetails != nil && u.PromptTokensDetails.CachedTokens > 0 {
		return u.PromptTokensDetails.CachedTokens
	}
	if u.InputTokensDetails != nil {
		return u.InputTokensDetails.CachedTokens
	}
	return 0
}

// reasoningTokens extracts the provider-metered reasoning/thinking share
// of the completion tokens (0 when absent).
func (u *usageInfo) reasoningTokens() int64 {
	if u == nil {
		return 0
	}
	if u.CompletionTokensDetails != nil && u.CompletionTokensDetails.ReasoningTokens > 0 {
		return u.CompletionTokensDetails.ReasoningTokens
	}
	if u.OutputTokensDetails != nil {
		return u.OutputTokensDetails.ReasoningTokens
	}
	return 0
}

type responsesEventEnvelope struct {
	Type        string               `json:"type,omitempty"`
	Response    *responsesResponse   `json:"response,omitempty"`
	Item        *responsesOutputItem `json:"item,omitempty"`
	OutputIndex *int                 `json:"output_index,omitempty"`
	Delta       string               `json:"delta,omitempty"`
	Arguments   string               `json:"arguments,omitempty"`
	Error       *openAIError         `json:"error,omitempty"`
	// The responses "error" event carries code/message at the top level
	// instead of nested under an error object.
	Code    string `json:"code,omitempty"`
	Message string `json:"message,omitempty"`
}

func (e responsesEventEnvelope) outputIndex() int {
	if e.OutputIndex == nil {
		return 0
	}
	return *e.OutputIndex
}

type responsesResponse struct {
	ID                string                      `json:"id,omitempty"`
	Status            string                      `json:"status,omitempty"`
	Usage             *usageInfo                  `json:"usage,omitempty"`
	Error             *openAIError                `json:"error,omitempty"`
	IncompleteDetails *responsesIncompleteDetails `json:"incomplete_details,omitempty"`
}

type responsesIncompleteDetails struct {
	Reason string `json:"reason,omitempty"`
}

type openAIError struct {
	Code    string `json:"code,omitempty"`
	Message string `json:"message,omitempty"`
}

type responsesOutputItem struct {
	Type      string `json:"type,omitempty"`
	CallID    string `json:"call_id,omitempty"`
	Name      string `json:"name,omitempty"`
	Arguments string `json:"arguments,omitempty"`
}

func responseUsage(resp *responsesResponse) *usageInfo {
	if resp == nil || resp.Usage == nil {
		return nil
	}
	if resp.Usage.PromptTokens == 0 && resp.Usage.CompletionTokens == 0 {
		return &usageInfo{
			PromptTokens:            resp.Usage.InputTokens,
			CompletionTokens:        resp.Usage.OutputTokens,
			InputTokensDetails:      resp.Usage.InputTokensDetails,
			PromptTokensDetails:     resp.Usage.PromptTokensDetails,
			CompletionTokensDetails: resp.Usage.CompletionTokensDetails,
			OutputTokensDetails:     resp.Usage.OutputTokensDetails,
		}
	}
	return &usageInfo{
		PromptTokens:            resp.Usage.PromptTokens,
		CompletionTokens:        resp.Usage.CompletionTokens,
		PromptTokensDetails:     resp.Usage.PromptTokensDetails,
		InputTokensDetails:      resp.Usage.InputTokensDetails,
		CompletionTokensDetails: resp.Usage.CompletionTokensDetails,
		OutputTokensDetails:     resp.Usage.OutputTokensDetails,
	}
}

func incompleteReason(resp *responsesResponse) string {
	if resp == nil || resp.IncompleteDetails == nil {
		return ""
	}
	return strings.TrimSpace(resp.IncompleteDetails.Reason)
}

func incompleteMessage(reason string) string {
	if reason == "" {
		return "openai provider: response incomplete"
	}
	return fmt.Sprintf("openai provider: response incomplete: %s", reason)
}

// responseFailureError extracts the machine-readable code and the human
// message from a response.failed / error frame; either may be empty when the
// gateway omits them.
func responseFailureError(eventName string, envelope responsesEventEnvelope) (code, message string) {
	err := envelope.Error
	if err == nil && envelope.Response != nil {
		err = envelope.Response.Error
	}
	if err != nil {
		code = err.Code
		if strings.TrimSpace(err.Message) != "" {
			message = err.Message
		}
	}
	if code == "" {
		code = envelope.Code
	}
	if message == "" {
		message = strings.TrimSpace(envelope.Message)
	}
	if message == "" {
		message = fmt.Sprintf("openai provider: %s", eventName)
	}
	return code, message
}

// transientProviderErrorCodes are wire error codes that signal a server-side
// transient condition (capacity, overload, internal hiccups) rather than a
// problem with the request itself — safe to retry after a backoff.
var transientProviderErrorCodes = map[string]bool{
	"internal":                true,
	"internal_error":          true,
	"server_error":            true,
	"overloaded":              true,
	"overloaded_error":        true,
	"unavailable":             true,
	"service_unavailable":     true,
	"temporarily_unavailable": true,
	"scheduler_unavailable":   true,
	"rate_limit":              true,
	"rate_limit_exceeded":     true,
	"timeout":                 true,
	"request_timeout":         true,
}

// nonRetryableErrorMarkers identify request/account problems that retrying
// can never fix (bad input, quota, auth); they win over any transient hint.
var nonRetryableErrorMarkers = []string{
	"quota", "billing", "invalid", "malformed", "context length", "context window",
	"maximum context", "authentication", "api key", "permission", "not found",
}

// transientErrorMarkers match capacity/availability phrases in free-text
// messages when the wire code is missing or unrecognized.
var transientErrorMarkers = []string{
	"scheduler unavailable", "overloaded", "temporarily", "service unavailable",
	"internal error", "internal server", "capacity", "timed out", "timeout",
	"rate limit", "too many requests", "try again", "unavailable",
}

// isTransientProviderError classifies an in-band provider error frame. The
// classification only matters while the stream delivered no content — the
// agent loop never retries once partial output reached the transcript — and
// context-overflow messages are routed before the retry check, so a false
// positive here degrades to a bounded backoff, not a silent re-issue.
func isTransientProviderError(code, message string) bool {
	lowerMessage := strings.ToLower(message)
	for _, marker := range nonRetryableErrorMarkers {
		if strings.Contains(lowerMessage, marker) {
			return false
		}
	}
	if transientProviderErrorCodes[strings.ToLower(strings.TrimSpace(code))] {
		return true
	}
	for _, marker := range transientErrorMarkers {
		if strings.Contains(lowerMessage, marker) {
			return true
		}
	}
	return false
}

func mapIncompleteStopReason(reason string) domain.StopReason {
	switch strings.ToLower(strings.TrimSpace(reason)) {
	case "max_output_tokens", "max_output", "max_tokens", "output_tokens", "length":
		return domain.StopMaxOutput
	default:
		return domain.StopProviderError
	}
}

func isReasoningEventName(eventName string) bool {
	return strings.Contains(eventName, "reasoning")
}

func isReasoningItemType(itemType string) bool {
	return strings.Contains(itemType, "reasoning")
}

func decodeDeltaText(raw json.RawMessage) (string, bool, error) {
	if len(raw) == 0 {
		return "", false, nil
	}
	if string(raw) == "null" {
		return "", false, nil
	}

	var text string
	if err := json.Unmarshal(raw, &text); err != nil {
		return "", false, fmt.Errorf("openai provider: unsupported delta content: %s", string(raw))
	}
	return text, true, nil
}

func mapStopReason(reason string) domain.StopReason {
	switch reason {
	case "stop":
		return domain.StopEndTurn
	case "tool_calls", "function_call":
		return domain.StopToolUse
	case "length":
		return domain.StopMaxOutput
	case "content_filter":
		return domain.StopContentFilter
	case "cancelled":
		return domain.StopCancelled
	default:
		return domain.StopUnknown
	}
}
