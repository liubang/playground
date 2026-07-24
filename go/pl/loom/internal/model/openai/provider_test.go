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
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestToolResultContentKeepsArtifactRefsOutOfModelPayload(t *testing.T) {
	ref := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 1024}
	result := domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: `{"stdout":"bounded"}`},
			{Kind: domain.PartArtifact, Artifact: &ref},
		},
		Metadata: map[string]string{"stdout_artifact_id": ref.ID.String()},
	}
	if got := toolResultContent(result); got != `{"stdout":"bounded"}` {
		t.Fatalf("toolResultContent = %q", got)
	}
}

func TestProviderStreamRequestAdaptationAndAuthorization(t *testing.T) {
	t.Parallel()

	toolCallID := domain.NewToolCallID()
	var sawRequest atomic.Bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sawRequest.Store(true)

		if got := r.Header.Get("Authorization"); got != "Bearer secret-key" {
			t.Fatalf("unexpected Authorization header: %q", got)
		}
		if got := r.Header.Get("Accept"); got != "text/event-stream" {
			t.Fatalf("unexpected Accept header: %q", got)
		}
		if got := r.URL.Path; got != "/v1/chat/completions" {
			t.Fatalf("unexpected request path: %q", got)
		}

		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("ReadAll: %v", err)
		}
		if strings.Contains(string(body), "secret-key") {
			t.Fatal("API key leaked into request body")
		}

		var payload struct {
			Model         string  `json:"model"`
			Stream        bool    `json:"stream"`
			MaxTokens     int64   `json:"max_tokens"`
			Temperature   float64 `json:"temperature"`
			StreamOptions struct {
				IncludeUsage bool `json:"include_usage"`
			} `json:"stream_options"`
			Messages []struct {
				Role       string `json:"role"`
				Content    any    `json:"content"`
				ToolCallID string `json:"tool_call_id"`
				ToolCalls  []struct {
					ID       string `json:"id"`
					Type     string `json:"type"`
					Function struct {
						Name      string `json:"name"`
						Arguments string `json:"arguments"`
					} `json:"function"`
				} `json:"tool_calls"`
			} `json:"messages"`
			Tools []struct {
				Type     string `json:"type"`
				Function struct {
					Name        string         `json:"name"`
					Description string         `json:"description"`
					Parameters  map[string]any `json:"parameters"`
				} `json:"function"`
			} `json:"tools"`
		}
		if err := json.Unmarshal(body, &payload); err != nil {
			t.Fatalf("json.Unmarshal: %v", err)
		}

		if payload.Model != "gpt-test" {
			t.Fatalf("unexpected model: %q", payload.Model)
		}
		if !payload.Stream {
			t.Fatal("expected stream=true")
		}
		if !payload.StreamOptions.IncludeUsage {
			t.Fatal("expected stream_options.include_usage=true")
		}
		if payload.MaxTokens != 128 {
			t.Fatalf("unexpected max_tokens: %d", payload.MaxTokens)
		}
		if payload.Temperature != 0.5 {
			t.Fatalf("unexpected temperature: %v", payload.Temperature)
		}
		if len(payload.Messages) != 4 {
			t.Fatalf("unexpected message count: %d", len(payload.Messages))
		}
		if payload.Messages[0].Role != "system" || payload.Messages[0].Content != "sys" {
			t.Fatalf("unexpected system message: %+v", payload.Messages[0])
		}
		if payload.Messages[1].Role != "user" || payload.Messages[1].Content != "question" {
			t.Fatalf("unexpected user message: %+v", payload.Messages[1])
		}
		if payload.Messages[2].Role != "assistant" || payload.Messages[2].Content != "working" {
			t.Fatalf("unexpected assistant message: %+v", payload.Messages[2])
		}
		if len(payload.Messages[2].ToolCalls) != 1 {
			t.Fatalf("unexpected assistant tool call count: %d", len(payload.Messages[2].ToolCalls))
		}
		if payload.Messages[2].ToolCalls[0].ID != toolCallID.String() {
			t.Fatalf("unexpected tool call id: %q", payload.Messages[2].ToolCalls[0].ID)
		}
		if payload.Messages[2].ToolCalls[0].Function.Name != "read_file" {
			t.Fatalf("unexpected tool call name: %q", payload.Messages[2].ToolCalls[0].Function.Name)
		}
		if payload.Messages[3].Role != "tool" || payload.Messages[3].ToolCallID != toolCallID.String() {
			t.Fatalf("unexpected tool result message: %+v", payload.Messages[3])
		}
		if payload.Messages[3].Content != "file-body" {
			t.Fatalf("unexpected tool result content: %v", payload.Messages[3].Content)
		}
		if len(payload.Tools) != 1 {
			t.Fatalf("unexpected tool definition count: %d", len(payload.Tools))
		}
		if payload.Tools[0].Type != "function" || payload.Tools[0].Function.Name != "read_file" {
			t.Fatalf("unexpected tool definition: %+v", payload.Tools[0])
		}

		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"done\"}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":3}}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 0)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName:   "gpt-test",
		MaxTokens:   128,
		Temperature: 0.5,
		Messages: []domain.Message{
			textMessage(domain.RoleSystem, "sys"),
			textMessage(domain.RoleUser, "question"),
			assistantMessageWithToolCall("working", domain.ToolCall{
				ID:        toolCallID,
				Name:      "read_file",
				Arguments: json.RawMessage(`{"path":"a.txt"}`),
			}),
			toolResultMessageForRequest(domain.ToolResult{
				CallID: toolCallID,
				Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{
					Kind: domain.PartText,
					Text: "file-body",
				}},
			}),
		},
		Tools: []domain.ToolDefinition{{
			Name:        "read_file",
			Description: "Read a file",
			InputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
		}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if !sawRequest.Load() {
		t.Fatal("expected server to receive request")
	}

	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	)
	if got := events[2].TextDelta; got != "done" {
		t.Fatalf("unexpected text delta: %q", got)
	}
	if got := events[4].InputTokens; got != 11 {
		t.Fatalf("unexpected input tokens: %d", got)
	}
	if got := events[4].OutputTokens; got != 3 {
		t.Fatalf("unexpected output tokens: %d", got)
	}
	if got := events[5].StopReason; got != domain.StopEndTurn {
		t.Fatalf("unexpected stop reason: %s", got)
	}
}

