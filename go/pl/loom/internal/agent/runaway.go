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
// Created: 2026/07/29

package agent

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Behavior-based runaway detection (docs/CONTEXT_DESIGN.md §4.4.3). These
// detectors catch pathological patterns — exact-repeat loops, unbroken
// failure streaks, stalls — instead of punishing workload size the way
// turn/tool-call quotas did.

// runawayConfig resolves the effective thresholds: an entirely zero
// struct means "not configured" and falls back to the defaults.
func (l *Loop) runawayConfig() domain.RunawayConfig {
	if l.Runaway == (domain.RunawayConfig{}) {
		return domain.DefaultRunawayConfig()
	}
	return l.Runaway
}

// rawArgsHash fingerprints tool-call arguments (canonical JSON for
// prepared calls, raw JSON otherwise). It deliberately does NOT use
// PreparedCall.ArgsHash: that value is an HMAC signature over the call
// fingerprint, which includes the unique call ID — identical arguments
// would never collide (found via E2E: the repeated-call detector never
// fired with real tools).
func rawArgsHash(raw json.RawMessage) string {
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])[:16]
}

// callSignature identifies one logical tool action for repeat detection.
func callSignature(tool string, args json.RawMessage) string {
	return tool + "\x00" + rawArgsHash(args)
}

// trackToolCall feeds the runaway detectors with one routed tool call
// (prepared or failed-to-prepare). It returns the termination reason when
// the repeated-call threshold is hit; the caller then closes the batch
// and terminates the run. The repeat just below the threshold earns a
// queued warning so the model gets one chance to change strategy.
func (l *Loop) trackToolCall(tool string, args json.RawMessage) string {
	cfg := l.runawayConfig()
	sig := callSignature(tool, args)
	if sig == l.lastCallSig {
		l.repeatedCallCount++
	} else {
		l.lastCallSig = sig
		l.repeatedCallCount = 1
	}
	// A first-seen signature is a progress signal (the model is fetching
	// new information or producing new effects); repeats are not.
	if l.seenCallSigs == nil {
		l.seenCallSigs = make(map[string]struct{})
	}
	if _, seen := l.seenCallSigs[sig]; !seen {
		l.seenCallSigs[sig] = struct{}{}
		l.progressThisTurn = true
	}
	if cfg.MaxRepeatedCalls <= 0 {
		return ""
	}
	switch {
	case l.repeatedCallCount >= cfg.MaxRepeatedCalls:
		return fmt.Sprintf("runaway detected: tool %q was called with identical arguments %d times in a row", tool, l.repeatedCallCount)
	case l.repeatedCallCount == cfg.MaxRepeatedCalls-1 && l.fireNoticeOnce(dimensionRunaway+":"+sig, 1):
		l.queueNotice(domain.BudgetNoticePayload{
			Dimension: dimensionRunaway, Level: 1,
			Usage: int64(l.repeatedCallCount), Limit: int64(cfg.MaxRepeatedCalls),
			Message: noticeMessage(fmt.Sprintf(
				"[runaway warning] Tool %q was just called with identical arguments %d times in a row. Repeating it will not produce new information — change strategy (different arguments, a different tool, or report the blocker).",
				tool, l.repeatedCallCount), l.Run.Clock.Now()),
		})
	}
	return ""
}

// trackExecResult feeds the consecutive-execution-failure detector and
// returns the termination reason when the streak hits the threshold.
// Only execution-phase outcomes count: prepare failures are the model
// struggling with a schema (covered by repeat detection instead), and
// successful calls reset the streak.
func (l *Loop) trackExecResult(result domain.ToolResult) string {
	cfg := l.runawayConfig()
	if result.Status == domain.ToolStatusError {
		l.consecutiveExecFailures++
	} else {
		l.consecutiveExecFailures = 0
	}
	if cfg.MaxConsecutiveFailures > 0 && l.consecutiveExecFailures >= cfg.MaxConsecutiveFailures {
		return fmt.Sprintf("runaway detected: %d consecutive tool executions failed", l.consecutiveExecFailures)
	}
	return ""
}

// trackStall maintains the no-progress counter at the turn boundary
// (prepare). Any progress signal from the previous turn — visible text,
// a file change, a plan revision, or a first-seen tool signature —
// resets it; otherwise it grows, and every stall_warn_turns it injects a
// converge reminder (never a termination: wandering is corrected, not
// punished).
func (l *Loop) trackStall() {
	cfg := l.runawayConfig()
	if cfg.StallWarnTurns <= 0 {
		l.progressThisTurn = false
		return
	}
	// The first prepare has no previous turn to judge — consume the flag
	// without counting.
	if l.Run.Usage.Turns <= 1 {
		l.progressThisTurn = false
		return
	}
	if l.progressThisTurn {
		l.stallTurns = 0
	} else {
		l.stallTurns++
	}
	l.progressThisTurn = false
	if l.stallTurns >= cfg.StallWarnTurns && l.fireNoticeOnce(dimensionRunaway+":stall", l.stallTurns/cfg.StallWarnTurns) {
		l.stallTurns = 0
		l.Run.AddBudgetNotice(domain.BudgetNoticePayload{
			Dimension: dimensionRunaway, Level: 1,
			Usage: int64(cfg.StallWarnTurns), Limit: int64(cfg.StallWarnTurns),
			Message: noticeMessage(fmt.Sprintf(
				"[runaway warning] The last %d turns produced no visible progress (no new information, no file changes, no plan movement). Stop re-exploring and either commit to a concrete next action or report what is blocking you.",
				cfg.StallWarnTurns), l.Run.Clock.Now()),
		})
	}
}
