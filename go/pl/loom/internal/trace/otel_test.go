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

	"go.opentelemetry.io/otel/attribute"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestConfigFromEnv(t *testing.T) {
	t.Run("disabled when incomplete", func(t *testing.T) {
		cfg := ConfigFromEnv()
		if cfg.Enabled {
			t.Fatalf("enabled without full config: %+v", cfg)
		}
	})

	t.Run("loom variables take precedence", func(t *testing.T) {
		t.Setenv("LOOM_LANGFUSE_HOST", "http://loom-host:3100/")
		t.Setenv("LOOM_LANGFUSE_PUBLIC_KEY", "pk-loom")
		t.Setenv("LOOM_LANGFUSE_SECRET_KEY", "sk-loom")
		t.Setenv("LANGFUSE_HOST", "http://std-host:3100")
		cfg := ConfigFromEnv()
		if !cfg.Enabled {
			t.Fatal("expected enabled with loom variables")
		}
		if cfg.Host != "http://loom-host:3100" {
			t.Fatalf("host = %q, want loom value with trailing slash trimmed", cfg.Host)
		}
		if !cfg.IncludeContent {
			t.Fatal("content should default to included")
		}
	})

	t.Run("standard langfuse variables as fallback", func(t *testing.T) {
		t.Setenv("LANGFUSE_HOST", "http://std-host:3100")
		t.Setenv("LANGFUSE_PUBLIC_KEY", "pk-std")
		t.Setenv("LANGFUSE_SECRET_KEY", "sk-std")
		t.Setenv("LOOM_TRACE_CONTENT", "0")
		cfg := ConfigFromEnv()
		if !cfg.Enabled || cfg.Host != "http://std-host:3100" {
			t.Fatalf("standard variables not honored: %+v", cfg)
		}
		if cfg.IncludeContent {
			t.Fatal("LOOM_TRACE_CONTENT=0 should redact content")
		}
	})
}

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