func TestProviderStreamResponsesTextContract(t *testing.T) {
	t.Parallel()

	toolCallID := domain.NewToolCallID()
	var sawRequest atomic.Bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sawRequest.Store(true)

		if got := r.Header.Get("Authorization"); got != "Bearer secret-key" {
			t.Fatalf("unexpected Authorization header: %q", got)
		}
		if got := r.URL.Path; got != "/v1/responses" {
			t.Fatalf("unexpected request path: %q", got)
		}

		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("ReadAll: %v", err)
		}
		if strings.Contains(string(body), "secret-key") {
			t.Fatal("API key leaked into request body")
		}

		var payload struct {
			Model           string `json:"model"`
			Stream          bool   `json:"stream"`
			MaxOutputTokens int64  `json:"max_output_tokens"`
			Input           []struct {
				Type    string `json:"type"`
				Role    string `json:"role"`
				CallID  string `json:"call_id"`
				Name    string `json:"name"`
				Output  string `json:"output"`
				Content []struct {
					Type string `json:"type"`
					Text string `json:"text"`
				} `json:"content"`
				Arguments string `json:"arguments"`
			} `json:"input"`
			Tools []struct {
				Type        string         `json:"type"`
				Name        string         `json:"name"`
				Description string         `json:"description"`
				Parameters  map[string]any `json:"parameters"`
			} `json:"tools"`
		}
		if err := json.Unmarshal(body, &payload); err != nil {
			t.Fatalf("json.Unmarshal: %v", err)
		}

		if payload.Model != "glm-5.2" {
			t.Fatalf("unexpected model: %q", payload.Model)
		}
		if !payload.Stream {
			t.Fatal("expected stream=true")
		}
		if payload.MaxOutputTokens != 256 {
			t.Fatalf("unexpected max_output_tokens: %d", payload.MaxOutputTokens)
		}
		if len(payload.Input) != 5 {
			t.Fatalf("unexpected input length: %d", len(payload.Input))
		}
		if payload.Input[0].Type != "message" || payload.Input[0].Role != "system" || payload.Input[0].Content[0].Type != "input_text" || payload.Input[0].Content[0].Text != "sys" {
			t.Fatalf("unexpected system input item: %+v", payload.Input[0])
		}
		if payload.Input[1].Type != "message" || payload.Input[1].Role != "user" || payload.Input[1].Content[0].Text != "question" {
			t.Fatalf("unexpected user input item: %+v", payload.Input[1])
		}
		if payload.Input[2].Type != "message" || payload.Input[2].Role != "assistant" || payload.Input[2].Content[0].Type != "output_text" || payload.Input[2].Content[0].Text != "working" {
			t.Fatalf("unexpected assistant text item: %+v", payload.Input[2])
		}
		if payload.Input[3].Type != "function_call" || payload.Input[3].CallID != toolCallID.String() || payload.Input[3].Name != "read_file" || payload.Input[3].Arguments != `{"path":"a.txt"}` {
			t.Fatalf("unexpected assistant function_call item: %+v", payload.Input[3])
		}
		if payload.Input[4].Type != "function_call_output" || payload.Input[4].CallID != toolCallID.String() || payload.Input[4].Output != "file-body" {
			t.Fatalf("unexpected function_call_output item: %+v", payload.Input[4])
		}
		if len(payload.Tools) != 1 || payload.Tools[0].Type != "function" || payload.Tools[0].Name != "read_file" {
			t.Fatalf("unexpected tool definitions: %+v", payload.Tools)
		}

		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "event: response.created\n")
		fmt.Fprint(w, "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\",\"status\":\"in_progress\"}}\n\n")
		fmt.Fprint(w, "event: response.reasoning_summary_text.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"should-not-leak\"}\n\n")
		fmt.Fprint(w, "event: response.output_text.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hel\"}\n\n")
		fmt.Fprint(w, "event: response.output_text.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n")
		fmt.Fprint(w, "event: response.output_text.done\n")
		fmt.Fprint(w, "data: {\"type\":\"response.output_text.done\"}\n\n")
		fmt.Fprint(w, "event: response.completed\n")
		fmt.Fprint(w, "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"status\":\"completed\",\"usage\":{\"input_tokens\":21,\"output_tokens\":7}}}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProviderWithWireAPI(t, server.URL+"/v1", server.Client(), 0, WireAPIResponses)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "glm-5.2",
		MaxTokens: 256,
		Messages: []domain.Message{
			textMessage(domain.RoleSystem, "sys"),
			textMessage(domain.RoleUser, "question"),
			assistantMessageWithToolCall("working", domain.ToolCall{
				ID:        toolCallID,
				Name:      "read_file",
				Arguments: json.RawMessage(`{"path":"a.txt"}`),
			}),
			toolResultMessageForRequest(domain.ToolResult{
				CallID: toolCallID,
				Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{
					Kind: domain.PartText,
					Text: "file-body",
				}},
			}),
		},
		Tools: []domain.ToolDefinition{{
			Name:        "read_file",
			Description: "Read a file",
			InputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
		}},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if !sawRequest.Load() {
		t.Fatal("expected server to receive request")
	}

	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventReasoningStart,
		domain.ModelEventReasoningDelta,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventReasoningEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	)
	if events[2].ReasoningDelta != "should-not-leak" {
		t.Fatalf("unexpected reasoning delta: %+v", events[2])
	}
	if events[4].TextDelta != "Hel" || events[5].TextDelta != "lo" {
		t.Fatalf("unexpected text deltas: %+v", events)
	}
	for _, evt := range events {
		if strings.Contains(evt.TextDelta, "should-not-leak") {
			t.Fatalf("reasoning text leaked into visible output: %+v", events)
		}
	}
	if events[8].InputTokens != 21 || events[8].OutputTokens != 7 {
		t.Fatalf("unexpected usage: %+v", events[8])
	}
	if events[9].StopReason != domain.StopEndTurn {
		t.Fatalf("unexpected stop reason: %s", events[9].StopReason)
	}
}

