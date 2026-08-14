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

package anthropic

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func sseServer(t *testing.T, events string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, events)
	}))
}

func collectEvents(t *testing.T, stream domain.ModelStream) []domain.ModelEvent {
	t.Helper()
	defer stream.Close()
	var events []domain.ModelEvent
	for {
		evt, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return events
			}
			t.Fatalf("Recv: %v", err)
		}
		events = append(events, evt)
	}
}

func eventKinds(events []domain.ModelEvent) []domain.ModelEventKind {
	kinds := make([]domain.ModelEventKind, len(events))
	for i, evt := range events {
		kinds[i] = evt.Kind
	}
	return kinds
}

func findEvent(events []domain.ModelEvent, kind domain.ModelEventKind) (domain.ModelEvent, bool) {
	for _, evt := range events {
		if evt.Kind == kind {
			return evt, true
		}
	}
	return domain.ModelEvent{}, false
}

// Regression (REVIEW M3): a base_url ending in "/v1" (OpenAI-style) used
// to produce /v1/v1/messages.
func TestNewHandlesV1SuffixInBaseURL(t *testing.T) {
	t.Parallel()

	var path string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path = r.URL.Path
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}\n\n")
	}))
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL + "/v1", APIKey: "sk-ant"})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test", MaxTokens: 16,
		Messages: []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	collectEvents(t, stream)
	if path != "/v1/messages" {
		t.Fatalf("path = %q, want /v1/messages", path)
	}
}

// Regression (REVIEW M5): an unknown SSE event type used to kill the whole
// stream — Anthropic adding any new event would break every session. Unknown
// events must be ignored for forward compatibility.
func TestStreamIgnoresUnknownEventTypes(t *testing.T) {
	t.Parallel()

	server := sseServer(t, "event: future_kind\ndata: {\"type\":\"future_kind\",\"x\":1}\n\n"+
		"event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n"+
		"event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n")
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL, APIKey: "sk-ant"})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test", MaxTokens: 16,
		Messages: []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	events := collectEvents(t, stream)
	if _, ok := findEvent(events, domain.ModelEventResponseStart); !ok {
		t.Fatalf("response_start missing after unknown event: %v", eventKinds(events))
	}
	if _, ok := findEvent(events, domain.ModelEventStreamError); ok {
		t.Fatalf("unknown event must not produce a stream error: %v", eventKinds(events))
	}
}

