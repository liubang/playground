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
// Created: 2026/07/25

package trace

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestEncodeMessagesContentModes(t *testing.T) {
	callID := domain.NewToolCallID()
	messages := []domain.Message{
		{
			Role: domain.RoleUser,
			Parts: []domain.ContentPart{
				{Kind: domain.PartText, Text: "secret prompt"},
			},
		},
		{
			Role: domain.RoleAssistant,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{
					ID: callID, Name: "edit", Arguments: json.RawMessage(`{"path":"a.go"}`),
				}},
			},
		},
	}

	full := encodeMessages(messages, true)
	if !strings.Contains(full, "secret prompt") || !strings.Contains(full, "a.go") {
		t.Fatalf("full mode should carry text and args:\n%s", full)
	}

	redacted := encodeMessages(messages, false)
	if strings.Contains(redacted, "secret") || strings.Contains(redacted, "a.go") {
		t.Fatalf("redacted mode leaks content:\n%s", redacted)
	}
	if !strings.Contains(redacted, `"bytes"`) {
		t.Fatalf("redacted mode should carry byte sizes:\n%s", redacted)
	}
}

// TestEncodeChatMessagesChatML pins the OpenAI ChatML wire form used for
// langfuse.observation.input/output: the Langfuse formatted view renders
// only this schema and blanks unknown fields like our parts structure.
func TestEncodeChatMessagesChatML(t *testing.T) {
	callID := domain.NewToolCallID()
	messages := []domain.Message{
		{
			Role: domain.RoleUser,
			Parts: []domain.ContentPart{
				{Kind: domain.PartText, Text: "secret prompt"},
			},
		},
		{
			Role: domain.RoleAssistant,
			Parts: []domain.ContentPart{
				{Kind: domain.PartReasoning, Reasoning: &domain.ReasoningContent{Text: "thinking"}},
				{Kind: domain.PartToolCall, ToolCall: &domain.ToolCall{
					ID: callID, Name: "edit", Arguments: json.RawMessage(`{"path":"a.go"}`),
				}},
			},
		},
		{
			Role: domain.RoleUser,
			Parts: []domain.ContentPart{
				{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
					CallID:  callID,
					Status:  domain.ToolStatusSuccess,
					Content: []domain.ContentPart{{Kind: domain.PartText, Text: "edit applied"}},
				}},
			},
		},
	}

	full := encodeChatMessages(messages, true)
	var fullMsgs []map[string]any
	if err := json.Unmarshal([]byte(full), &fullMsgs); err != nil {
		t.Fatalf("chat messages must be valid JSON: %v\n%s", err, full)
	}
	if len(fullMsgs) != 3 {
		t.Fatalf("chat messages = %d, want 3 (user, assistant, tool):\n%s", len(fullMsgs), full)
	}
	if fullMsgs[0]["role"] != "user" || fullMsgs[0]["content"] != "secret prompt" {
		t.Fatalf("user message not in ChatML form: %v", fullMsgs[0])
	}
	if fullMsgs[1]["role"] != "assistant" {
		t.Fatalf("assistant message missing: %v", fullMsgs[1])
	}
	calls, ok := fullMsgs[1]["tool_calls"].([]any)
	if !ok || len(calls) != 1 {
		t.Fatalf("assistant tool_calls missing: %v", fullMsgs[1])
	}
	call := calls[0].(map[string]any)
	if call["id"] != callID.String() || call["type"] != "function" {
		t.Fatalf("tool_call not in ChatML form: %v", call)
	}
	fn := call["function"].(map[string]any)
	if fn["name"] != "edit" || fn["arguments"] != `{"path":"a.go"}` {
		t.Fatalf("tool_call function not in ChatML form: %v", fn)
	}
	if _, leaked := fullMsgs[1]["reasoning"]; leaked {
		t.Fatalf("reasoning must be dropped from ChatML: %v", fullMsgs[1])
	}
	if fullMsgs[2]["role"] != "tool" || fullMsgs[2]["tool_call_id"] != callID.String() ||
		fullMsgs[2]["content"] != "edit applied" {
		t.Fatalf("tool result not in ChatML form: %v", fullMsgs[2])
	}

	redacted := encodeChatMessages(messages, false)
	if strings.Contains(redacted, "secret") || strings.Contains(redacted, "a.go") ||
		strings.Contains(redacted, "edit applied") {
		t.Fatalf("redacted chat mode leaks content:\n%s", redacted)
	}
	if !strings.Contains(redacted, "redacted") {
		t.Fatalf("redacted chat mode should carry placeholders:\n%s", redacted)
	}
	if !strings.Contains(redacted, `"name":"edit"`) {
		t.Fatalf("redacted chat mode should keep tool names:\n%s", redacted)
	}
}