func TestProviderStreamResponsesToolCallsAndToolResultRoundTrip(t *testing.T) {
	t.Parallel()

	toolCallID := domain.NewToolCallID()
	var sawRequest atomic.Bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sawRequest.Store(true)
		if got := r.URL.Path; got != "/v1/responses" {
			t.Fatalf("unexpected request path: %q", got)
		}

		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("ReadAll: %v", err)
		}

		var payload struct {
			Input []struct {
				Type      string `json:"type"`
				CallID    string `json:"call_id"`
				Name      string `json:"name"`
				Arguments string `json:"arguments"`
				Output    string `json:"output"`
			} `json:"input"`
		}
		if err := json.Unmarshal(body, &payload); err != nil {
			t.Fatalf("json.Unmarshal: %v", err)
		}
		if len(payload.Input) != 3 {
			t.Fatalf("unexpected input length: %d", len(payload.Input))
		}
		if payload.Input[1].Type != "function_call" || payload.Input[1].CallID != toolCallID.String() || payload.Input[1].Name != "read_file" || payload.Input[1].Arguments != `{"path":"a.txt"}` {
			t.Fatalf("unexpected function_call item: %+v", payload.Input[1])
		}
		if payload.Input[2].Type != "function_call_output" || payload.Input[2].CallID != toolCallID.String() || payload.Input[2].Output != "file-body" {
			t.Fatalf("unexpected function_call_output item: %+v", payload.Input[2])
		}

		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "event: response.created\n")
		fmt.Fprint(w, "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_tool\",\"status\":\"in_progress\"}}\n\n")
		fmt.Fprint(w, "event: response.output_item.added\n")
		fmt.Fprint(w, "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"call_id\":\"call_stream\",\"name\":\"read_file\"}}\n\n")
		fmt.Fprint(w, "event: response.reasoning.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.reasoning.delta\",\"delta\":\"ignore\"}\n\n")
		fmt.Fprint(w, "event: response.function_call_arguments.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":\"{\"}\n\n")
		fmt.Fprint(w, "event: response.function_call_arguments.delta\n")
		fmt.Fprint(w, "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":\"\\\"path\\\":\\\"b.txt\\\"}\"}\n\n")
		fmt.Fprint(w, "event: response.function_call_arguments.done\n")
		fmt.Fprint(w, "data: {\"type\":\"response.function_call_arguments.done\",\"output_index\":0,\"arguments\":\"{\\\"path\\\":\\\"b.txt\\\"}\"}\n\n")
		fmt.Fprint(w, "event: response.output_item.done\n")
		fmt.Fprint(w, "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"call_id\":\"call_stream\",\"name\":\"read_file\"}}\n\n")
		fmt.Fprint(w, "event: response.completed\n")
		fmt.Fprint(w, "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_tool\",\"status\":\"completed\",\"usage\":{\"input_tokens\":10,\"output_tokens\":4}}}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProviderWithWireAPI(t, server.URL+"/v1", server.Client(), 0, WireAPIResponses)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "glm-5.2",
		Messages: []domain.Message{
			textMessage(domain.RoleUser, "use a tool"),
			assistantMessageWithToolCall("", domain.ToolCall{
				ID:        toolCallID,
				Name:      "read_file",
				Arguments: json.RawMessage(`{"path":"a.txt"}`),
			}),
			toolResultMessageForRequest(domain.ToolResult{
				CallID: toolCallID,
				Status: domain.ToolStatusSuccess,
				Content: []domain.ContentPart{{
					Kind: domain.PartText,
					Text: "file-body",
				}},
			}),
		},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if !sawRequest.Load() {
		t.Fatal("expected server to receive request")
	}

	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventToolCallStart,
		domain.ModelEventReasoningStart,
		domain.ModelEventReasoningDelta,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolCallEnd,
		domain.ModelEventReasoningEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	)
	if events[1].ToolID != "call_stream" || events[1].ToolName != "read_file" {
		t.Fatalf("unexpected tool call start: %+v", events[1])
	}
	if events[3].ReasoningDelta != "ignore" {
		t.Fatalf("unexpected reasoning delta: %+v", events[3])
	}
	if events[4].ToolArgs != "{" || events[5].ToolArgs != `"path":"b.txt"}` {
		t.Fatalf("unexpected tool arg deltas: %+v", events)
	}
	if events[6].ToolID != "call_stream" {
		t.Fatalf("unexpected tool call end: %+v", events[6])
	}
	if events[8].InputTokens != 10 || events[8].OutputTokens != 4 {
		t.Fatalf("unexpected usage: %+v", events[8])
	}
	if events[9].StopReason != domain.StopToolUse {
		t.Fatalf("unexpected stop reason: %s", events[9].StopReason)
	}
}

