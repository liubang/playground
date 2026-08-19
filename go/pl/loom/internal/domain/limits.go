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

package domain

import (
	"time"
	"unicode/utf8"
)

// Display bounds for tool-result previews and argument diffs, shared by the
// app layer (event payloads) and the UI (transcript blocks) — single home
// for what used to be duplicated constants (REVIEW R8).
const (
	ToolPreviewMaxLines = 12
	ToolPreviewMaxBytes = 1200
	ToolDiffMaxLines    = 40
	// ToolDiffUnbounded renders the complete diff (no line cap, no per-line
	// width cut): the web frontend folds long diffs into a collapsible block
	// instead of truncating them.
	ToolDiffUnbounded = 0
)

// ToolErrorEchoMaxBytes bounds user-supplied values echoed into tool
// error messages, so a pathological argument cannot bloat the transcript.
const ToolErrorEchoMaxBytes = 256

// TruncateForErrorEcho bounds a user-supplied value echoed into a tool
// error message: rune-safe truncation to ToolErrorEchoMaxBytes plus an
// ellipsis marker when truncated. Single home for what used to be a
// per-tool-package copy (truncateForErrorMessage).
func TruncateForErrorEcho(s string) string {
	if len(s) <= ToolErrorEchoMaxBytes {
		return s
	}
	return TruncateAtRuneBoundary(s, ToolErrorEchoMaxBytes) + "..."
}

// TruncateAtRuneBoundary returns the longest prefix of s within maxBytes
// that does not split a multi-byte UTF-8 character. Shared by every layer
// that bounds display text (REVIEW R8).
func TruncateAtRuneBoundary(s string, maxBytes int) string {
	if len(s) <= maxBytes {
		return s
	}
	cut := maxBytes
	for cut > 0 && !utf8.ValidString(s[:cut]) {
		cut--
	}
	return s[:cut]
}

// Limits constrains the resources a Run can consume.
//
// Only genuinely scarce resources are budgeted: cumulative session tokens
// and estimated cost, both session-scoped counters that never reset at
// prompt boundaries (docs/CONTEXT_DESIGN.md §4.4.3). Turn/tool-call counts
// and elapsed wall time are deliberately absent — they are proxy metrics
// that punish legitimate long work instead of catching runaway behavior;
// runaway behavior (including stalls) is detected behaviorally (see
// RunawayConfig). Per-request context pressure is absorbed by compaction
// against the model's context window. MaxInputTokens survives only as the
// fallback context-window proxy for models that do not declare one, and
// MaxOutputTokens as the per-request output cap.
type Limits struct {
	MaxInputTokens      int64   `json:"max_input_tokens"`
	MaxOutputTokens     int64   `json:"max_output_tokens"`
	MaxEstimatedCostUSD float64 `json:"max_estimated_cost_usd"`
	// MaxTokens budgets the session-cumulative metered tokens
	// (input + output) across every model call in the session. Zero
	// means unlimited: long-horizon tasks are undisturbed unless the
	// user explicitly opts into a resource ceiling.
	MaxTokens          int64 `json:"max_tokens"`
	MaxToolOutputBytes int64 `json:"max_tool_output_bytes"`
	MaxArtifactBytes   int64 `json:"max_artifact_bytes"`
}

// DefaultLimits returns the standard limits.
func DefaultLimits() Limits {
	return Limits{
		MaxInputTokens:      200_000,
		MaxOutputTokens:     16_384,
		MaxEstimatedCostUSD: 5.0,
		// Unlimited by default (opt-in), matching the codex token-budget
		// philosophy: long-horizon tasks are constrained by actual resource
		// consumption only when the user asks for it.
		MaxTokens: 0,
		// 48KB (~12k tokens) bounds each ingested tool result; larger
		// outputs are truncated head+tail with the full text preserved in
		// the artifact store.
		MaxToolOutputBytes: 48 * 1024,
		MaxArtifactBytes:   100 * 1024 * 1024,
	}
}

// Usage tracks accumulated resource consumption against Limits.
//
// InputTokens/OutputTokens/CostUSD are SESSION-cumulative counters: they
// survive prompt boundaries (inherited from the checkpoint by ContinueRun)
// and back the budget dimensions. Turns/ToolCalls/WallTime are per-prompt
// observability counters (status bar, budget events) reset at each prompt
// boundary — never budget dimensions.
type Usage struct {
	Turns        int   `json:"turns"`
	ToolCalls    int   `json:"tool_calls"`
	InputTokens  int64 `json:"input_tokens"`
	OutputTokens int64 `json:"output_tokens"`
	// CachedInputTokens accumulates provider-reported prompt-cache hits.
	// Observability only (cache efficiency indicator), never a budget
	// dimension. Provider semantics DIVERGE: OpenAI's prompt_tokens already
	// includes cached tokens, while Anthropic's input_tokens EXCLUDES them
	// (full input = input_tokens + cache_read + cache_creation) — so treat
	// the field as a ratio-free indicator, not a subset of InputTokens.
	CachedInputTokens int64 `json:"cached_input_tokens"`
	// ContextTokens accumulates the provider-metered context-window
	// footprint of every call (ModelEvent.ContextTokens: OpenAI
	// prompt_tokens, Anthropic input+cache_read+cache_creation). It is the
	// exact, provider-uniform denominator of the session cache-hit ratio
	// CachedInputTokens/ContextTokens.
	ContextTokens int64         `json:"context_tokens"`
	CostUSD       float64       `json:"cost_usd"`
	WallTime      time.Duration `json:"wall_time_ns"`
}

// CheckResult reports soft/hard threshold breaches.
type CheckResult struct {
	SoftBreaches []string
	HardBreaches []string
}

// HasSoft reports whether any soft threshold is breached.
func (c CheckResult) HasSoft() bool { return len(c.SoftBreaches) > 0 }

// HasHard reports whether any hard threshold is breached.
func (c CheckResult) HasHard() bool { return len(c.HardBreaches) > 0 }

// SoftHas reports whether the named dimension (e.g. "wall_time") is among
// the soft breaches, letting callers react to specific dimensions only.
func (c CheckResult) SoftHas(name string) bool {
	for _, b := range c.SoftBreaches {
		if b == name {
			return true
		}
	}
	return false
}

// Check evaluates current usage against the budget dimensions (tokens,
// cost). Soft = 80% of limit (the graduated-notice band, see the agent
// loop's budget notices); Hard = 100% of limit (enters the soft-landing
// wrap-up before termination).
func (u Usage) Check(l Limits) CheckResult {
	var res CheckResult
	soft := func(name string, cur, limit float64) {
		if limit <= 0 {
			return
		}
		ratio := cur / limit
		if ratio >= 1.0 {
			res.HardBreaches = append(res.HardBreaches, name)
		} else if ratio >= 0.8 {
			res.SoftBreaches = append(res.SoftBreaches, name)
		}
	}
	soft("tokens", float64(u.InputTokens+u.OutputTokens), float64(l.MaxTokens))
	soft("cost_usd", u.CostUSD, l.MaxEstimatedCostUSD)
	return res
}