// TestOTelRecorderEmitsRunSpans drives the recorder end-to-end against an
// in-memory exporter and verifies the span tree and Langfuse attributes.
func TestOTelRecorderEmitsRunSpans(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	recorder := &otelRecorder{
		tracer: tp.Tracer("test"), content: true,
		userID: "dev@example.com", release: "0.2.0-dev", costIn: 0.5, costOut: 2.0,
	}

	ctx := context.Background()
	ctx, run := recorder.StartRun(ctx, RunMeta{
		SessionID: "sess-1", RunID: "run-1", Model: "glm-5.2", Prompt: "hello", Workspace: "playground",
	})
	started := time.Now()
	run.RecordGeneration(ctx, GenerationRecord{
		RequestID: "req-1", Turn: 1, Model: "glm-5.2", PromptName: "loom-system", PromptVersion: 3,
		Input: []domain.Message{{
			Role:  domain.RoleUser,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}},
		}},
		Output: domain.Message{
			ID:    domain.NewMessageID(),
			Role:  domain.RoleAssistant,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hi there"}},
		},
		StopReason: "end_turn", InputTokens: 10, OutputTokens: 5,
		StartTime: started, EndTime: started.Add(100 * time.Millisecond),
	})
	run.RecordTool(ctx, ToolRecord{
		CallID: "tc-1", Name: "edit", Risk: "R2", Status: "error", Error: "conflict",
		StartTime: started, EndTime: started.Add(20 * time.Millisecond),
	})
	run.RecordEvent(ctx, "context.compacted", map[string]string{"archived_messages": "71"})
	run.End(RunResult{Outcome: "failed", Error: "boom"})

	if err := tp.ForceFlush(ctx); err != nil {
		t.Fatal(err)
	}
	spans := exporter.GetSpans()
	if len(spans) != 3 {
		t.Fatalf("spans = %d, want 3 (generation, tool, run)", len(spans))
	}

	byName := map[string]tracetest.SpanStub{}
	for _, s := range spans {
		byName[s.Name] = s
	}

	gen, ok := byName["gen_ai.chat"]
	if !ok {
		t.Fatalf("generation span missing: %v", keys(byName))
	}
	assertAttr(t, gen.Attributes, attrObservationType, "generation")
	assertAttr(t, gen.Attributes, attrObservationModel, "glm-5.2")
	assertAttr(t, gen.Attributes, attrGenAIInputTokens, int64(10))
	assertAttr(t, gen.Attributes, attrObservationPromptName, "loom-system")
	input := attrValue(gen.Attributes, attrObservationInput)
	if !strings.Contains(input, `"content":"hello"`) {
		t.Fatalf("generation input must be ChatML (role/content) for the formatted view: %s", input)
	}
	if output := attrValue(gen.Attributes, attrObservationOutput); !strings.Contains(output, `"content":"hi there"`) {
		t.Fatalf("generation output must be ChatML (role/content) for the formatted view: %s", output)
	}
	cost := attrValue(gen.Attributes, attrObservationCost)
	if !strings.Contains(cost, "input") {
		t.Fatalf("generation cost_details missing: %s", cost)
	}

	tool, ok := byName["tool.edit"]
	if !ok {
		t.Fatal("tool span missing")
	}
	assertAttr(t, tool.Attributes, "loom.tool.status", "error")

	root, ok := byName["loom.run"]
	if !ok {
		t.Fatal("run span missing")
	}
	assertAttr(t, root.Attributes, attrTraceSessionID, "sess-1")
	assertAttr(t, root.Attributes, attrTraceInput, "hello")
	assertAttr(t, root.Attributes, attrTraceUserID, "dev@example.com")
	assertAttr(t, root.Attributes, attrTraceRelease, "0.2.0-dev")
	if gen.Parent.TraceID() != root.SpanContext.TraceID() || gen.Parent.SpanID() != root.SpanContext.SpanID() {
		t.Fatal("generation span must parent to the run span")
	}
	if tool.Parent.SpanID() != root.SpanContext.SpanID() {
		t.Fatal("tool span must parent to the run span")
	}
	if len(root.Events) != 1 || root.Events[0].Name != "context.compacted" {
		t.Fatalf("compaction event missing: %+v", root.Events)
	}
	if root.Status.Code.String() != "Error" {
		t.Fatalf("failed run should end with Error status, got %s", root.Status.Code)
	}
}