func TestProviderStreamMapsToolCallsAndUsage(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\"}}]}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"path\\\":\\\"a.txt\\\"}\"}}]}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4}}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 0)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "use a tool")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventToolCallStart,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolCallEnd,
		domain.ModelEventUsage,
		domain.ModelEventResponseEnd,
	)
	if events[1].ToolName != "read_file" {
		t.Fatalf("unexpected tool name: %q", events[1].ToolName)
	}
	if events[1].ToolID != "call_1" {
		t.Fatalf("unexpected tool id: %q", events[1].ToolID)
	}
	if events[2].ToolArgs != "{" || events[3].ToolArgs != `"path":"a.txt"}` {
		t.Fatalf("unexpected tool arg chunks: %q %q", events[2].ToolArgs, events[3].ToolArgs)
	}
	if events[6].StopReason != domain.StopToolUse {
		t.Fatalf("unexpected stop reason: %s", events[6].StopReason)
	}
}

func TestProviderStreamBuffersToolArgumentsUntilIDAndNameArrive(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"read_file\",\"arguments\":\"{\"}}]}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_late\",\"function\":{\"arguments\":\"\\\"path\\\":\\\"a.txt\\\"}\"}}]}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 0)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "use tool")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventToolCallStart,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolArgsDelta,
		domain.ModelEventToolCallEnd,
		domain.ModelEventResponseEnd,
	)
	if events[1].ToolID != "call_late" || events[2].ToolArgs != "{" {
		t.Fatalf("unexpected buffered tool events: %+v", events)
	}
}

