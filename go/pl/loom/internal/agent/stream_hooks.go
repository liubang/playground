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
	"time"

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
	// OnContextUsage reports the calibrated occupancy of the next model
	// request — the same value the compaction trigger checks: the
	// provider-metered footprint of the last completed call (cache-inclusive)
	// plus an estimate of everything appended since, or a full request
	// estimate before the first call. It fires after each completed
	// response, after each tool batch and after each compaction, so
	// frontends can show live context-window occupancy.
	OnContextUsage func(occupancyTokens int64)
}

// StreamAggregator validates and collects canonical model events while
// exposing streaming deltas via hooks. This replaces the previous
// aggregateStream function to allow real-time UI updates.
type StreamAggregator struct {
	clock domain.Clock
	hooks StreamHooks
	text  string
	// reasoning accumulates the deltas of the currently open reasoning
	// block; sealed blocks (with their provider signatures) live in
	// reasoningBlocks and are persisted as transcript parts.
	reasoning       string
	reasoningBlocks []domain.ReasoningContent
	// reasoningStart is when the open block's first delta arrived; sealed
	// into the block's DurationMs at reasoning_end so the transcript can
	// show the thinking span after reloads.
	reasoningStart time.Time
	tools          map[int]*streamToolCall
	seenIDs        map[string]struct{}
	// idTaken reports whether a tool call id already appears in the
	// transcript (installed via WithIDRewrite); rewritten records every
	// collision rewrite (original → replacement) for observability.
	idTaken      func(string) bool
	rewritten    map[string]string
	stop         domain.StopReason
	inputTokens  int64
	outputTokens int64
	// cachedInputTokens accumulates provider-reported prompt-cache hits
	// (observability only; inputTokens already includes them).
	cachedInputTokens int64
	// cacheCreationInputTokens accumulates provider-reported prompt-cache
	// writes (Anthropic only; OpenAI folds them into prompt_tokens).
	cacheCreationInputTokens int64
	// contextTokens is the provider-metered context-window footprint of
	// the request (cache-inclusive); see ContextTokens().
	contextTokens int64
	// reasoningTokens is the provider-metered reasoning/thinking share of
	// outputTokens (0 when the provider does not split it out).
	reasoningTokens int64
	responseEnded   bool
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

// WithIDRewrite installs a transcript-level conflict detector. Providers
// like kimi reset their tool-call counters every turn ("run_cmd_0",
// "run_cmd_1", ...), so a fresh response can reuse an id from an earlier
// turn. Loom pairs calls with results and keys prepared-call maps by id,
// so collisions silently drop executions and leave the replay history
// dangling. Colliding ids are rewritten to fresh unique ones; hooks and
// the persisted message observe only the rewritten id, keeping live UI
// and replay consistent. The provider is unaffected: it only requires
// call/result id consistency within the replayed history.
func (a *StreamAggregator) WithIDRewrite(taken func(id string) bool) *StreamAggregator {
	a.idTaken = taken
	return a
}

// RewrittenIDs returns the collision rewrites applied so far
// (original → replacement); empty when no provider id collided.
func (a *StreamAggregator) RewrittenIDs() map[string]string {
	return a.rewritten
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
		domain.ModelEventProviderWarning:
		// No-op
	case domain.ModelEventReasoningStart:
		// A new block begins; interleaved thinking produces several blocks
		// per response, each sealed independently at reasoning_end.
		a.reasoning = ""
		a.reasoningStart = a.clock.Now()
	case domain.ModelEventReasoningEnd:
		if a.reasoning != "" || evt.ReasoningSignature != "" || evt.ReasoningRedacted {
			var dur int64
			if !a.reasoningStart.IsZero() {
				dur = max(a.clock.Since(a.reasoningStart).Milliseconds(), 0)
				a.reasoningStart = time.Time{}
			}
			a.reasoningBlocks = append(a.reasoningBlocks, domain.ReasoningContent{
				Text:       a.reasoning,
				Signature:  evt.ReasoningSignature,
				Redacted:   evt.ReasoningRedacted,
				DurationMs: dur,
			})
		}
		a.reasoning = ""
	case domain.ModelEventTextDelta:
		a.text += evt.TextDelta
		if a.hooks.OnTextDelta != nil {
			a.hooks.OnTextDelta(evt.TextDelta)
		}
	case domain.ModelEventReasoningDelta:
		// Providers that skip reasoning_start still get a timing anchor.
		if a.reasoningStart.IsZero() {
			a.reasoningStart = a.clock.Now()
		}
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
		id := evt.ToolID
		_, dup := a.seenIDs[id]
		if dup || (a.idTaken != nil && a.idTaken(id)) {
			id = domain.NewToolCallID().String()
			if a.rewritten == nil {
				a.rewritten = make(map[string]string)
			}
			a.rewritten[evt.ToolID] = id
		}
		a.seenIDs[id] = struct{}{}
		a.tools[evt.ToolIndex] = &streamToolCall{index: evt.ToolIndex, id: id, name: evt.ToolName}
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
		a.cachedInputTokens = evt.CachedInputTokens
		a.cacheCreationInputTokens = evt.CacheCreationInputTokens
		a.reasoningTokens = evt.ReasoningTokens
		// Providers that do not distinguish the window footprint from the
		// billing input (OpenAI-style prompt_tokens is already
		// cache-inclusive) leave ContextTokens unset; the metered input is
		// the occupancy measure then.
		a.contextTokens = evt.ContextTokens
		if a.contextTokens <= 0 {
			a.contextTokens = evt.InputTokens
		}
		if a.hooks.OnModelUsage != nil {
			a.hooks.OnModelUsage(evt.InputTokens, evt.OutputTokens)
		}
	case domain.ModelEventStreamError:
		if evt.Error == "" {
			evt.Error = "provider stream error"
		}
		if evt.Retryable {
			// A transient stream failure (truncation, transport drop) keeps
			// its classification so the loop can re-issue the request.
			return domain.NewError(domain.ErrUnavailable, evt.Error, domain.WithRetryable(true))
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
	parts := make([]domain.ContentPart, 0, len(a.reasoningBlocks)+len(indexes)+1)
	appendPart := func(part domain.ContentPart) {
		part.PartIndex = len(parts)
		parts = append(parts, part)
	}
	// Reasoning precedes the visible answer: providers that authenticate
	// thinking blocks require them at the head of the assistant message.
	for i := range a.reasoningBlocks {
		block := a.reasoningBlocks[i]
		appendPart(domain.ContentPart{Kind: domain.PartReasoning, Reasoning: &block})
	}
	if a.text != "" {
		appendPart(domain.ContentPart{Kind: domain.PartText, Text: a.text})
	}
	for _, index := range indexes {
		tool := a.tools[index]
		id, err := domain.ParseToolCallID(tool.id)
		if err != nil {
			return domain.Message{}, "", 0, 0, fmt.Errorf("invalid tool call id %q: %w", tool.id, err)
		}
		args := json.RawMessage(tool.args)
		if !json.Valid(args) {
			// Providers occasionally stream malformed arguments (pretty-printed
			// JSON with literal newlines, payloads truncated mid-stream, or no
			// arguments at all). Failing the whole run here strands the
			// session; instead preserve the raw payload as evidence and let
			// the tool layer reject the call with a recoverable prepare
			// error, so the model can re-issue it correctly.
			args = malformedArgumentsPlaceholder(tool.args)
		}
		call := domain.ToolCall{ID: id, Name: tool.name, Arguments: args}
		if err := call.Validate(); err != nil {
			return domain.Message{}, "", 0, 0, fmt.Errorf("invalid tool call at index %d: %w", index, err)
		}
		appendPart(domain.ContentPart{Kind: domain.PartToolCall, ToolCall: &call})
	}
	// A response of pure reasoning with neither visible text nor tool calls
	// is still an empty answer — reasoning alone cannot advance the run.
	if a.text == "" && len(indexes) == 0 {
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

// malformedArgumentsPlaceholder wraps a provider's malformed tool-call
// arguments into valid JSON that keeps the raw payload as evidence. The
// unknown field makes every built-in tool's argument decoder reject the
// call with a recoverable error result instead of executing garbage.
func malformedArgumentsPlaceholder(raw string) json.RawMessage {
	const maxRaw = 2048
	if len(raw) > maxRaw {
		raw = raw[:maxRaw] + "\u2026"
	}
	payload, err := json.Marshal(map[string]string{
		"__malformed_arguments": raw,
		"error":                 "model emitted invalid arguments JSON; re-issue the tool call with valid arguments",
	})
	if err != nil {
		return json.RawMessage(`{"__malformed_arguments":"","error":"model emitted invalid arguments JSON"}`)
	}
	return payload
}

// CachedInputTokens reports provider-reported prompt-cache hits for the
// completed call (0 when the provider does not report them). The value is
// observability-only: inputTokens already includes cached tokens.
func (a *StreamAggregator) CachedInputTokens() int64 { return a.cachedInputTokens }

// ContextTokens reports the provider-metered context-window footprint of
// the completed call: the exact number of tokens the request occupied
// (cache-inclusive). It defaults to the metered input when the provider
// does not report a distinct footprint.
func (a *StreamAggregator) ContextTokens() int64 { return a.contextTokens }

// CacheCreationInputTokens reports provider-reported prompt-cache writes
// for the completed call (0 when the provider does not split them out).
func (a *StreamAggregator) CacheCreationInputTokens() int64 {
	return a.cacheCreationInputTokens
}

// ReasoningTokens reports the provider-metered reasoning/thinking share of
// the completed call's output tokens (0 when the provider does not split
// it out).
func (a *StreamAggregator) ReasoningTokens() int64 { return a.reasoningTokens }

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

// HasActivity reports whether the stream delivered anything user-visible
// or persistable — text, reasoning, or a tool-call fragment. A failure
// with no activity is as safely retryable as a start-stage failure:
// nothing reached the UI or the transcript.
func (a *StreamAggregator) HasActivity() bool {
	return a.text != "" || a.reasoning != "" || len(a.reasoningBlocks) > 0 || len(a.tools) > 0
}

// consumeStream reads all events from a model stream into an aggregator.
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