func TestStreamRequestAdaptationAndHeaders(t *testing.T) {
	t.Parallel()

	toolCallID := domain.NewToolCallID()
	reasoningSig := "sig-abc"

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("x-api-key"); got != "sk-ant-secret" {
			t.Errorf("x-api-key = %q", got)
		}
		if got := r.Header.Get("anthropic-version"); got != defaultVersion {
			t.Errorf("anthropic-version = %q", got)
		}
		if got := r.Header.Get("Authorization"); got != "" {
			t.Errorf("unexpected Authorization header with x-api-key auth: %q", got)
		}
		if got := r.URL.Path; got != "/v1/messages" {
			t.Errorf("path = %q", got)
		}

		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Errorf("ReadAll: %v", err)
		}
		if strings.Contains(string(body), "sk-ant-secret") {
			t.Error("API key leaked into request body")
		}

		var payload struct {
			Model       string   `json:"model"`
			MaxTokens   int64    `json:"max_tokens"`
			Stream      bool     `json:"stream"`
			System      string   `json:"system"`
			Temperature *float64 `json:"temperature"`
			Thinking    struct {
				Type         string `json:"type"`
				BudgetTokens int64  `json:"budget_tokens"`
			} `json:"thinking"`
			Messages []struct {
				Role    string           `json:"role"`
				Content []map[string]any `json:"content"`
			} `json:"messages"`
			Tools []struct {
				Name        string         `json:"name"`
				Description string         `json:"description"`
				InputSchema map[string]any `json:"input_schema"`
			} `json:"tools"`
		}
		if err := json.Unmarshal(body, &payload); err != nil {
			t.Errorf("json.Unmarshal: %v", err)
		}

		if payload.Model != "claude-test" {
			t.Errorf("model = %q", payload.Model)
		}
		if payload.MaxTokens != 4096 {
			t.Errorf("max_tokens = %d", payload.MaxTokens)
		}
		if !payload.Stream {
			t.Error("expected stream=true")
		}
		if payload.System != "you are helpful" {
			t.Errorf("system = %q", payload.System)
		}
		if payload.Temperature != nil {
			t.Errorf("temperature must be omitted while thinking is enabled, got %v", *payload.Temperature)
		}
		if payload.Thinking.Type != "enabled" || payload.Thinking.BudgetTokens != 2000 {
			t.Errorf("thinking = %+v", payload.Thinking)
		}
		if len(payload.Tools) != 1 || payload.Tools[0].Name != "read_file" {
			t.Errorf("tools = %+v", payload.Tools)
		}
		if payload.Tools[0].InputSchema["type"] != "object" {
			t.Errorf("input_schema = %+v", payload.Tools[0].InputSchema)
		}

		// Transcript shape: user text, assistant reasoning+text+tool_use,
		// user tool_result.
		if len(payload.Messages) != 3 {
			t.Fatalf("messages = %+v", payload.Messages)
		}
		if payload.Messages[0].Role != "user" || payload.Messages[0].Content[0]["type"] != "text" {
			t.Errorf("messages[0] = %+v", payload.Messages[0])
		}
		assistant := payload.Messages[1]
		if assistant.Role != "assistant" || len(assistant.Content) != 3 {
			t.Fatalf("assistant message = %+v", assistant)
		}
		if assistant.Content[0]["type"] != "thinking" || assistant.Content[0]["thinking"] != "pondering" || assistant.Content[0]["signature"] != reasoningSig {
			t.Errorf("thinking block = %+v", assistant.Content[0])
		}
		if assistant.Content[1]["type"] != "text" || assistant.Content[1]["text"] != "let me check" {
			t.Errorf("text block = %+v", assistant.Content[1])
		}
		if assistant.Content[2]["type"] != "tool_use" || assistant.Content[2]["id"] != toolCallID.String() || assistant.Content[2]["name"] != "read_file" {
			t.Errorf("tool_use block = %+v", assistant.Content[2])
		}
		input, ok := assistant.Content[2]["input"].(map[string]any)
		if !ok || input["path"] != "/tmp/x" {
			t.Errorf("tool_use input = %+v", assistant.Content[2]["input"])
		}
		result := payload.Messages[2]
		if result.Role != "user" || result.Content[0]["type"] != "tool_result" || result.Content[0]["tool_use_id"] != toolCallID.String() {
			t.Errorf("tool_result = %+v", result)
		}
		if result.Content[0]["is_error"] != false {
			t.Errorf("is_error = %v", result.Content[0]["is_error"])
		}

		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, "event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"+
			"event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"+
			"event: content_block_stop\ndata: {\"index\":0}\n\n"+
			"event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":5}}\n\n"+
			"event: message_stop\ndata: {}\n\n")
	}))
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL, APIKey: "sk-ant-secret"})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName:   "claude-test",
		MaxTokens:   4096,
		Temperature: 0.7,
		Reasoning:   domain.ReasoningSpec{BudgetTokens: 2000},
		Messages: []domain.Message{
			{Role: domain.RoleSystem, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "you are helpful"}}},
			{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}},
			{Role: domain.RoleAssistant, Parts: []domain.ContentPart{
				{Kind: domain.PartReasoning, Reasoning: &domain.ReasoningContent{Text: "pondering", Signature: reasoningSig}},
				{Kind: domain.PartText, Text: "let me check"},
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{ID: toolCallID, Name: "read_file", Arguments: json.RawMessage(`{"path":"/tmp/x"}`)}},
				{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
					CallID:  toolCallID,
					Status:  domain.ToolStatusSuccess,
					Content: []domain.ContentPart{{Kind: domain.PartText, Text: "file contents"}},
				}},
			}},
		},
		Tools: []domain.ToolDefinition{{
			Name:        "read_file",
			Description: "read a file",
			InputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}}}`),
		}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	events := collectEvents(t, stream)
	want := []domain.ModelEventKind{
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	}
	got := eventKinds(events)
	if len(got) != len(want) {
		t.Fatalf("kinds = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("kinds = %v, want %v", got, want)
		}
	}
	usage, _ := findEvent(events, domain.ModelEventUsage)
	if usage.InputTokens != 10 || usage.OutputTokens != 5 {
		t.Fatalf("usage = %+v", usage)
	}
	end, _ := findEvent(events, domain.ModelEventResponseEnd)
	if end.StopReason != domain.StopEndTurn {
		t.Fatalf("stop = %q", end.StopReason)
	}
}

func TestStreamThinkingAndToolUseEvents(t *testing.T) {
	t.Parallel()

	server := sseServer(t,
		"event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":3,\"output_tokens\":1}}}\n\n"+
			"event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"deep \"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"thoughts\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig-1\"}}\n\n"+
			"event: content_block_stop\ndata: {\"index\":0}\n\n"+
			"event: content_block_start\ndata: {\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01X\",\"name\":\"run_cmd\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"cmd\\\":\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"ls\\\"}\"}}\n\n"+
			"event: content_block_stop\ndata: {\"index\":1}\n\n"+
			"event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":20}}\n\n"+
			"event: message_stop\ndata: {}\n\n")
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	events := collectEvents(t, stream)
	want := []domain.ModelEventKind{
		domain.ModelEventResponseStart,
		domain.ModelEventReasoningStart,
		domain.ModelEventReasoningDelta,
		domain.ModelEventReasoningDelta,
		domain.ModelEventReasoningEnd,
		domain.ModelEventToolCallStart,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolCallEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	}
	got := eventKinds(events)
	if len(got) != len(want) {
		t.Fatalf("kinds = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("kinds = %v, want %v", got, want)
		}
	}

	reasoningEnd, _ := findEvent(events, domain.ModelEventReasoningEnd)
	if reasoningEnd.ReasoningSignature != "sig-1" || reasoningEnd.ReasoningRedacted {
		t.Fatalf("reasoningEnd = %+v", reasoningEnd)
	}
	toolStart, _ := findEvent(events, domain.ModelEventToolCallStart)
	if toolStart.ToolID != "toolu_01X" || toolStart.ToolName != "run_cmd" || toolStart.ToolIndex != 1 {
		t.Fatalf("toolStart = %+v", toolStart)
	}
	end, _ := findEvent(events, domain.ModelEventResponseEnd)
	if end.StopReason != domain.StopToolUse {
		t.Fatalf("stop = %q", end.StopReason)
	}
}

func TestStreamRedactedThinking(t *testing.T) {
	t.Parallel()

	server := sseServer(t,
		"event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n"+
			"event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"opaque-blob\"}}\n\n"+
			"event: content_block_stop\ndata: {\"index\":0}\n\n"+
			"event: content_block_start\ndata: {\"index\":1,\"content_block\":{\"type\":\"text\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}\n\n"+
			"event: content_block_stop\ndata: {\"index\":1}\n\n"+
			"event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":2}}\n\n"+
			"event: message_stop\ndata: {}\n\n")
	defer server.Close()

	provider, _ := New(Config{BaseURL: server.URL})
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	events := collectEvents(t, stream)
	reasoningEnd, ok := findEvent(events, domain.ModelEventReasoningEnd)
	if !ok {
		t.Fatalf("no reasoning_end in %v", eventKinds(events))
	}
	if !reasoningEnd.ReasoningRedacted || reasoningEnd.ReasoningSignature != "opaque-blob" {
		t.Fatalf("reasoningEnd = %+v", reasoningEnd)
	}
}

func TestStreamErrorEvent(t *testing.T) {
	t.Parallel()

	server := sseServer(t,
		"event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n"+
			"event: error\ndata: {\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n")
	defer server.Close()

	provider, _ := New(Config{BaseURL: server.URL})
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	events := collectEvents(t, stream)
	streamErr, ok := findEvent(events, domain.ModelEventStreamError)
	if !ok || !strings.Contains(streamErr.Error, "Overloaded") {
		t.Fatalf("events = %+v", events)
	}
	end, _ := findEvent(events, domain.ModelEventResponseEnd)
	if end.StopReason != domain.StopProviderError {
		t.Fatalf("stop = %q", end.StopReason)
	}
}

func TestStreamPrematureEOF(t *testing.T) {
	t.Parallel()

	server := sseServer(t,
		"event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n"+
			"event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"+
			"event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"cut\"}}\n\n")
	defer server.Close()

	provider, _ := New(Config{BaseURL: server.URL})
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	events := collectEvents(t, stream)
	// The dangling text block must be closed before the error surfaces.
	want := []domain.ModelEventKind{
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventStreamError,
		domain.ModelEventResponseEnd,
	}
	got := eventKinds(events)
	if len(got) != len(want) {
		t.Fatalf("kinds = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("kinds = %v, want %v", got, want)
		}
	}
}

func TestMarshalNonLeadingSystemDowngraded(t *testing.T) {
	t.Parallel()

	systemBlocks, messages, err := toAnthropicMessages([]domain.Message{
		{Role: domain.RoleSystem, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "prompt"}}},
		{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "one"}}},
		{Role: domain.RoleSystem, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "compaction marker"}}},
	})
	if err != nil {
		t.Fatalf("toAnthropicMessages: %v", err)
	}
	if len(systemBlocks) != 1 || systemBlocks[0]["text"] != "prompt" {
		t.Fatalf("system = %+v", systemBlocks)
	}
	if len(messages) != 1 {
		t.Fatalf("messages = %+v (consecutive user messages must merge)", messages)
	}
	content := messages[0]["content"].([]map[string]any)
	if len(content) != 2 || content[1]["text"] != "compaction marker" {
		t.Fatalf("content = %+v", content)
	}
}

func TestMarshalUnsignedReasoningDropped(t *testing.T) {
	t.Parallel()

	sink := &messageSink{}
	msg := domain.Message{Role: domain.RoleAssistant, Parts: []domain.ContentPart{
		{Kind: domain.PartReasoning, Reasoning: &domain.ReasoningContent{Text: "no signature"}},
		{Kind: domain.PartReasoning, Reasoning: &domain.ReasoningContent{Signature: "blob", Redacted: true}},
	}}
	if err := appendAssistant(msg, sink); err != nil {
		t.Fatalf("appendAssistant: %v", err)
	}
	if len(sink.out) != 1 {
		t.Fatalf("out = %+v (unsigned reasoning must be dropped)", sink.out)
	}
	block := sink.out[0]["content"].([]map[string]any)[0]
	if block["type"] != "redacted_thinking" || block["data"] != "blob" {
		t.Fatalf("block = %+v", block)
	}
}

// A tool_result inside an assistant message may only merge into a preceding
// user message that already consists of tool_result blocks; merging into a
// user TEXT message would put the tool_result ahead of its tool_use on the
// wire — a guaranteed API 400 (REVIEW M30).
func TestAppendAssistantToolResultMergeGuard(t *testing.T) {
	t.Parallel()

	result := domain.ToolResult{
		CallID:  domain.NewToolCallID(),
		Status:  domain.ToolStatusSuccess,
		Content: []domain.ContentPart{{Kind: domain.PartText, Text: "ok"}},
	}
	msg := domain.Message{Role: domain.RoleAssistant, Parts: []domain.ContentPart{
		{Kind: domain.PartToolResult, ToolResult: &result},
	}}

	// Preceding user text message → the merge must be rejected.
	sink := &messageSink{}
	sink.append(string(domain.RoleUser), map[string]any{"type": "text", "text": "hello"})
	if err := appendAssistant(msg, sink); err == nil {
		t.Fatal("tool_result merging into a user text message must fail")
	}

	// Preceding user message of pure tool_results (parallel results) → merges.
	sink = &messageSink{}
	sink.append(string(domain.RoleAssistant), map[string]any{"type": "tool_use", "id": "x", "name": "t", "input": map[string]any{}})
	if err := sink.appendToolResult(toolResultBlock(result)); err != nil {
		t.Fatalf("first tool_result: %v", err)
	}
	if err := appendAssistant(msg, sink); err != nil {
		t.Fatalf("parallel tool_result merge: %v", err)
	}
	content := sink.out[1]["content"].([]map[string]any)
	if len(content) != 2 || content[0]["type"] != "tool_result" || content[1]["type"] != "tool_result" {
		t.Fatalf("content = %+v, want two merged tool_result blocks", content)
	}

	// Preceding assistant message → a fresh user message is valid wire.
	sink = &messageSink{}
	sink.append(string(domain.RoleAssistant), map[string]any{"type": "tool_use", "id": "x", "name": "t", "input": map[string]any{}})
	if err := appendAssistant(msg, sink); err != nil {
		t.Fatalf("tool_result after assistant tool_use: %v", err)
	}
	if len(sink.out) != 2 || sink.out[1]["role"] != string(domain.RoleUser) {
		t.Fatalf("out = %+v, want a new user message", sink.out)
	}
}

func TestThinkingBudgetDerivation(t *testing.T) {
	t.Parallel()

	cases := []struct {
		name      string
		spec      domain.ReasoningSpec
		maxTokens int64
		want      int64
		wantErr   bool
	}{
		{"disabled zero", domain.ReasoningSpec{}, 8192, 0, false},
		{"disabled off", domain.ReasoningSpec{Effort: domain.ReasoningEffortOff}, 8192, 0, false},
		{"low", domain.ReasoningSpec{Effort: domain.ReasoningEffortLow}, 8192, 1024, false},
		{"medium", domain.ReasoningSpec{Effort: domain.ReasoningEffortMedium}, 8192, 2730, false},
		{"high", domain.ReasoningSpec{Effort: domain.ReasoningEffortHigh}, 8192, 5461, false},
		{"explicit wins", domain.ReasoningSpec{Effort: domain.ReasoningEffortLow, BudgetTokens: 5000}, 8192, 5000, false},
		{"explicit floor", domain.ReasoningSpec{BudgetTokens: 10}, 8192, 1024, false},
		{"budget too large", domain.ReasoningSpec{BudgetTokens: 8192}, 8192, 0, true},
		{"tiny max_tokens", domain.ReasoningSpec{Effort: domain.ReasoningEffortHigh}, 1000, 0, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := thinkingBudgetFor(tc.spec, tc.maxTokens)
			if (err != nil) != tc.wantErr {
				t.Fatalf("err = %v, wantErr %v", err, tc.wantErr)
			}
			if got != tc.want {
				t.Fatalf("budget = %d, want %d", got, tc.want)
			}
		})
	}
}

func TestStreamBearerAuth(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("Authorization"); got != "Bearer gw-token" {
			t.Errorf("Authorization = %q", got)
		}
		if got := r.Header.Get("x-api-key"); got != "" {
			t.Errorf("unexpected x-api-key header: %q", got)
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, "event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n"+
			"event: message_delta\ndata: {\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":1}}\n\n"+
			"event: message_stop\ndata: {}\n\n")
	}))
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL, APIKey: "gw-token", AuthType: AuthTypeBearer})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	events := collectEvents(t, stream)
	end, ok := findEvent(events, domain.ModelEventResponseEnd)
	if !ok || end.StopReason != domain.StopEndTurn {
		t.Fatalf("events = %+v", events)
	}
}

func TestNewRejectsBadAuthType(t *testing.T) {
	t.Parallel()
	if _, err := New(Config{BaseURL: "https://a.com", AuthType: "digest"}); err == nil {
		t.Fatal("expected error for unknown auth type")
	}
}

func TestStreamErrorStatus(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = fmt.Fprint(w, `{"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}}`)
	}))
	defer server.Close()

	provider, _ := New(Config{BaseURL: server.URL, MaxRetries: 0})
	_, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	if err == nil {
		t.Fatal("expected error")
	}
	if !strings.Contains(err.Error(), "invalid x-api-key") {
		t.Fatalf("err = %v", err)
	}
}

