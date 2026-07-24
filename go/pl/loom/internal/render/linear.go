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

// Package render provides non-interactive renderers for the Loom runtime
// event protocol. The Linear renderer produces human-readable plain text
// output suitable for non-TTY terminals; the JSONL renderer produces
// machine-readable JSON lines. Both implement runtimeevent.Observer and
// share the same event protocol as the TUI.
package render

import (
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Linear renders RuntimeEvents as human-readable plain text to a writer.
// It produces no ANSI escape codes, no spinners, and no interactive prompts.
// Tool execution is shown as one line per phase; model text is streamed
// inline with a prefix.
type Linear struct {
	out    io.Writer
	indent int

	// lastUsage tracks the most recent per-request token counts so the
	// response summary line can report them; response events themselves do
	// not carry token totals.
	lastInputTokens  int64
	lastOutputTokens int64
}

// NewLinear creates a new Linear renderer.
func NewLinear(out io.Writer) *Linear {
	return &Linear{out: out}
}

// ObserveEphemeral handles ephemeral (non-durable) events. Text deltas are
// written inline; tool progress is ignored in linear mode.
func (r *Linear) ObserveEphemeral(evt runtimeevent.RuntimeEvent) {
	switch evt.Kind {
	case runtimeevent.KindModelTextDelta:
		var payload runtimeevent.ModelTextDeltaPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprint(r.out, payload.Delta)
		}
	case runtimeevent.KindModelToolCallDelta:
		// In linear mode, tool call deltas are not streamed; the final
		// tool events (prepared/started/completed) are durable and
		// handled in ObserveDurable.
	case runtimeevent.KindToolProgress:
		// Progress events are ephemeral; linear mode ignores them.
	case runtimeevent.KindRunCancelRequested:
		fmt.Fprintln(r.out, "[cancelling]")
	}
}

// ObserveDurable handles durable events. Linear prints one line per
// significant durable event to provide a readable trace.
func (r *Linear) ObserveDurable(evt runtimeevent.RuntimeEvent) error {
	switch evt.Kind {
	case runtimeevent.KindSessionOpened:
		// Session open is logged quietly.
	case runtimeevent.KindTurnStarted:
		var payload runtimeevent.TurnStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "\n[TURN %d] %s\n", payload.TurnIndex, payload.Prompt)
		}
	case runtimeevent.KindRunPhaseChanged:
		// Phase changes are ephemeral-level detail in linear mode.
	case runtimeevent.KindModelRequestStarted:
		var payload runtimeevent.ModelRequestStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprint(r.out, "[assistant] ")
		}
		_ = payload
	case runtimeevent.KindModelResponseCompleted:
		var payload runtimeevent.ModelResponseCompletedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintln(r.out)
			if r.lastInputTokens > 0 || r.lastOutputTokens > 0 {
				fmt.Fprintf(r.out, "  (tokens: in=%d out=%d)\n", r.lastInputTokens, r.lastOutputTokens)
				r.lastInputTokens, r.lastOutputTokens = 0, 0
			}
		}
	case runtimeevent.KindModelRequestFailed:
		var payload runtimeevent.ModelRequestFailedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "\n[model error] %s: %s\n", payload.Stage, payload.Code)
		}
	case runtimeevent.KindApprovalRequested:
		var payload runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[approval needed] %s (risk=%d hash=%s)\n  %s\n",
				payload.ToolName, payload.Risk, payload.ArgsHash, payload.Description)
			fmt.Fprintln(r.out, "  DENIED (non-TTY mode: no interactive approver available)")
		}
	case runtimeevent.KindApprovalResolved:
		var payload runtimeevent.ApprovalResolvedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[approval] %s: %s\n", payload.CallID, payload.Decision)
		}
	case runtimeevent.KindToolPrepared:
		var payload runtimeevent.ToolPreparedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[tool] %s (risk=%d) prepared\n", payload.ToolName, payload.Risk)
		}
	case runtimeevent.KindToolStarted:
		var payload runtimeevent.ToolStartedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[tool] %s running\n", payload.ToolName)
		}
	case runtimeevent.KindToolCompleted:
		var payload runtimeevent.ToolCompletedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			if payload.Status == domain.ToolStatusSuccess {
				fmt.Fprintf(r.out, "[tool] %s completed (%dms)\n",
					payload.ToolName, payload.DurationMs)
			} else if payload.Error != "" {
				fmt.Fprintf(r.out, "[tool] %s %s: %s (%dms)\n",
					payload.ToolName, payload.Status, payload.Error, payload.DurationMs)
			} else {
				fmt.Fprintf(r.out, "[tool] %s %s (%dms)\n",
					payload.ToolName, payload.Status, payload.DurationMs)
			}
		}
	case runtimeevent.KindBudgetUpdated:
		// Cumulative budget totals are not printed in linear mode.
	case runtimeevent.KindUsageUpdated:
		var payload runtimeevent.UsageUpdatedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			r.lastInputTokens = payload.InputTokens
			r.lastOutputTokens = payload.OutputTokens
		}
	case runtimeevent.KindRunCancelled:
		var payload runtimeevent.RunCancelledPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil && payload.Reason != "" {
			fmt.Fprintf(r.out, "[cancelled] %s\n", payload.Reason)
		} else {
			fmt.Fprintln(r.out, "[cancelled]")
		}
	case runtimeevent.KindRunCompleted:
		fmt.Fprintln(r.out, "[completed]")
	case runtimeevent.KindRuntimeWarning:
		var payload runtimeevent.RuntimeWarningPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[warning] %s\n", payload.Message)
		}
	case runtimeevent.KindRuntimeFatal:
		var payload runtimeevent.RuntimeFatalPayload
		if err := json.Unmarshal(evt.Payload, &payload); err == nil {
			fmt.Fprintf(r.out, "[fatal] %s\n", payload.Message)
		}
	}
	return nil
}

// ReportError writes a plain error message.
func (r *Linear) ReportError(msg string) {
	fmt.Fprintf(r.out, "[error] %s\n", msg)
}

// RiskDescription returns a human-readable risk level name.
func RiskDescription(risk domain.RiskLevel) string {
	switch risk {
	case domain.R0:
		return "none"
	case domain.R1:
		return "read-only"
	case domain.R2:
		return "write"
	case domain.R3:
		return "destructive"
	case domain.R4:
		return "critical"
	default:
		return fmt.Sprintf("unknown(%d)", int(risk))
	}
}

// SanitizeText removes control characters that could affect terminal state.
// Newlines and tabs are preserved; carriage returns are dropped because they
// can overwrite the current terminal line, as are C1 control characters.
func SanitizeText(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for _, r := range s {
		switch {
		case r == '\n' || r == '\t':
			b.WriteRune(r)
		case r == '\r':
			// Drop CR: it can erase the current line on real terminals.
		case r < 0x20 || r == 0x7f || (r >= 0x80 && r <= 0x9f):
			// Drop C0, DEL and C1 control characters.
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}
