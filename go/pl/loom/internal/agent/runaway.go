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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Behavior-based runaway detection (docs/CONTEXT_DESIGN.md §4.4.3). These
// detectors catch pathological patterns — exact-repeat loops, unbroken
// failure streaks, stalls — instead of punishing workload size the way
// turn/tool-call quotas did.

// runawayDetector holds the behavior-based detection state for one run.
// Like every other piece of loop bookkeeping it is owned by the loop
// goroutine: all mutations happen on the serial routing/execution path.
type runawayDetector struct {
	// lastCallSig/repeatedCallCount track consecutive identical tool calls
	// (repeated-call detection).
	lastCallSig       string
	repeatedCallCount int
	// consecutiveExecFailures counts execution-phase tool failures in a
	// row (prepare failures excluded by design).
	consecutiveExecFailures int
	// seenCallSigs records every (tool, args_hash) signature executed this
	// run; a first-seen signature is a progress signal (new information
	// fetched), which keeps read-only research tasks from stalling out.
	seenCallSigs map[string]struct{}
	// stallTurns counts consecutive turns without any progress signal.
	stallTurns int
	// progressThisTurn is set by any progress signal and consumed by the
	// next prepare.
	progressThisTurn bool
	// lastProgressAt anchors the stall watchdog: the ACTIVE time since
	// the last progress signal. Approval waits shift it forward so user
	// thinking time never counts (docs/CONTEXT_DESIGN.md §4.4.3).
	lastProgressAt time.Time
}

// resolveRunawayConfig resolves the effective thresholds: an entirely
// zero struct means "not configured" and falls back to the defaults.
func resolveRunawayConfig(cfg domain.RunawayConfig) domain.RunawayConfig {
	if cfg == (domain.RunawayConfig{}) {
		return domain.DefaultRunawayConfig()
	}
	return cfg
}