// TestStreamClassifiesRateLimit pins the error-mapping contract: a 429
// surfaces as a retryable domain.ErrRateLimited (not an opaque internal
// error), so the agent loop can wait out the gateway's rate window
// instead of killing the run.
func TestStreamClassifiesRateLimit(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusTooManyRequests)
		_, _ = fmt.Fprint(w, `{"type":"error","error":{"type":"rate_limit_error","message":"requests per minute exceeded"}}`)
	}))
	defer server.Close()

	provider, _ := New(Config{BaseURL: server.URL, MaxRetries: 0})
	_, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test",
		Messages:  []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "go"}}}},
	})
	var ae *domain.AgentError
	if !errors.As(err, &ae) {
		t.Fatalf("err = %v, want *domain.AgentError", err)
	}
	if ae.Code != domain.ErrRateLimited || !ae.Retryable {
		t.Fatalf("err = [%s] retryable=%v, want [rate_limited] retryable=true", ae.Code, ae.Retryable)
	}
	if !strings.Contains(err.Error(), "requests per minute exceeded") {
		t.Fatalf("err = %v, want the provider message preserved", err)
	}
}

func TestToolResultBlockWithImage(t *testing.T) {
	result := domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: "image: shot.png"},
			{Kind: domain.PartImage, Image: &domain.ImageContent{MediaType: "image/png", Data: "cG5n"}},
		},
	}
	block := toolResultBlock(result)
	content, ok := block["content"].([]map[string]any)
	if !ok {
		t.Fatalf("content type = %T, want []map[string]any", block["content"])
	}
	if len(content) != 2 || content[0]["type"] != "text" || content[1]["type"] != "image" {
		t.Fatalf("content = %+v", content)
	}
	source, _ := content[1]["source"].(map[string]any)
	if source["type"] != "base64" || source["media_type"] != "image/png" || source["data"] != "cG5n" {
		t.Fatalf("image source = %+v", source)
	}
}

