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
// Created: 2026/09/05

package wireutil

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/model/stream"
)

func TestNewHTTPClient(t *testing.T) {
	if _, err := NewHTTPClient("openai", httpc.Config{}); err != nil {
		t.Fatalf("NewHTTPClient: %v", err)
	}
	if _, err := NewHTTPClient("anthropic", httpc.Config{MaxRetries: -1}); err == nil ||
		!strings.Contains(err.Error(), "anthropic provider:") {
		t.Fatalf("err = %v", err)
	}
}

func TestStreamHeaders(t *testing.T) {
	h := StreamHeaders()
	if got := h.Get("Accept"); got != "text/event-stream" {
		t.Fatalf("Accept = %q", got)
	}
	if got := h.Get("Content-Type"); got != "application/json" {
		t.Fatalf("Content-Type = %q", got)
	}
}

func TestMessageText(t *testing.T) {
	msg := domain.Message{
		Role: domain.RoleUser,
		Parts: []domain.ContentPart{
			{Kind: domain.PartText, Text: "hello "},
			{Kind: domain.PartText, Text: "world"},
		},
	}
	text, err := MessageText("openai", msg)
	if err != nil || text != "hello world" {
		t.Fatalf("MessageText = %q, %v", text, err)
	}

	msg.Parts = append(msg.Parts, domain.ContentPart{Kind: domain.PartImage, Image: &domain.ImageContent{MediaType: "image/png", Data: "x"}})
	if _, err := MessageText("openai", msg); err == nil ||
		!strings.Contains(err.Error(), "openai provider: role \"user\" only supports text parts") {
		t.Fatalf("MessageText err = %v", err)
	}
}

func TestToolInputSchema(t *testing.T) {
	def := domain.ToolDefinition{Name: "noop"}
	schema, err := ToolInputSchema("anthropic", def)
	if err != nil {
		t.Fatalf("ToolInputSchema: %v", err)
	}
	if m, ok := schema.(map[string]any); !ok || m["type"] != "object" {
		t.Fatalf("default schema = %v", schema)
	}

	def.InputSchema = []byte(`{"type":"object","properties":{"a":{"type":"string"}}}`)
	schema, err = ToolInputSchema("anthropic", def)
	if err != nil {
		t.Fatalf("ToolInputSchema: %v", err)
	}
	if m := schema.(map[string]any); m["type"] != "object" {
		t.Fatalf("decoded schema = %v", schema)
	}

	def.InputSchema = []byte(`{oops`)
	if _, err := ToolInputSchema("anthropic", def); err == nil ||
		!strings.Contains(err.Error(), `anthropic provider: decode tool schema for "noop"`) {
		t.Fatalf("decode err = %v", err)
	}
}

func TestToolResultTextKeepsArtifactRefsOutOfModelPayload(t *testing.T) {
	ref := domain.ArtifactRef{ID: domain.NewArtifactID(), Size: 1024}
	result := domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: `{"stdout":"bounded"}`},
			{Kind: domain.PartArtifact, Artifact: &ref},
		},
		Metadata: map[string]string{"stdout_artifact_id": ref.ID.String()},
	}
	if got := ToolResultText(result); got != `{"stdout":"bounded"}` {
		t.Fatalf("ToolResultText = %q", got)
	}
}

func TestToolResultTextErrorEnvelope(t *testing.T) {
	result := domain.ToolResult{
		CallID: domain.NewToolCallID(),
		Status: domain.ToolStatusError,
		Error:  &domain.ToolError{Message: "boom"},
	}
	if got := ToolResultText(result); !strings.Contains(got, `"boom"`) || !strings.Contains(got, `"status"`) {
		t.Fatalf("error envelope = %q", got)
	}
}

func TestToolResultTextStructuredFallback(t *testing.T) {
	img := domain.ImageContent{MediaType: "image/png", Data: "eHh4"}
	result := domain.ToolResult{
		CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess,
		Content: []domain.ContentPart{
			{Kind: domain.PartText, Text: "saw it"},
			{Kind: domain.PartImage, Image: &img},
		},
	}
	if !ToolResultHasImages(result) {
		t.Fatal("ToolResultHasImages = false, want true")
	}
	got := ToolResultText(result)
	if !strings.Contains(got, `"status"`) || !strings.Contains(got, `"content"`) {
		t.Fatalf("structured envelope = %q", got)
	}
	if ToolResultHasImages(domain.ToolResult{CallID: domain.NewToolCallID(), Status: domain.ToolStatusSuccess}) {
		t.Fatal("ToolResultHasImages = true, want false")
	}
}