func TestProviderStreamHTTPStatusError(t *testing.T) {
	t.Parallel()

	var hits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		fmt.Fprint(w, `{"error":{"message":"bad key"}}`)
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 2)
	_, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err == nil {
		t.Fatal("expected status error")
	}
	if hits.Load() != 1 {
		t.Fatalf("expected 1 request, got %d", hits.Load())
	}
	if !strings.Contains(err.Error(), "bad key") {
		t.Fatalf("unexpected error: %v", err)
	}
	if strings.Contains(err.Error(), "secret-key") {
		t.Fatalf("API key leaked into error: %v", err)
	}
}

func TestProviderStreamRetriesHTTP429BeforeStream(t *testing.T) {
	t.Parallel()

	var hits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if hits.Add(1) == 1 {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusTooManyRequests)
			fmt.Fprint(w, `{"error":{"message":"rate limited"}}`)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 1)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if hits.Load() != 2 {
		t.Fatalf("expected 2 requests, got %d", hits.Load())
	}
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventResponseEnd,
	)
}

func TestProviderStreamRetriesRequestFailureBeforeResponse(t *testing.T) {
	t.Parallel()

	var serverHits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		serverHits.Add(1)
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"retry-ok\"}}]}\n\n")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer server.Close()

	flaky := &flakyRoundTripper{
		base: server.Client().Transport,
		fail: 1,
	}
	client := &http.Client{Transport: flaky}
	provider := newTestProvider(t, server, client, 1)

	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if flaky.calls.Load() != 2 {
		t.Fatalf("expected 2 transport attempts, got %d", flaky.calls.Load())
	}
	if serverHits.Load() != 1 {
		t.Fatalf("expected 1 server hit, got %d", serverHits.Load())
	}
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventResponseEnd,
	)
}

func TestProviderStreamDoesNotRetryAfterPartialStreamDisconnect(t *testing.T) {
	t.Parallel()

	var hits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"half\"}}]}\n\n")
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 3)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	if hits.Load() != 1 {
		t.Fatalf("expected 1 request, got %d", hits.Load())
	}
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventTextStart,
		domain.ModelEventTextDelta,
		domain.ModelEventTextEnd,
		domain.ModelEventStreamError,
		domain.ModelEventResponseEnd,
	)
	if events[5].StopReason != domain.StopProviderError {
		t.Fatalf("unexpected stop reason: %s", events[5].StopReason)
	}
	if !strings.Contains(events[4].Error, "[DONE]") {
		t.Fatalf("unexpected stream error: %q", events[4].Error)
	}
}

