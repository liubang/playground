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
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

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
	if !strings.Contains(input, "hello") {
		t.Fatalf("generation input missing messages: %s", input)
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