func TestUserMessageBlocksWithImage(t *testing.T) {
	msg := domain.Message{
		Role: domain.RoleUser,
		Parts: []domain.ContentPart{
			{Kind: domain.PartText, Text: "look"},
			{Kind: domain.PartImage, Image: &domain.ImageContent{MediaType: "image/webp", Data: "d2VicA=="}},
		},
	}
	blocks, err := userMessageBlocks(msg)
	if err != nil {
		t.Fatalf("userMessageBlocks() error = %v", err)
	}
	if len(blocks) != 2 || blocks[0]["type"] != "text" || blocks[1]["type"] != "image" {
		t.Fatalf("blocks = %+v", blocks)
	}

	// Pure text produces exactly one text block.
	textOnly, err := userMessageBlocks(domain.Message{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}})
	if err != nil {
		t.Fatalf("userMessageBlocks() error = %v", err)
	}
	if len(textOnly) != 1 || textOnly[0]["type"] != "text" || textOnly[0]["text"] != "hi" {
		t.Fatalf("text-only blocks = %+v", textOnly)
	}
}

func TestMarshalLeadingSystemBlocksCarryCacheControl(t *testing.T) {
	t.Parallel()

	body, err := marshalRequest(domain.ModelRequest{
		ModelName: "claude-test", MaxTokens: 16,
		Messages: []domain.Message{
			{
				Role:     domain.RoleSystem,
				Parts:    []domain.ContentPart{{Kind: domain.PartText, Text: "static part"}},
				Metadata: map[string]string{domain.MetadataPromptCache: domain.PromptCacheEphemeral},
			},
			{Role: domain.RoleSystem, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "dynamic part"}}},
			{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}},
		},
	})
	if err != nil {
		t.Fatalf("marshalRequest: %v", err)
	}
	var payload map[string]any
	if err := json.Unmarshal(body, &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	system, ok := payload["system"].([]any)
	if !ok || len(system) != 2 {
		t.Fatalf("system = %+v, want two leading system blocks", payload["system"])
	}
	first := system[0].(map[string]any)
	cc, ok := first["cache_control"].(map[string]any)
	if !ok || cc["type"] != "ephemeral" {
		t.Fatalf("static block missing cache_control: %+v", first)
	}
	second := system[1].(map[string]any)
	if second["cache_control"] != nil {
		t.Fatalf("dynamic block must not carry cache_control: %+v", second)
	}
}