// runawayConfig resolves the loop's configured thresholds.
func (l *Loop) runawayConfig() domain.RunawayConfig {
	return resolveRunawayConfig(l.Runaway)
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
func (d *runawayDetector) trackToolCall(cfg domain.RunawayConfig, tool string, args json.RawMessage, notices *noticeCenter, clock domain.Clock) string {
	sig := callSignature(tool, args)
	if sig == d.lastCallSig {
		d.repeatedCallCount++
	} else {
		d.lastCallSig = sig
		d.repeatedCallCount = 1
	}
	// A first-seen signature is a progress signal (the model is fetching
	// new information or producing new effects); repeats are not.
	if d.seenCallSigs == nil {
		d.seenCallSigs = make(map[string]struct{})
	}
	if _, seen := d.seenCallSigs[sig]; !seen {
		d.seenCallSigs[sig] = struct{}{}
		d.markProgress(clock)
	}
	if cfg.MaxRepeatedCalls <= 0 {
		return ""
	}
	switch {
	case d.repeatedCallCount >= cfg.MaxRepeatedCalls:
		return fmt.Sprintf("runaway detected: tool %q was called with identical arguments %d times in a row", tool, d.repeatedCallCount)
	case d.repeatedCallCount == cfg.MaxRepeatedCalls-1 && notices.fireOnce(dimensionRunaway+":"+sig, 1):
		notices.queue(domain.BudgetNoticePayload{
			Dimension: dimensionRunaway, Level: 1,
			Usage: int64(d.repeatedCallCount), Limit: int64(cfg.MaxRepeatedCalls),
			Message: noticeMessage(fmt.Sprintf(
				"[runaway warning] Tool %q was just called with identical arguments %d times in a row. Repeating it will not produce new information — change strategy (different arguments, a different tool, or report the blocker).",
				tool, d.repeatedCallCount,
			), clock.Now()),
		})
	}
	return ""
}

// trackExecResult feeds the consecutive-execution-failure detector and
// returns the termination reason when the streak hits the threshold.
// Only execution-phase outcomes count: prepare failures are the model
// struggling with a schema (covered by repeat detection instead), and
// successful calls reset the streak.
func (d *runawayDetector) trackExecResult(cfg domain.RunawayConfig, result domain.ToolResult) string {
	if result.Status == domain.ToolStatusError {
		d.consecutiveExecFailures++
	} else {
		d.consecutiveExecFailures = 0
	}
	if cfg.MaxConsecutiveFailures > 0 && d.consecutiveExecFailures >= cfg.MaxConsecutiveFailures {
		return fmt.Sprintf("runaway detected: %d consecutive tool executions failed", d.consecutiveExecFailures)
	}
	return ""
}

// markProgress records a progress signal: it arms the per-turn stall
// counter and re-anchors the stall watchdog's active-time baseline.
func (d *runawayDetector) markProgress(clock domain.Clock) {
	d.progressThisTurn = true
	d.lastProgressAt = clock.Now()
}

// compensateSuspend shifts the stall watchdog's baseline forward by the
// time spent suspended (approval waits, user-interactive tools): user
// thinking time is not agent activity (docs/CONTEXT_DESIGN.md §4.4.3).
func (d *runawayDetector) compensateSuspend(clock domain.Clock, since time.Time) {
	if !d.lastProgressAt.IsZero() {
		d.lastProgressAt = d.lastProgressAt.Add(clock.Since(since))
	}
}

// trackStall maintains the no-progress counter at the turn boundary
// (prepare). Any progress signal from the previous turn — visible text,
// a file change, a plan revision, or a first-seen tool signature —
// resets it; otherwise it grows, and every stall_warn_turns it injects a
// converge reminder (never a termination: wandering is corrected, not
// punished).
func (d *runawayDetector) trackStall(cfg domain.RunawayConfig, run *Run, notices *noticeCenter) {
	// Anchor the watchdog baseline at the first prepare even when the
	// turns-based warning is disabled: the two detectors are orthogonal.
	if d.lastProgressAt.IsZero() {
		d.lastProgressAt = run.Clock.Now()
	}
	if cfg.StallWarnTurns <= 0 {
		d.progressThisTurn = false
		return
	}
	// The first prepare has no previous turn to judge — consume the flag
	// without counting.
	if run.Usage.Turns <= 1 {
		d.progressThisTurn = false
		return
	}
	if d.progressThisTurn {
		d.stallTurns = 0
	} else {
		d.stallTurns++
	}
	d.progressThisTurn = false
	// The streak keeps growing after each reminder so the level
	// (stallTurns/StallWarnTurns) advances — resetting it here would pin
	// the level at 1 and fireOnce would suppress every later
	// reminder, defeating the "every stall_warn_turns" contract
	// (REVIEW H15).
	if d.stallTurns >= cfg.StallWarnTurns {
		level := d.stallTurns / cfg.StallWarnTurns
		if notices.fireOnce(dimensionRunaway+":stall", level) {
			run.AddBudgetNotice(domain.BudgetNoticePayload{
				Dimension: dimensionRunaway, Level: level,
				Usage: int64(d.stallTurns), Limit: int64(cfg.StallWarnTurns),
				Message: noticeMessage(fmt.Sprintf(
					"[runaway warning] The last %d turns produced no visible progress (no new information, no file changes, no plan movement). Stop re-exploring and either commit to a concrete next action or report what is blocking you.",
					d.stallTurns,
				), run.Clock.Now()),
			})
		}
	}
}

// stallActiveDuration reports the ACTIVE time since the last progress
// signal. Suspended time (approval waits) is compensated back into the
// baseline by awaitApproval, so user thinking time never counts
// (docs/CONTEXT_DESIGN.md §4.4.3). A zero baseline — no prepare has run
// yet — reports no stall.
func (d *runawayDetector) stallActiveDuration(clock domain.Clock) time.Duration {
	if d.lastProgressAt.IsZero() {
		return 0
	}
	return max(clock.Since(d.lastProgressAt), 0)
}

// stallExpired reports whether the stall watchdog fired: the active time
// since the last progress signal reached StallTimeout. Evaluated at the
// PhasePreparing boundary alongside the budget check — long tool
// executions and slow model responses count as active time, which the
// generous default (15m) accommodates. The caller (outside an active
// wrap-up) then enters the soft-landing wrap-up (dimension stall)
// instead of letting the run spin forever.
func (d *runawayDetector) stallExpired(cfg domain.RunawayConfig, clock domain.Clock) bool {
	return cfg.StallTimeout > 0 && d.stallActiveDuration(clock) >= cfg.StallTimeout
}