func TestProviderStreamMalformedJSONYieldsStreamError(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {bad-json\n\n")
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 0)
	stream, err := provider.Stream(context.Background(), domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	events := collectEvents(t, stream)
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventStreamError,
		domain.ModelEventResponseEnd,
	)
	if !strings.Contains(events[1].Error, "malformed chunk JSON") {
		t.Fatalf("unexpected stream error: %q", events[1].Error)
	}
	if events[2].StopReason != domain.StopProviderError {
		t.Fatalf("unexpected stop reason: %s", events[2].StopReason)
	}
}

func TestProviderStreamContextCancelYieldsCancelled(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		<-r.Context().Done()
	}))
	defer server.Close()

	provider := newTestProvider(t, server, server.Client(), 0)
	ctx, cancel := context.WithCancel(context.Background())
	stream, err := provider.Stream(ctx, domain.ModelRequest{
		ModelName: "gpt-test",
		Messages:  []domain.Message{textMessage(domain.RoleUser, "hi")},
	})
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}
	defer stream.Close()

	cancel()
	events := collectEvents(t, stream)
	assertEventKinds(
		t, events,
		domain.ModelEventResponseStart,
		domain.ModelEventStreamError,
		domain.ModelEventResponseEnd,
	)
	if !strings.Contains(events[1].Error, context.Canceled.Error()) {
		t.Fatalf("unexpected stream error: %q", events[1].Error)
	}
	if events[2].StopReason != domain.StopCancelled {
		t.Fatalf("unexpected stop reason: %s", events[2].StopReason)
	}
}

func newTestProvider(t *testing.T, server *httptest.Server, client *http.Client, maxRetries int) *Provider {
	t.Helper()
	return newTestProviderWithWireAPI(t, server.URL, client, maxRetries, WireAPIChatCompletions)
}

func newTestProviderWithWireAPI(t *testing.T, baseURL string, client *http.Client, maxRetries int, wireAPI WireAPI) *Provider {
	t.Helper()

	provider, err := New(Config{
		BaseURL:        baseURL,
		APIKey:         "secret-key",
		HTTPClient:     client,
		WireAPI:        wireAPI,
		MaxRetries:     maxRetries,
		InitialBackoff: time.Millisecond,
		MaxBackoff:     2 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return provider
}

func collectEvents(t *testing.T, stream domain.ModelStream) []domain.ModelEvent {
	t.Helper()

	var events []domain.ModelEvent
	for {
		evt, err := stream.Recv()
		if errors.Is(err, io.EOF) {
			return events
		}
		if err != nil {
			t.Fatalf("Recv: %v", err)
		}
		events = append(events, evt)
	}
}

func assertEventKinds(t *testing.T, events []domain.ModelEvent, want ...domain.ModelEventKind) {
	t.Helper()

	if len(events) != len(want) {
		t.Fatalf("unexpected event count: got=%d want=%d events=%+v", len(events), len(want), events)
	}
	for i, kind := range want {
		if events[i].Kind != kind {
			t.Fatalf("unexpected event kind at %d: got=%s want=%s", i, events[i].Kind, kind)
		}
	}
}

func textMessage(role domain.Role, text string) domain.Message {
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      role,
		CreatedAt: time.Unix(0, 0).UTC(),
		Parts: []domain.ContentPart{{
			Kind: domain.PartText,
			Text: text,
		}},
	}
}

func assistantMessageWithToolCall(text string, call domain.ToolCall) domain.Message {
	parts := make([]domain.ContentPart, 0, 2)
	if text != "" {
		parts = append(parts, domain.ContentPart{
			Kind: domain.PartText,
			Text: text,
		})
	}
	parts = append(parts, domain.ContentPart{
		Kind:     domain.PartToolCall,
		ToolCall: &call,
	})
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleAssistant,
		CreatedAt: time.Unix(0, 0).UTC(),
		Parts:     parts,
	}
}

func toolResultMessageForRequest(result domain.ToolResult) domain.Message {
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleAssistant,
		CreatedAt: time.Unix(0, 0).UTC(),
		Parts: []domain.ContentPart{{
			Kind:       domain.PartToolResult,
			ToolResult: &result,
		}},
	}
}

type flakyRoundTripper struct {
	base  http.RoundTripper
	fail  int32
	calls atomic.Int32
}

func (rt *flakyRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	call := rt.calls.Add(1)
	if call <= rt.fail {
		return nil, errors.New("synthetic dial error")
	}
	return rt.base.RoundTrip(req)
}
