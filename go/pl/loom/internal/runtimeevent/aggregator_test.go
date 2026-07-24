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
// Created: 2026/07/23

package runtimeevent

import (
	"io"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestStreamAggregatorSimpleText(t *testing.T) {
	var deltas []string
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{
		OnTextDelta: func(delta string) {
			deltas = append(deltas, delta)
		},
	})

	// Simulate text deltas
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "Hello"})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: " world"})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})

	msg, stop, _, _, err := agg.Finalize()
	if err != nil {
		t.Fatalf("Finalize: %v", err)
	}
	if stop != domain.StopEndTurn {
		t.Fatalf("expected end_turn, got %q", stop)
	}
	parts := msg.TextParts()
	if len(parts) != 1 || parts[0] != "Hello world" {
		t.Fatalf("expected 'Hello world', got %v", parts)
	}
	if len(deltas) != 2 || deltas[0] != "Hello" || deltas[1] != " world" {
		t.Fatalf("expected deltas [Hello,  world], got %v", deltas)
	}
}

func TestStreamAggregatorWithToolCall(t *testing.T) {
	var toolDeltaCount int
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{
		OnToolCallDelta: func(toolIndex int, toolID, toolName, args string, deltaBytes int) {
			toolDeltaCount++
		},
	})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventTextStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: "call_1", ToolName: "read_file"})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: `{"path": "`})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: `foo.txt"}`})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse})

	msg, stop, _, _, err := agg.Finalize()
	if err != nil {
		t.Fatalf("Finalize: %v", err)
	}
	if stop != domain.StopToolUse {
		t.Fatalf("expected tool_use, got %q", stop)
	}
	calls := msg.ToolCalls()
	if len(calls) != 1 {
		t.Fatalf("expected 1 tool call, got %d", len(calls))
	}
	if calls[0].Name != "read_file" {
		t.Fatalf("expected read_file, got %q", calls[0].Name)
	}
	if toolDeltaCount != 2 {
		t.Fatalf("expected 2 tool delta callbacks, got %d", toolDeltaCount)
	}
}

func TestStreamAggregatorDuplicateToolIndex(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: "call_1", ToolName: "read_file"})
	err := agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: "call_2", ToolName: "list_dir"})
	if err == nil {
		t.Fatal("expected error for duplicate tool index")
	}
}

func TestStreamAggregatorEventAfterResponseEnd(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})
	err := agg.Apply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "extra"})
	if err == nil {
		t.Fatal("expected error for event after response_end")
	}
}

func TestStreamAggregatorMissingToolEnd(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: "call_1", ToolName: "read_file"})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse})

	_, _, _, _, err := agg.Finalize()
	if err == nil {
		t.Fatal("expected error for incomplete tool call")
	}
}

func TestStreamAggregatorEmptyResponse(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn})

	_, _, _, _, err := agg.Finalize()
	if err == nil {
		t.Fatal("expected error for empty response")
	}
}

func TestStreamAggregatorInterruptedMessage(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventTextDelta, TextDelta: "partial"})

	interrupted := agg.InterruptedMessage()
	if interrupted.Status != domain.MessageStatusInterrupted {
		t.Fatalf("expected interrupted status, got %q", interrupted.Status)
	}
	parts := interrupted.TextParts()
	if len(parts) != 1 || parts[0] != "partial" {
		t.Fatalf("expected ['partial'], got %v", parts)
	}
}

func TestStreamAggregatorNoPartialContent(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	if agg.HasPartialContent() {
		t.Fatal("expected no partial content")
	}
	interrupted := agg.InterruptedMessage()
	if interrupted.ID.IsZero() == false {
		t.Fatal("expected empty message")
	}
}

func TestStreamAggregatorUnknownEvent(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	err := agg.Apply(domain.ModelEvent{Kind: domain.ModelEventKind("unknown_event")})
	if err == nil {
		t.Fatal("expected error for unknown event kind")
	}
}

func TestStreamAggregatorNegativeUsage(t *testing.T) {
	agg := NewStreamAggregator(domain.RealClock{}, AggregatorCallbacks{})

	_ = agg.Apply(domain.ModelEvent{Kind: domain.ModelEventResponseStart})
	err := agg.Apply(domain.ModelEvent{Kind: domain.ModelEventUsage, InputTokens: -1})
	if err == nil {
		t.Fatal("expected error for negative tokens")
	}
}

func TestStreamToAggregator(t *testing.T) {
	events := []domain.ModelEvent{
		{Kind: domain.ModelEventResponseStart},
		{Kind: domain.ModelEventTextDelta, TextDelta: "Hello"},
		{Kind: domain.ModelEventTextDelta, TextDelta: " world"},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}
	stream := &fakeModelStream{events: events}

	var deltas []string
	msg, stop, _, _, err := StreamToAggregator(stream, domain.RealClock{}, AggregatorCallbacks{
		OnTextDelta: func(d string) { deltas = append(deltas, d) },
	})
	if err != nil {
		t.Fatalf("StreamToAggregator: %v", err)
	}
	if stop != domain.StopEndTurn {
		t.Fatalf("expected end_turn, got %q", stop)
	}
	parts := msg.TextParts()
	if len(parts) != 1 || parts[0] != "Hello world" {
		t.Fatalf("text = %v", parts)
	}
	if len(deltas) != 2 {
		t.Fatalf("deltas = %v", deltas)
	}
}

func TestStreamToAggregatorInterrupted(t *testing.T) {
	events := []domain.ModelEvent{
		{Kind: domain.ModelEventResponseStart},
		{Kind: domain.ModelEventTextDelta, TextDelta: "partial"},
		// No response_end
	}
	stream := &fakeModelStream{events: events, eofAtEnd: true}

	msg, stop, _, _, err := StreamToAggregator(stream, domain.RealClock{}, AggregatorCallbacks{})
	if err == nil {
		t.Fatal("expected error for incomplete stream")
	}
	if stop != domain.StopCancelled {
		t.Fatalf("expected cancelled stop, got %q", stop)
	}
	if msg.Status != domain.MessageStatusInterrupted {
		t.Fatalf("expected interrupted, got %q", msg.Status)
	}
}

// fakeModelStream is a test helper that returns predetermined events.
type fakeModelStream struct {
	events   []domain.ModelEvent
	index    int
	eofAtEnd bool
}

func (s *fakeModelStream) Recv() (domain.ModelEvent, error) {
	if s.index >= len(s.events) {
		if s.eofAtEnd {
			return domain.ModelEvent{}, io.EOF
		}
		return domain.ModelEvent{}, io.EOF
	}
	evt := s.events[s.index]
	s.index++
	return evt, nil
}

func (s *fakeModelStream) Close() error {
	return nil
}