// TestRedactedRunHygiene pins the privacy contract of LOOM_TRACE_CONTENT=0:
// no workspace path in tags, user id only as an irreversible hash, tool
// errors reduced to their stable code.
func TestRedactedRunHygiene(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	recorder := &otelRecorder{
		tracer: tp.Tracer("test"), content: false,
		userID: "dev@example.com", release: "0.2.0-dev",
	}

	ctx := context.Background()
	ctx, run := recorder.StartRun(ctx, RunMeta{
		SessionID: "sess-1", RunID: "run-1", Model: "m", Prompt: "secret", Workspace: "/Users/dev/secret-repo",
	})
	toolErr := &domain.ToolError{Code: "permission_denied", Message: "tool call denied by policy: /Users/dev/secret-repo/x.go"}
	run.RecordGeneration(ctx, GenerationRecord{
		RequestID: "req-1", Model: "m",
		Input: []domain.Message{{
			Role: domain.RoleAssistant,
			Parts: []domain.ContentPart{{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
				CallID: domain.NewToolCallID(), Status: domain.ToolStatusError, Error: toolErr,
			}}},
		}},
		StartTime: time.Now(), EndTime: time.Now(),
	})
	run.RecordTool(ctx, ToolRecord{
		CallID: "tc-1", Name: "run_cmd", Status: "error", Code: toolErr.Code, Error: toolErr.Message,
		StartTime: time.Now(), EndTime: time.Now(),
	})
	run.End(RunResult{Outcome: "completed"})
	if err := tp.ForceFlush(ctx); err != nil {
		t.Fatal(err)
	}

	spans := exporter.GetSpans()
	byName := map[string]tracetest.SpanStub{}
	for _, s := range spans {
		byName[s.Name] = s
	}

	root := byName["loom.run"]
	for _, kv := range root.Attributes {
		if string(kv.Key) == attrTraceTags {
			for _, tag := range kv.Value.AsStringSlice() {
				if strings.Contains(tag, "secret-repo") || strings.Contains(tag, "/Users/") {
					t.Fatalf("redacted trace leaks workspace path in tags: %v", tag)
				}
			}
		}
		if string(kv.Key) == attrTraceUserID || string(kv.Key) == attrTraceCompatUserID {
			v := kv.Value.Emit()
			if strings.Contains(v, "@") || !strings.HasPrefix(v, "u_") {
				t.Fatalf("redacted user id must be a hash, got %q", v)
			}
		}
	}
	if attrValue(root.Attributes, attrTraceInput) != "" {
		t.Fatal("redacted trace must not carry prompt input")
	}

	gen := byName["gen_ai.chat"]
	input := attrValue(gen.Attributes, attrObservationInput)
	if strings.Contains(input, "secret-repo") {
		t.Fatalf("redacted generation input leaks error message: %s", input)
	}
	if !strings.Contains(input, "permission_denied") {
		t.Fatalf("redacted generation input should carry the error code: %s", input)
	}

	tool := byName["tool.run_cmd"]
	if tool.Status.Description != "permission_denied" {
		t.Fatalf("redacted tool status = %q, want the stable code", tool.Status.Description)
	}
}

// TestFullContentToolError verifies the non-redacted path still reports the
// human-readable error message.
func TestFullContentToolError(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	recorder := &otelRecorder{tracer: tp.Tracer("test"), content: true, userID: "dev@example.com"}
	ctx := context.Background()
	ctx, run := recorder.StartRun(ctx, RunMeta{SessionID: "s", RunID: "r", Workspace: "/ws"})
	run.RecordTool(ctx, ToolRecord{
		CallID: "tc", Name: "edit", Status: "error", Code: "conflict", Error: "file changed on disk",
		StartTime: time.Now(), EndTime: time.Now(),
	})
	run.End(RunResult{Outcome: "completed"})
	if err := tp.ForceFlush(ctx); err != nil {
		t.Fatal(err)
	}
	for _, s := range exporter.GetSpans() {
		if s.Name == "tool.edit" {
			if s.Status.Description != "file changed on disk" {
				t.Fatalf("full-content tool status = %q", s.Status.Description)
			}
			return
		}
	}
	t.Fatal("tool span missing")
}

// TestGenAISystemFromModel pins the model-name heuristic behind the
// gen_ai.system attribute (REVIEW M31): claude/anthropic → "anthropic",
// gpt/openai → "openai", anything else → "unknown" instead of the old
// hardcoded "openai".
func TestGenAISystemFromModel(t *testing.T) {
	cases := map[string]string{
		"claude-sonnet-4-5":       "anthropic",
		"anthropic/claude-opus-4": "anthropic",
		"Claude-3-Haiku":          "anthropic",
		"gpt-5":                   "openai",
		"openai/gpt-4o":           "openai",
		"glm-5.2":                 "unknown",
		"":                        "unknown",
	}
	for model, want := range cases {
		if got := genAISystem(model); got != want {
			t.Fatalf("genAISystem(%q) = %q, want %q", model, got, want)
		}
	}
}