func TestMarshalSingleUnmarkedSystemStaysString(t *testing.T) {
	t.Parallel()

	body, err := marshalRequest(domain.ModelRequest{
		ModelName: "claude-test", MaxTokens: 16,
		Messages: []domain.Message{
			{Role: domain.RoleSystem, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "prompt"}}},
			{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}},
		},
	})
	if err != nil {
		t.Fatalf("marshalRequest: %v", err)
	}
	var payload map[string]any
	if err := json.Unmarshal(body, &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload["system"] != "prompt" {
		t.Fatalf("system = %+v, want the plain-string form", payload["system"])
	}
}

func TestStreamReportsCachedInputTokens(t *testing.T) {
	t.Parallel()

	server := sseServer(t,
		"event: message_start\ndata: {\"message\":{\"usage\":{\"input_tokens\":100,\"output_tokens\":1,\"cache_read_input_tokens\":80,\"cache_creation_input_tokens\":20}}}\n\n"+
			"event: message_stop\ndata: {}\n\n")
	defer server.Close()

	provider, err := New(Config{BaseURL: server.URL, APIKey: "sk-ant"})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "claude-test", MaxTokens: 16,
		Messages: []domain.Message{{Role: domain.RoleUser, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}}}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	events := collectEvents(t, stream)
	usage, ok := findEvent(events, domain.ModelEventUsage)
	if !ok {
		t.Fatal("usage event missing")
	}
	if usage.InputTokens != 100 || usage.CachedInputTokens != 80 {
		t.Fatalf("usage = %+v, want input=100 cached=80", usage)
	}
}
