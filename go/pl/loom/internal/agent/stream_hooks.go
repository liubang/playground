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

package agent

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// StreamHooks provides optional real-time callbacks for streaming model events.
// All callbacks are invoked synchronously from the model stream consumption
// goroutine; implementations must not block for long periods.
type StreamHooks struct {
	// OnTextDelta is called for each text delta received from the model.
	OnTextDelta func(delta string)
	// OnReasoningDelta is called for each provider-supplied reasoning delta.
	OnReasoningDelta func(delta string)
	// OnToolCallDelta is called when tool call arguments are received.
	OnToolCallDelta func(toolIndex int, toolID, toolName, args string, deltaBytes int)
	// OnToolCallComplete is called when a tool call is fully received.
	OnToolCallComplete func(toolIndex int, toolID, toolName, args string)
	// OnModelUsage is called when usage information is received.
	OnModelUsage func(inputTokens, outputTokens int64)
}

// StreamAggregator validates and collects canonical model events while
// exposing streaming deltas via hooks. This replaces the previous
// aggregateStream function to allow real-time UI updates.
type StreamAggregator struct {
	clock         domain.Clock
	hooks         StreamHooks
	text          string
	reasoning     string
	tools         map[int]*streamToolCall
	seenIDs       map[string]struct{}
	stop          domain.StopReason
	inputTokens   int64
	outputTokens  int64
	responseEnded bool
}

type streamResponse struct {
	Message      domain.Message
	StopReason   domain.StopReason
	InputTokens  int64
	OutputTokens int64
}

type streamToolCall struct {
	index int
	id    string
	name  string
	args  string
	ended bool
}

// NewStreamAggregator creates a new StreamAggregator.
func NewStreamAggregator(clock domain.Clock, hooks StreamHooks) *StreamAggregator {
	return &StreamAggregator{
		clock:   clock,
		hooks:   hooks,
		tools:   make(map[int]*streamToolCall),
		seenIDs: make(map[string]struct{}),
	}
}

// Apply processes a single model event.
func (a *StreamAggregator) Apply(evt domain.ModelEvent) error {
	if a.responseEnded {
		return fmt.Errorf("event %q after response_end", evt.Kind)
	}
	switch evt.Kind {
	case domain.ModelEventResponseStart:
		// No-op
	case domain.ModelEventTextStart, domain.ModelEventTextEnd,
		domain.ModelEventReasoningStart, domain.ModelEventReasoningEnd,
		domain.ModelEventProviderWarning:
		// No-op
	case domain.ModelEventTextDelta:
		a.text += evt.TextDelta
		if a.hooks.OnTextDelta != nil {
			a.hooks.OnTextDelta(evt.TextDelta)
		}
	case domain.ModelEventReasoningDelta:
		a.reasoning += evt.ReasoningDelta
		if a.hooks.OnReasoningDelta != nil {
			a.hooks.OnReasoningDelta(evt.ReasoningDelta)
		}
	case domain.ModelEventToolCallStart:
		if _, exists := a.tools[evt.ToolIndex]; exists {
			return fmt.Errorf("duplicate tool index %d", evt.ToolIndex)
		}
		if evt.ToolID == "" || evt.ToolName == "" {
			return fmt.Errorf("tool call start requires id and name")
		}
		if _, exists := a.seenIDs[evt.ToolID]; exists {
			return fmt.Errorf("duplicate tool call id %q", evt.ToolID)
		}
		a.seenIDs[evt.ToolID] = struct{}{}
		a.tools[evt.ToolIndex] = &streamToolCall{index: evt.ToolIndex, id: evt.ToolID, name: evt.ToolName}
	case domain.ModelEventToolArgsDelta:
		tool, ok := a.tools[evt.ToolIndex]
		if !ok {
			return fmt.Errorf("arguments for unknown tool index %d", evt.ToolIndex)
		}
		tool.args += evt.ToolArgs
		if a.hooks.OnToolCallDelta != nil {
			a.hooks.OnToolCallDelta(evt.ToolIndex, tool.id, tool.name, tool.args, len(evt.ToolArgs))
		}
	case domain.ModelEventToolCallEnd:
		tool, ok := a.tools[evt.ToolIndex]
		if !ok {
			return fmt.Errorf("end for unknown tool index %d", evt.ToolIndex)
		}
		tool.ended = true
		if a.hooks.OnToolCallComplete != nil {
			a.hooks.OnToolCallComplete(evt.ToolIndex, tool.id, tool.name, tool.args)
		}
	case domain.ModelEventUsage:
		if evt.InputTokens < 0 || evt.OutputTokens < 0 {
			return fmt.Errorf("negative token usage")
		}
		a.inputTokens = evt.InputTokens
		a.outputTokens = evt.OutputTokens
		if a.hooks.OnModelUsage != nil {
			a.hooks.OnModelUsage(evt.InputTokens, evt.OutputTokens)
		}
	case domain.ModelEventStreamError:
		if evt.Error == "" {
			evt.Error = "provider stream error"
		}
		return errors.New(evt.Error)
	case domain.ModelEventResponseEnd:
		if evt.StopReason == "" {
			return fmt.Errorf("response_end requires stop reason")
		}
		a.stop = evt.StopReason
		a.responseEnded = true
	default:
		return fmt.Errorf("unknown model event kind %q", evt.Kind)
	}
	return nil
}