// TestGenerationSpanGenAISystem verifies the recorded generation span
// carries the heuristic system value, not the old hardcoded "openai"
// (REVIEW M31).
func TestGenerationSpanGenAISystem(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	recorder := &otelRecorder{tracer: tp.Tracer("test"), content: true, userID: "u"}
	ctx := context.Background()
	ctx, run := recorder.StartRun(ctx, RunMeta{SessionID: "s", RunID: "r", Model: "claude-sonnet-4-5"})
	now := time.Now()
	run.RecordGeneration(ctx, GenerationRecord{
		RequestID: "req-1", Model: "claude-sonnet-4-5", StartTime: now, EndTime: now,
	})
	run.End(RunResult{Outcome: "completed"})
	if err := tp.ForceFlush(ctx); err != nil {
		t.Fatal(err)
	}
	for _, s := range exporter.GetSpans() {
		if s.Name == "gen_ai.chat" {
			assertAttr(t, s.Attributes, attrGenAISystem, "anthropic")
			return
		}
	}
	t.Fatal("generation span missing")
}

// TestExportErrorHandlerFanOut is the regression test for REVIEW M31:
// otel.SetErrorHandler is process-global, so Setup must subscribe through
// the singleton fan-out handler instead of overwriting the global — two
// live providers both receive export errors, and Shutdown unsubscribes
// only its own logger.
func TestExportErrorHandlerFanOut(t *testing.T) {
	var buf1, buf2 bytes.Buffer
	cfg := Config{Enabled: true, Host: "http://127.0.0.1:1", PublicKey: "pk", SecretKey: "sk"}
	cfg.Logger = slog.New(slog.NewTextHandler(&buf1, nil))
	p1, err := Setup(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Logger = slog.New(slog.NewTextHandler(&buf2, nil))
	p2, err := Setup(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}

	otel.Handle(errors.New("export boom 1"))
	if !strings.Contains(buf1.String(), "export boom 1") {
		t.Fatalf("first provider's logger must see the export error:\n%s", buf1.String())
	}
	if !strings.Contains(buf2.String(), "export boom 1") {
		t.Fatalf("second Setup must not hijack the first provider's export errors:\n%s", buf2.String())
	}

	// Shutdown flushes buffered spans against the unreachable endpoint;
	// those export errors are expected and irrelevant here.
	_ = p1.Shutdown(context.Background())
	buf1.Reset()
	buf2.Reset()
	otel.Handle(errors.New("export boom 2"))
	if strings.Contains(buf1.String(), "export boom 2") {
		t.Fatalf("shut-down provider must be unsubscribed:\n%s", buf1.String())
	}
	if !strings.Contains(buf2.String(), "export boom 2") {
		t.Fatalf("live provider must still see export errors:\n%s", buf2.String())
	}
	_ = p2.Shutdown(context.Background())
}

func TestNoopRecorderIsSafe(t *testing.T) {
	ctx, run := Noop().StartRun(context.Background(), RunMeta{SessionID: "s"})
	run.RecordGeneration(ctx, GenerationRecord{})
	run.RecordTool(ctx, ToolRecord{})
	run.RecordEvent(ctx, "x", nil)
	run.End(RunResult{Error: "ignored"})
}

func attrValue(attrs []attribute.KeyValue, key string) string {
	for _, kv := range attrs {
		if string(kv.Key) == key {
			return kv.Value.Emit()
		}
	}
	return ""
}

func assertAttr[V comparable](t *testing.T, attrs []attribute.KeyValue, key string, want V) {
	t.Helper()
	for _, kv := range attrs {
		if string(kv.Key) == key {
			got, ok := kv.Value.AsInterface().(V)
			if !ok {
				t.Fatalf("attribute %s type %T, want %T", key, kv.Value.AsInterface(), want)
			}
			if got != want {
				t.Fatalf("attribute %s = %v, want %v", key, got, want)
			}
			return
		}
	}
	t.Fatalf("attribute %s missing", key)
}

func keys(m map[string]tracetest.SpanStub) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}

func TestTruncateContentAlwaysValidUTF8(t *testing.T) {
	cases := []struct {
		name string
		in   string
	}{
		{"invalid bytes", "abc\xFF\xFEdef"},
		{"lone continuation", "x\x80y"},
		{"oversized with cut inside a rune", strings.Repeat("a", maxAttributeContent) + "完整保证截断落在字符中间" + strings.Repeat("b", 100)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := truncateContent(tc.in)
			if !utf8.ValidString(got) {
				t.Fatalf("truncateContent() produced invalid UTF-8")
			}
		})
	}
	// Sanitization must not alter already-valid input.
	if got := truncateContent("有效内容不变"); got != "有效内容不变" {
		t.Fatalf("truncateContent() = %q, want input unchanged", got)
	}
}
