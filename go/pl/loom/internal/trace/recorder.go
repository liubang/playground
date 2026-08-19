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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// RunMeta identifies one agent run (one user submission through the loop to
// a terminal state) and becomes the root trace in Langfuse.
type RunMeta struct {
	SessionID string
	RunID     string
	Model     string
	Prompt    string // the user's submission (trace input)
	// Workspace is the working directory root, reported as a trace tag.
	Workspace string
}

// GenerationRecord captures one completed model call with full fidelity.
type GenerationRecord struct {
	RequestID    string
	Turn         int
	Model        string
	Input        []domain.Message // effective messages sent to the model
	Output       domain.Message   // final assistant message (zero on hard failure)
	StopReason   string
	InputTokens  int64
	OutputTokens int64
	// CachedInputTokens / CacheCreationInputTokens carry the provider's
	// prompt-cache split for the usage_details cost accounting (Anthropic
	// cache_read / cache_creation; OpenAI reports reads only, folded into
	// InputTokens). Zero when the provider does not report them.
	CachedInputTokens        int64
	CacheCreationInputTokens int64
	StartTime                time.Time
	EndTime                  time.Time
	Err                      error
	// PromptName and PromptVersion link the generation to a Langfuse-managed
	// prompt when the system prompt came from Prompt Management (zero =
	// not managed).
	PromptName    string
	PromptVersion int
}

// ToolRecord captures one completed tool execution.
type ToolRecord struct {
	CallID    string
	Name      string
	Risk      string
	Arguments json.RawMessage
	Status    string // success | error | cancelled | timeout
	// Code is the stable error classification (permission_denied, internal,
	// ...); it carries no user content and is reported even when content
	// capture is redacted, unlike Error.
	Code      string
	Error     string
	Preview   string // bounded output excerpt
	StartTime time.Time
	EndTime   time.Time
}

// RunResult summarizes the terminal state of a run.
type RunResult struct {
	// Outcome is the run outcome (completed, failed, cancelled, ...).
	Outcome string
	// Error is the terminal error, when any.
	Error string
	// Output is the final assistant reply (trace output).
	Output string
}

// Recorder is the agent loop's observability sink. Implementations must be
// safe for synchronous use from the loop goroutine and must not block:
// exporters buffer in the background.
type Recorder interface {
	// StartRun opens the root span for one run. The returned context carries
	// the span so generation/tool spans parent to it.
	StartRun(ctx context.Context, meta RunMeta) (context.Context, RunHandle)
	// ScoreTrace submits a score for a trace by ID, independent of any
	// active run handle — user feedback arrives after the run ended and its
	// handle is gone. delivered reports whether a backend accepted the
	// submission; the noop recorder returns false so callers can surface
	// "tracing disabled" instead of silently dropping the vote.
	ScoreTrace(ctx context.Context, traceID, name string, value float64, comment string) (delivered bool)
}

// RunHandle records child observations and closes the root span.
type RunHandle interface {
	// RecordGeneration appends a model-call generation span.
	RecordGeneration(ctx context.Context, rec GenerationRecord)
	// RecordTool appends a tool-execution span.
	RecordTool(ctx context.Context, rec ToolRecord)
	// RecordEvent attaches a point-in-time event (compaction, approval) to
	// the root span.
	RecordEvent(ctx context.Context, name string, attrs map[string]string)
	// Score attaches a numeric score to the trace (e.g. run_success 0/1).
	// Implementations report asynchronously; failures are logged, never
	// propagated.
	Score(ctx context.Context, name string, value float64, comment string)
	// TraceID returns the backend trace identifier for this run ("" for the
	// noop handle). The ID stays valid after End: scores submitted later
	// (user feedback) link to the same trace by ID.
	TraceID() string
	// End closes the root span with the terminal outcome.
	End(result RunResult)
}

// noopRecorder is the zero-overhead recorder used when tracing is disabled.
type noopRecorder struct{}

type noopRun struct{}

// Noop returns a recorder that discards everything.
func Noop() Recorder { return noopRecorder{} }

func (noopRecorder) StartRun(ctx context.Context, _ RunMeta) (context.Context, RunHandle) {
	return ctx, noopRun{}
}

func (noopRun) RecordGeneration(context.Context, GenerationRecord)     {}
func (noopRun) RecordTool(context.Context, ToolRecord)                 {}
func (noopRun) RecordEvent(context.Context, string, map[string]string) {}
func (noopRun) Score(context.Context, string, float64, string)         {}
func (noopRun) TraceID() string                                        { return "" }
func (noopRun) End(RunResult)                                          {}

func (noopRecorder) ScoreTrace(context.Context, string, string, float64, string) bool {
	return false
}
