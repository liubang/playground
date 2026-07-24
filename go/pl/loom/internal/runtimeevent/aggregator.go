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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// AggregatorCallbacks receives streaming updates from StreamAggregator.
type AggregatorCallbacks struct {
	OnTextDelta        func(delta string)
	OnToolCallDelta    func(toolIndex int, toolID, toolName, args string, deltaBytes int)
	OnToolCallComplete func(toolIndex int, toolID, toolName, args string)
	OnUsage            func(inputTokens, outputTokens int64)
}

// StreamAggregator validates and collects canonical model events while
// exposing streaming deltas via callbacks.
type StreamAggregator struct {
	clock     domain.Clock
	callbacks AggregatorCallbacks

	text          string
	tools         map[int]*aggToolCall
	seenIDs       map[string]struct{}
	stop          domain.StopReason
	inputTokens   int64
	outputTokens  int64
	responseEnded bool
}

type aggToolCall struct {
	index int
	id    string
	name  string
	args  string
	ended bool
}

// NewStreamAggregator creates a new StreamAggregator.
func NewStreamAggregator(clock domain.Clock, callbacks AggregatorCallbacks) *StreamAggregator {
	return &StreamAggregator{
		clock:     clock,
		callbacks: callbacks,
		tools:     make(map[int]*aggToolCall),
		seenIDs:   make(map[string]struct{}),
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
	case domain.ModelEventTextStart, domain.ModelEventTextEnd, domain.ModelEventProviderWarning:
		// No-op
	case domain.ModelEventTextDelta:
		a.text += evt.TextDelta
		if a.callbacks.OnTextDelta != nil {
			a.callbacks.OnTextDelta(evt.TextDelta)
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
		a.tools[evt.ToolIndex] = &aggToolCall{index: evt.ToolIndex, id: evt.ToolID, name: evt.ToolName}
	case domain.ModelEventToolArgsDelta:
		tool, ok := a.tools[evt.ToolIndex]
		if !ok {
			return fmt.Errorf("arguments for unknown tool index %d", evt.ToolIndex)
		}
		tool.args += evt.ToolArgs
		if a.callbacks.OnToolCallDelta != nil {
			a.callbacks.OnToolCallDelta(evt.ToolIndex, tool.id, tool.name, tool.args, len(evt.ToolArgs))
		}
	case domain.ModelEventToolCallEnd:
		tool, ok := a.tools[evt.ToolIndex]
		if !ok {
			return fmt.Errorf("end for unknown tool index %d", evt.ToolIndex)
		}
		tool.ended = true
		if a.callbacks.OnToolCallComplete != nil {
			a.callbacks.OnToolCallComplete(evt.ToolIndex, tool.id, tool.name, tool.args)
		}
	case domain.ModelEventUsage:
		if evt.InputTokens < 0 || evt.OutputTokens < 0 {
			return fmt.Errorf("negative token usage")
		}
		a.inputTokens = evt.InputTokens
		a.outputTokens = evt.OutputTokens
		if a.callbacks.OnUsage != nil {
			a.callbacks.OnUsage(evt.InputTokens, evt.OutputTokens)
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

// StreamToAggregator consumes a model stream and returns the canonical response.
func StreamToAggregator(stream domain.ModelStream, clock domain.Clock, callbacks AggregatorCallbacks) (domain.Message, domain.StopReason, int64, int64, error) {
	agg := NewStreamAggregator(clock, callbacks)
	for {
		evt, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) && agg.responseEnded {
				break
			}
			if agg.HasPartialContent() {
				return agg.InterruptedMessage(), domain.StopCancelled, agg.inputTokens, agg.outputTokens, fmt.Errorf("stream ended before response_end: %w", err)
			}
			return domain.Message{}, "", 0, 0, fmt.Errorf("stream ended before response_end: %w", err)
		}
		if err := agg.Apply(evt); err != nil {
			if agg.HasPartialContent() {
				return agg.InterruptedMessage(), domain.StopCancelled, agg.inputTokens, agg.outputTokens, err
			}
			return domain.Message{}, "", 0, 0, err
		}
	}
	return agg.Finalize()
}