// Finalize completes the aggregation and returns the canonical response.
func (a *StreamAggregator) Finalize() (domain.Message, domain.StopReason, int64, int64, error) {
	if !a.responseEnded {
		return domain.Message{}, "", 0, 0, fmt.Errorf("stream ended before response_end")
	}
	indexes := make([]int, 0, len(a.tools))
	for index, tool := range a.tools {
		if !tool.ended {
			return domain.Message{}, "", 0, 0, fmt.Errorf("incomplete tool call at index %d", index)
		}
		indexes = append(indexes, index)
	}
	sort.Ints(indexes)
	parts := make([]domain.ContentPart, 0, len(indexes)+1)
	if a.text != "" {
		parts = append(parts, domain.ContentPart{Kind: domain.PartText, Text: a.text})
	}
	for _, index := range indexes {
		tool := a.tools[index]
		id, err := domain.ParseToolCallID(tool.id)
		if err != nil {
			return domain.Message{}, "", 0, 0, fmt.Errorf("invalid tool call id %q: %w", tool.id, err)
		}
		call := domain.ToolCall{ID: id, Name: tool.name, Arguments: json.RawMessage(tool.args)}
		if err := call.Validate(); err != nil {
			return domain.Message{}, "", 0, 0, fmt.Errorf("invalid tool call at index %d: %w", index, err)
		}
		parts = append(parts, domain.ContentPart{PartIndex: len(parts), Kind: domain.PartToolCall, ToolCall: &call})
	}
	if len(parts) == 0 {
		return domain.Message{}, "", 0, 0, fmt.Errorf("empty model response")
	}
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		Parts:     parts,
		CreatedAt: a.clock.Now(),
	}, a.stop, a.inputTokens, a.outputTokens, nil
}

// InterruptedMessage creates an interrupted message from partial text.
func (a *StreamAggregator) InterruptedMessage() domain.Message {
	if a.text == "" {
		return domain.Message{}
	}
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleAssistant,
		Status:    domain.MessageStatusInterrupted,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: a.text}},
		CreatedAt: a.clock.Now(),
	}
}

// HasPartialContent reports whether the aggregator has any text content.
func (a *StreamAggregator) HasPartialContent() bool {
	return a.text != ""
}

// consumeStream reads all events from a model stream into an aggregator.
// aggregateStream preserves the former aggregation helper for callers and tests
// while delegating validation to StreamAggregator.
func aggregateStream(stream domain.ModelStream, clock domain.Clock) (streamResponse, error) {
	agg := NewStreamAggregator(clock, StreamHooks{})
	if err := consumeStream(stream, agg); err != nil {
		response := streamResponse{}
		if agg.HasPartialContent() {
			response.Message = agg.InterruptedMessage()
		}
		return response, err
	}
	message, stop, inputTokens, outputTokens, err := agg.Finalize()
	if err != nil {
		response := streamResponse{}
		if agg.HasPartialContent() {
			response.Message = agg.InterruptedMessage()
		}
		return response, err
	}
	return streamResponse{Message: message, StopReason: stop, InputTokens: inputTokens, OutputTokens: outputTokens}, nil
}

func consumeStream(stream domain.ModelStream, agg *StreamAggregator) error {
	for {
		evt, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) && agg.responseEnded {
				return nil
			}
			return err
		}
		if err := agg.Apply(evt); err != nil {
			return err
		}
	}
}
