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
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	defaultBaseURL        = "https://api.openai.com/v1"
	defaultInitialBackoff = 200 * time.Millisecond
	defaultMaxBackoff     = 2 * time.Second
	statusBodyLimit       = 4096
)

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
	endpointURL    string
	apiKey         string
	client         *http.Client
	wireAPI        WireAPI
	maxRetries     int
	initialBackoff time.Duration
	maxBackoff     time.Duration
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

	client := cfg.HTTPClient
	if client == nil {
		client = http.DefaultClient
	}

	initialBackoff := cfg.InitialBackoff
	if initialBackoff <= 0 {
		initialBackoff = defaultInitialBackoff
	}

	maxBackoff := cfg.MaxBackoff
	if maxBackoff <= 0 {
		maxBackoff = defaultMaxBackoff
	}
	if maxBackoff < initialBackoff {
		maxBackoff = initialBackoff
	}

	if cfg.MaxRetries < 0 {
		return nil, fmt.Errorf("openai provider: max retries must be >= 0")
	}

	return &Provider{
		endpointURL:    endpointURL,
		apiKey:         cfg.APIKey,
		client:         client,
		wireAPI:        wireAPI,
		maxRetries:     cfg.MaxRetries,
		initialBackoff: initialBackoff,
		maxBackoff:     maxBackoff,
	}, nil
}

// Stream starts a streaming request against the configured wire API.
func (p *Provider) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	body, err := marshalRequest(req, p.wireAPI)
	if err != nil {
		return nil, err
	}

	streamCtx, cancel := context.WithCancel(ctx)
	resp, err := p.doRequestWithRetry(streamCtx, body)
	if err != nil {
		cancel()
		return nil, err
	}

	if err := validateStreamResponse(resp); err != nil {
		_ = resp.Body.Close()
		cancel()
		return nil, err
	}

	stream := newOpenAIStream(streamCtx, cancel, resp.Body, p.wireAPI)
	go stream.run()
	return stream, nil
}