func collectEvents() (func(domain.ModelEvent) bool, func() []domain.ModelEvent) {
	var events []domain.ModelEvent
	return func(evt domain.ModelEvent) bool {
		events = append(events, evt)
		return true
	}, func() []domain.ModelEvent { return events }
}

func TestEmitStreamFailure(t *testing.T) {
	emit, events := collectEvents()
	closed := 0
	EmitStreamFailure(emit, errors.New("boom"), domain.StopProviderError, true, func() { closed++ })

	if closed != 1 {
		t.Fatalf("closeOpen called %d times", closed)
	}
	got := events()
	if len(got) != 2 {
		t.Fatalf("%d events, want 2", len(got))
	}
	if got[0].Kind != domain.ModelEventStreamError || got[0].Error != "boom" || !got[0].Retryable {
		t.Fatalf("event[0] = %+v", got[0])
	}
	if got[1].Kind != domain.ModelEventResponseEnd || got[1].StopReason != domain.StopProviderError {
		t.Fatalf("event[1] = %+v", got[1])
	}
}

func TestFinishReadError(t *testing.T) {
	type outcome struct {
		err       string
		stop      domain.StopReason
		retryable bool
	}

	run := func(ctx context.Context, err error, handleEOF func() bool) (outcome, bool) {
		var out outcome
		finished := false
		FinishReadError(ctx, err, "openai", "[DONE]", handleEOF,
			func(e error, stop domain.StopReason, retryable bool) {
				out = outcome{err: e.Error(), stop: stop, retryable: retryable}
				finished = true
			})
		return out, finished
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	out, finished := run(ctx, errors.New("reset"), nil)
	if !finished || out.stop != domain.StopCancelled || out.retryable {
		t.Fatalf("cancel = %+v finished=%v", out, finished)
	}

	out, finished = run(context.Background(), io.EOF, nil)
	if !finished || out.stop != domain.StopProviderError || !out.retryable ||
		out.err != "openai provider: stream closed before [DONE]" {
		t.Fatalf("eof = %+v finished=%v", out, finished)
	}

	if _, finished = run(context.Background(), io.EOF, func() bool { return true }); finished {
		t.Fatal("claimed EOF must not reach finish")
	}

	out, finished = run(context.Background(), errors.New("connection reset"), nil)
	if !finished || !out.retryable ||
		out.err != "openai provider: stream read failed: connection reset" {
		t.Fatalf("read err = %+v finished=%v", out, finished)
	}
}

func TestStartStreamLaunchesPump(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "data: {}\n\n")
	}))
	defer server.Close()

	client, err := httpc.New(httpc.Config{})
	if err != nil {
		t.Fatalf("httpc.New: %v", err)
	}

	model, err := StartStream(context.Background(), client, server.URL, []byte(`{}`), StreamHeaders(), "openai",
		stream.Pump(func(_ context.Context, _ io.Reader, emit stream.Emitter) {
			emit(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
			emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})
		}))
	if err != nil {
		t.Fatalf("StartStream: %v", err)
	}
	defer func() { _ = model.Close() }()

	for i := 0; i < 2; i++ {
		evt, err := model.Recv()
		if err != nil {
			t.Fatalf("Recv: %v", err)
		}
		if i == 0 && evt.Kind != domain.ModelEventResponseStart {
			t.Fatalf("event[0] = %+v", evt)
		}
	}
}

func TestStartStreamRejectsNonSSE(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{}`)
	}))
	defer server.Close()

	client, err := httpc.New(httpc.Config{})
	if err != nil {
		t.Fatalf("httpc.New: %v", err)
	}
	if _, err := StartStream(context.Background(), client, server.URL, []byte(`{}`), StreamHeaders(), "anthropic", nil); err == nil ||
		!strings.Contains(err.Error(), "anthropic provider:") {
		t.Fatalf("err = %v", err)
	}
}

func TestStartStreamClassifiesHTTPFailure(t *testing.T) {
	t.Parallel()

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "slow down", http.StatusTooManyRequests)
	}))
	defer server.Close()

	client, err := httpc.New(httpc.Config{MaxRetryAfterWait: time.Millisecond})
	if err != nil {
		t.Fatalf("httpc.New: %v", err)
	}
	_, err = StartStream(context.Background(), client, server.URL, []byte(`{}`), StreamHeaders(), "openai", nil)
	var ae *domain.AgentError
	if !errors.As(err, &ae) || ae.Code != domain.ErrRateLimited {
		t.Fatalf("err = %v", err)
	}
}
