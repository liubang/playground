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

package fakes

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestFakeModelTextResponse(t *testing.T) {
	model := NewFakeModel(ScriptEntry{
		Text:       "Hello world",
		StopReason: domain.StopEndTurn,
		UsageIn:    50,
		UsageOut:   10,
	})

	stream, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err != nil {
		t.Fatalf("Stream error: %v", err)
	}
	defer stream.Close()

	var gotText string
	var gotStop domain.StopReason
	var gotIn, gotOut int64

	for {
		evt, err := stream.Recv()
		if err != nil {
			break
		}
		switch evt.Kind {
		case domain.ModelEventTextDelta:
			gotText += evt.TextDelta
		case domain.ModelEventResponseEnd:
			gotStop = evt.StopReason
		case domain.ModelEventUsage:
			gotIn = evt.InputTokens
			gotOut = evt.OutputTokens
		}
	}

	if gotText != "Hello world" {
		t.Fatalf("expected 'Hello world', got %q", gotText)
	}
	if gotStop != domain.StopEndTurn {
		t.Fatalf("expected end_turn, got %s", gotStop)
	}
	if gotIn != 50 || gotOut != 10 {
		t.Fatalf("usage mismatch: in=%d out=%d", gotIn, gotOut)
	}
}

func TestFakeModelToolCallResponse(t *testing.T) {
	tcID := domain.NewToolCallID()
	model := NewFakeModel(ScriptEntry{
		ToolCalls: []domain.ToolCall{
			{ID: tcID, Name: "read_file", Arguments: json.RawMessage(`{"path":"test.go"}`)},
		},
		StopReason: domain.StopToolUse,
		UsageIn:    100,
		UsageOut:   20,
	})

	stream, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err != nil {
		t.Fatalf("Stream error: %v", err)
	}
	defer stream.Close()

	var toolCallStart, toolArgsDelta, toolCallEnd bool
	var gotStop domain.StopReason

	for {
		evt, err := stream.Recv()
		if err != nil {
			break
		}
		switch evt.Kind {
		case domain.ModelEventToolCallStart:
			toolCallStart = true
			if evt.ToolName != "read_file" {
				t.Errorf("expected tool name read_file, got %s", evt.ToolName)
			}
		case domain.ModelEventToolArgsDelta:
			toolArgsDelta = true
		case domain.ModelEventToolCallEnd:
			toolCallEnd = true
		case domain.ModelEventResponseEnd:
			gotStop = evt.StopReason
		}
	}

	if !toolCallStart {
		t.Error("expected tool_call_start event")
	}
	if !toolArgsDelta {
		t.Error("expected tool_arguments_delta event")
	}
	if !toolCallEnd {
		t.Error("expected tool_call_end event")
	}
	if gotStop != domain.StopToolUse {
		t.Fatalf("expected tool_use stop, got %s", gotStop)
	}
}

func TestFakeModelScriptExhausted(t *testing.T) {
	model := NewFakeModel() // no entries

	_, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err == nil {
		t.Fatal("expected error for exhausted script")
	}
}

func TestFakeModelScriptError(t *testing.T) {
	model := NewFakeModel(ScriptEntry{
		Error: "provider unavailable",
	})

	_, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err == nil {
		t.Fatal("expected error from script entry")
	}
}

func TestFakeModelMultiTurn(t *testing.T) {
	model := NewFakeModel(
		ScriptEntry{Text: "first", StopReason: domain.StopToolUse},
		ScriptEntry{Text: "second", StopReason: domain.StopEndTurn},
	)

	// First call
	stream1, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err != nil {
		t.Fatalf("Stream 1 error: %v", err)
	}
	stream1.Close()

	// Second call
	stream2, err := model.Stream(context.Background(), domain.ModelRequest{})
	if err != nil {
		t.Fatalf("Stream 2 error: %v", err)
	}
	stream2.Close()

	// Third call should fail
	_, err = model.Stream(context.Background(), domain.ModelRequest{})
	if err == nil {
		t.Fatal("expected error on third call")
	}
}

func TestFakeModelTracksCalls(t *testing.T) {
	model := NewFakeModel(
		ScriptEntry{Text: "hi", StopReason: domain.StopEndTurn},
	)

	req1 := domain.ModelRequest{ModelName: "model-a"}
	_, _ = model.Stream(context.Background(), req1)

	calls := model.Calls()
	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d", len(calls))
	}
	if calls[0].ModelName != "model-a" {
		t.Errorf("expected model-a, got %s", calls[0].ModelName)
	}
}

func TestFakeStreamCloseThenRecv(t *testing.T) {
	model := NewFakeModel(ScriptEntry{Text: "hi", StopReason: domain.StopEndTurn})
	stream, _ := model.Stream(context.Background(), domain.ModelRequest{})
	stream.Close()

	_, err := stream.Recv()
	if err == nil {
		t.Fatal("expected error after close")
	}
}