func (p *Provider) doRequestWithRetry(ctx context.Context, body []byte) (*http.Response, error) {
	var lastErr error
	for attempt := 0; attempt <= p.maxRetries; attempt++ {
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.endpointURL, bytes.NewReader(body))
		if err != nil {
			return nil, fmt.Errorf("openai provider: create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "text/event-stream")
		req.Header.Set("Cache-Control", "no-cache")
		if p.apiKey != "" {
			req.Header.Set("Authorization", "Bearer "+p.apiKey)
		}

		resp, err := p.client.Do(req)
		if err != nil {
			if !shouldRetryRequestError(ctx, err) || attempt == p.maxRetries {
				return nil, fmt.Errorf("openai provider: request failed: %w", err)
			}
			lastErr = err
			if err := sleepContext(ctx, p.backoff(attempt)); err != nil {
				return nil, err
			}
			continue
		}

		if shouldRetryStatus(resp.StatusCode) && attempt < p.maxRetries {
			_ = drainAndClose(resp.Body)
			if err := sleepContext(ctx, p.backoff(attempt)); err != nil {
				return nil, err
			}
			continue
		}

		if resp.StatusCode != http.StatusOK {
			statusErr := readStatusError(resp)
			_ = resp.Body.Close()
			if lastErr != nil {
				return nil, fmt.Errorf("openai provider: request failed after retry: %w", statusErr)
			}
			return nil, statusErr
		}

		return resp, nil
	}

	if lastErr != nil {
		return nil, fmt.Errorf("openai provider: request failed after retry: %w", lastErr)
	}
	return nil, fmt.Errorf("openai provider: request failed")
}

func (p *Provider) backoff(attempt int) time.Duration {
	backoff := p.initialBackoff
	for i := 0; i < attempt; i++ {
		backoff *= 2
		if backoff >= p.maxBackoff {
			return p.maxBackoff
		}
	}
	if backoff > p.maxBackoff {
		return p.maxBackoff
	}
	return backoff
}

func validateStreamResponse(resp *http.Response) error {
	contentType := resp.Header.Get("Content-Type")
	if contentType == "" {
		return fmt.Errorf("openai provider: missing Content-Type")
	}
	mediaType, _, err := mime.ParseMediaType(contentType)
	if err != nil {
		return fmt.Errorf("openai provider: invalid Content-Type %q: %w", contentType, err)
	}
	if mediaType != "text/event-stream" {
		return fmt.Errorf("openai provider: unexpected Content-Type %q", contentType)
	}
	return nil
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
	if len(req.Tools) > 0 {
		tools, err := toOpenAITools(req.Tools)
		if err != nil {
			return nil, err
		}
		payload["tools"] = tools
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
	if len(req.Tools) > 0 {
		tools, err := toResponsesTools(req.Tools)
		if err != nil {
			return nil, err
		}
		payload["tools"] = tools
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
		parameters, err := decodeToolParameters(def)
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
		parameters, err := decodeToolParameters(def)
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

func decodeToolParameters(def domain.ToolDefinition) (any, error) {
	var parameters any
	if len(def.InputSchema) == 0 {
		parameters = map[string]any{"type": "object"}
	} else if err := json.Unmarshal(def.InputSchema, &parameters); err != nil {
		return nil, fmt.Errorf("openai provider: decode tool schema for %q: %w", def.Name, err)
	}
	return parameters, nil
}

func toOpenAIMessages(messages []domain.Message) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(messages))
	for _, msg := range messages {
		switch msg.Role {
		case domain.RoleSystem, domain.RoleUser:
			text, err := messageText(msg)
			if err != nil {
				return nil, err
			}
			out = append(out, map[string]any{
				"role":    string(msg.Role),
				"content": text,
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

func toResponsesInput(messages []domain.Message) ([]map[string]any, error) {
	out := make([]map[string]any, 0, len(messages))
	for _, msg := range messages {
		switch msg.Role {
		case domain.RoleSystem, domain.RoleUser:
			text, err := messageText(msg)
			if err != nil {
				return nil, err
			}
			out = append(out, responseMessageItem(msg.Role, "input_text", text))
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
		"output":  toolResultContent(result),
	}
}

func messageText(msg domain.Message) (string, error) {
	var b strings.Builder
	for _, part := range msg.Parts {
		if part.Kind != domain.PartText {
			return "", fmt.Errorf("openai provider: role %q only supports text parts", msg.Role)
		}
		b.WriteString(part.Text)
	}
	return b.String(), nil
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
		"content":      toolResultContent(result),
	}
}

func toolResultContent(result domain.ToolResult) string {
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
	var text strings.Builder
	for _, part := range result.Content {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartArtifact:
			// Artifact references are persisted in the canonical ToolResult. Tools
			// include model-safe reference metadata in their bounded text payload;
			// do not duplicate refs here and accidentally exceed context budgets.
		default:
			textAndArtifactRefs = false
		}
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

type statusError struct {
	Code    int
	Status  string
	Message string
}

func (e *statusError) Error() string {
	if e.Message == "" {
		return fmt.Sprintf("openai provider: HTTP %d %s", e.Code, e.Status)
	}
	return fmt.Sprintf("openai provider: HTTP %d %s: %s", e.Code, e.Status, e.Message)
}

func readStatusError(resp *http.Response) error {
	defer resp.Body.Close()

	body, _ := io.ReadAll(io.LimitReader(resp.Body, statusBodyLimit))
	message := strings.TrimSpace(string(body))

	var envelope struct {
		Error struct {
			Message string `json:"message"`
			Type    string `json:"type"`
			Code    any    `json:"code"`
		} `json:"error"`
	}
	if len(body) > 0 && json.Unmarshal(body, &envelope) == nil && envelope.Error.Message != "" {
		message = envelope.Error.Message
	}

	return &statusError{
		Code:    resp.StatusCode,
		Status:  resp.Status,
		Message: message,
	}
}

func shouldRetryStatus(code int) bool {
	return code == http.StatusTooManyRequests || code >= 500
}

func shouldRetryRequestError(ctx context.Context, err error) bool {
	if err == nil {
		return false
	}
	if ctx.Err() != nil {
		return false
	}
	return !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded)
}

func sleepContext(ctx context.Context, d time.Duration) error {
	timer := time.NewTimer(d)
	defer timer.Stop()

	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

func drainAndClose(body io.ReadCloser) error {
	_, _ = io.Copy(io.Discard, io.LimitReader(body, statusBodyLimit))
	return body.Close()
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

type openAIStream struct {
	ctx       context.Context
	cancel    context.CancelFunc
	body      io.ReadCloser
	wireAPI   WireAPI
	events    chan domain.ModelEvent
	closed    chan struct{}
	closeOnce sync.Once
}

func newOpenAIStream(ctx context.Context, cancel context.CancelFunc, body io.ReadCloser, wireAPI WireAPI) *openAIStream {
	return &openAIStream{
		ctx:     ctx,
		cancel:  cancel,
		body:    body,
		wireAPI: wireAPI,
		events:  make(chan domain.ModelEvent, 64),
		closed:  make(chan struct{}),
	}
}

func (s *openAIStream) Recv() (domain.ModelEvent, error) {
	evt, ok := <-s.events
	if !ok {
		return domain.ModelEvent{}, io.EOF
	}
	return evt, nil
}

func (s *openAIStream) Close() error {
	var err error
	s.closeOnce.Do(func() {
		close(s.closed)
		s.cancel()
		err = s.body.Close()
	})
	return err
}

func (s *openAIStream) run() {
	defer close(s.events)
	defer s.Close()

	parser := newSSEParser(s.body)
	state := newCanonicalState()

	switch s.wireAPI {
	case WireAPIResponses:
		s.runResponses(parser, state)
	default:
		s.runChatCompletions(parser, state)
	}
}

func (s *openAIStream) runChatCompletions(parser *sseParser, state *canonicalState) {
	if !state.emitResponseStart(s.emit) {
		return
	}

	for {
		evt, err := parser.Next()
		if err != nil {
			s.finishChatReadError(state, err)
			return
		}

		if evt.Data == "[DONE]" {
			s.finishChatDone(state)
			return
		}

		var chunk chatCompletionChunk
		if err := json.Unmarshal([]byte(evt.Data), &chunk); err != nil {
			s.finishWithError(state, fmt.Errorf("openai provider: malformed chunk JSON: %w", err), domain.StopProviderError)
			return
		}

		if err := state.applyChatChunk(chunk, s.emit); err != nil {
			s.finishWithError(state, err, domain.StopProviderError)
			return
		}
	}
}

func (s *openAIStream) runResponses(parser *sseParser, state *canonicalState) {
	for {
		evt, err := parser.Next()
		if err != nil {
			s.finishResponsesReadError(state, err)
			return
		}

		if evt.Data == "[DONE]" {
			if !state.finishSeen {
				s.finishWithError(state, fmt.Errorf("openai provider: responses stream missing terminal event before [DONE]"), domain.StopProviderError)
				return
			}
			state.flushBufferedTerminal(s.emit)
			return
		}

		var envelope responsesEventEnvelope
		if err := json.Unmarshal([]byte(evt.Data), &envelope); err != nil {
			s.finishWithError(state, fmt.Errorf("openai provider: malformed chunk JSON: %w", err), domain.StopProviderError)
			return
		}

		eventName := strings.TrimSpace(evt.Event)
		if eventName == "" {
			eventName = strings.TrimSpace(envelope.Type)
		}
		if eventName == "" {
			s.finishWithError(state, fmt.Errorf("openai provider: missing responses SSE event name"), domain.StopProviderError)
			return
		}
		if isReasoningEventName(eventName) {
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			if strings.HasSuffix(eventName, ".delta") {
				state.emitReasoningDelta(envelope.Delta, s.emit)
			} else if strings.HasSuffix(eventName, ".done") {
				state.closeReasoning(s.emit)
			}
			continue
		}
		if state.finishSeen {
			s.finishWithError(state, fmt.Errorf("openai provider: received event %q after terminal event", eventName), domain.StopProviderError)
			return
		}

		switch eventName {
		case "response.created":
			if state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: duplicate response.created"), domain.StopProviderError)
				return
			}
			if !state.emitResponseStart(s.emit) {
				return
			}
		case "response.in_progress", "response.content_part.added", "response.content_part.done":
			// Lifecycle/structural events carry no canonical visible content.
		case "response.output_text.delta":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			state.emitTextDelta(envelope.Delta, s.emit)
		case "response.output_text.done":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			state.closeText(s.emit)
		case "response.output_item.added", "response.output_item.done":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			if envelope.Item == nil {
				s.finishWithError(state, fmt.Errorf("openai provider: event %q missing item payload", eventName), domain.StopProviderError)
				return
			}
			if isReasoningItemType(envelope.Item.Type) {
				continue
			}
			if err := state.applyResponseToolItem(*envelope.Item, envelope.outputIndex(), strings.HasSuffix(eventName, ".done"), s.emit); err != nil {
				s.finishWithError(state, err, domain.StopProviderError)
				return
			}
		case "response.function_call_arguments.delta":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			state.applyResponseToolArgsDelta(envelope.outputIndex(), envelope.Delta, s.emit)
		case "response.function_call_arguments.done":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			if err := state.applyResponseToolArgsDone(envelope.outputIndex(), envelope.Arguments, s.emit); err != nil {
				s.finishWithError(state, err, domain.StopProviderError)
				return
			}
		case "response.completed":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			if err := state.prepareBufferedTerminal(state.responsesCompletedStop(), responseUsage(envelope.Response), "", s.emit); err != nil {
				s.finishWithError(state, err, domain.StopProviderError)
				return
			}
		case "response.incomplete":
			if !state.responseStarted {
				s.finishWithError(state, fmt.Errorf("openai provider: received %q before response.created", eventName), domain.StopProviderError)
				return
			}
			reason := incompleteReason(envelope.Response)
			if err := state.prepareBufferedTerminal(mapIncompleteStopReason(reason), nil, incompleteMessage(reason), s.emit); err != nil {
				s.finishWithError(state, err, domain.StopProviderError)
				return
			}
		case "response.failed", "error":
			message := responseFailureMessage(eventName, envelope)
			if err := state.prepareBufferedTerminal(domain.StopProviderError, nil, message, s.emit); err != nil {
				s.finishWithError(state, err, domain.StopProviderError)
				return
			}
		default:
			s.finishWithError(state, fmt.Errorf("openai provider: unsupported responses event %q", eventName), domain.StopProviderError)
			return
		}
	}
}

func (s *openAIStream) emit(evt domain.ModelEvent) bool {
	select {
	case <-s.closed:
		return false
	case s.events <- evt:
		return true
	}
}

func (s *openAIStream) finishChatDone(state *canonicalState) {
	if !state.finishSeen {
		state.finalStop = domain.StopUnknown
	}
	_ = state.closeOpen(s.emit)
	s.emit(domain.ModelEvent{
		Kind:       domain.ModelEventResponseEnd,
		StopReason: state.finalStop,
	})
}

func (s *openAIStream) finishChatReadError(state *canonicalState, err error) {
	switch {
	case s.ctx.Err() != nil:
		s.finishWithError(state, s.ctx.Err(), domain.StopCancelled)
	case errors.Is(err, io.EOF):
		s.finishWithError(state, fmt.Errorf("openai provider: stream closed before [DONE]"), domain.StopProviderError)
	default:
		s.finishWithError(state, fmt.Errorf("openai provider: stream read failed: %w", err), domain.StopProviderError)
	}
}

func (s *openAIStream) finishResponsesReadError(state *canonicalState, err error) {
	switch {
	case s.ctx.Err() != nil:
		s.finishWithError(state, s.ctx.Err(), domain.StopCancelled)
	case errors.Is(err, io.EOF) && state.finishSeen:
		// Some compatible gateways close immediately after response.completed
		// instead of sending the optional [DONE] sentinel.
		state.flushBufferedTerminal(s.emit)
	case errors.Is(err, io.EOF):
		s.finishWithError(state, fmt.Errorf("openai provider: responses stream closed before terminal event"), domain.StopProviderError)
	default:
		s.finishWithError(state, fmt.Errorf("openai provider: stream read failed: %w", err), domain.StopProviderError)
	}
}

func (s *openAIStream) finishWithError(state *canonicalState, err error, stop domain.StopReason) {
	_ = state.closeOpen(s.emit)
	s.emit(domain.ModelEvent{
		Kind:  domain.ModelEventStreamError,
		Error: err.Error(),
	})
	s.emit(domain.ModelEvent{
		Kind:       domain.ModelEventResponseEnd,
		StopReason: stop,
	})
}

type canonicalState struct {
	textOpen        bool
	reasoningOpen   bool
	responseStarted bool
	finishSeen      bool
	finalStop       domain.StopReason
	bufferedUsage   *usageInfo
	bufferedError   string
	toolUse         bool
	tools           map[int]*toolState
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
			Kind:         domain.ModelEventUsage,
			InputTokens:  chunk.Usage.PromptTokens,
			OutputTokens: chunk.Usage.CompletionTokens,
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
		tool.doneSeen = true
		return s.closeTool(tool, emit)
	}
	return nil
}

func (s *canonicalState) applyResponseToolArgsDelta(index int, delta string, emit func(domain.ModelEvent) bool) {
	tool := s.ensureTool(index)
	tool.sawArgumentEvents = true
	s.emitToolArgs(tool, delta, emit)
}

func (s *canonicalState) applyResponseToolArgsDone(index int, arguments string, emit func(domain.ModelEvent) bool) error {
	tool := s.ensureTool(index)
	tool.sawArgumentEvents = true
	return s.emitToolArgumentSnapshot(tool, arguments, emit)
}

func (s *canonicalState) prepareBufferedTerminal(stop domain.StopReason, usage *usageInfo, streamErr string, emit func(domain.ModelEvent) bool) error {
	if s.finishSeen {
		return fmt.Errorf("openai provider: duplicate terminal event")
	}
	if err := s.closeOpen(emit); err != nil {
		return err
	}
	s.finishSeen = true
	s.finalStop = stop
	s.bufferedError = streamErr
	s.bufferedUsage = usage
	if streamErr != "" {
		s.bufferedUsage = nil
	}
	return nil
}

func (s *canonicalState) flushBufferedTerminal(emit func(domain.ModelEvent) bool) {
	if s.bufferedError != "" {
		emit(domain.ModelEvent{
			Kind:  domain.ModelEventStreamError,
			Error: s.bufferedError,
		})
	}
	if s.bufferedUsage != nil {
		emit(domain.ModelEvent{
			Kind:         domain.ModelEventUsage,
			InputTokens:  s.bufferedUsage.PromptTokens,
			OutputTokens: s.bufferedUsage.CompletionTokens,
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
	index             int
	id                string
	name              string
	assembledArgs     string
	pendingArgs       string
	itemArguments     string
	sawArgumentEvents bool
	started           bool
	ended             bool
	doneSeen          bool
}

type chatCompletionChunk struct {
	Choices []chatChoice `json:"choices"`
	Usage   *usageInfo   `json:"usage,omitempty"`
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
	Type     string           `json:"type,omitempty"`
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
}

type responsesEventEnvelope struct {
	Type        string               `json:"type,omitempty"`
	Response    *responsesResponse   `json:"response,omitempty"`
	Item        *responsesOutputItem `json:"item,omitempty"`
	OutputIndex *int                 `json:"output_index,omitempty"`
	Delta       string               `json:"delta,omitempty"`
	Arguments   string               `json:"arguments,omitempty"`
	Error       *responsesError      `json:"error,omitempty"`
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
	Error             *responsesError             `json:"error,omitempty"`
	IncompleteDetails *responsesIncompleteDetails `json:"incomplete_details,omitempty"`
}

type responsesIncompleteDetails struct {
	Reason string `json:"reason,omitempty"`
}

type responsesError struct {
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
			PromptTokens:     resp.Usage.InputTokens,
			CompletionTokens: resp.Usage.OutputTokens,
		}
	}
	return &usageInfo{
		PromptTokens:     resp.Usage.PromptTokens,
		CompletionTokens: resp.Usage.CompletionTokens,
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

func responseFailureMessage(eventName string, envelope responsesEventEnvelope) string {
	if envelope.Error != nil && strings.TrimSpace(envelope.Error.Message) != "" {
		return envelope.Error.Message
	}
	if envelope.Response != nil && envelope.Response.Error != nil && strings.TrimSpace(envelope.Response.Error.Message) != "" {
		return envelope.Response.Error.Message
	}
	return fmt.Sprintf("openai provider: %s", eventName)
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
	case "", "null":
		return domain.StopUnknown
	default:
		return domain.StopUnknown
	}
}

type sseEvent struct {
	Event string
	Data  string
}

type sseParser struct {
	reader *bufio.Reader
}

func newSSEParser(r io.Reader) *sseParser {
	return &sseParser{reader: bufio.NewReader(r)}
}

func (p *sseParser) Next() (sseEvent, error) {
	var (
		dataLines []string
		eventName string
	)

	for {
		line, err := p.reader.ReadString('\n')
		eof := errors.Is(err, io.EOF)
		if err != nil && !eof {
			return sseEvent{}, err
		}

		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			if len(dataLines) == 0 {
				if eof {
					return sseEvent{}, io.EOF
				}
				continue
			}
			return sseEvent{Event: eventName, Data: strings.Join(dataLines, "\n")}, nil
		}

		if strings.HasPrefix(line, ":") {
			if eof {
				if len(dataLines) == 0 {
					return sseEvent{}, io.EOF
				}
				return sseEvent{Event: eventName, Data: strings.Join(dataLines, "\n")}, nil
			}
			continue
		}

		field, value, found := strings.Cut(line, ":")
		if !found {
			return sseEvent{}, fmt.Errorf("openai provider: malformed SSE line %q", line)
		}
		if strings.HasPrefix(value, " ") {
			value = value[1:]
		}

		switch field {
		case "data":
			dataLines = append(dataLines, value)
		case "event":
			eventName = value
		case "id", "retry":
		default:
			return sseEvent{}, fmt.Errorf("openai provider: unsupported SSE field %q", field)
		}

		if eof {
			if len(dataLines) == 0 {
				return sseEvent{}, io.EOF
			}
			return sseEvent{Event: eventName, Data: strings.Join(dataLines, "\n")}, nil
		}
	}
}
