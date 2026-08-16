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
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Budget-notice dimensions. They double as WrapUpPending values for the
// resource dimensions; wrapUpGoalTokens is the goal wrap-up marker.
const (
	dimensionOccupancy = "occupancy"
	dimensionTokens    = "tokens"
	dimensionCostUSD   = "cost_usd"
	dimensionRunaway   = "runaway"
	// dimensionStall marks the stall watchdog's wrap-up: not a resource
	// budget, but it shares the soft-landing state machine so the run
	// still ends with a conclusion (docs/CONTEXT_DESIGN.md §4.4.3).
	dimensionStall = "stall"
	// dimensionMaxOutput marks the output-cap salvage wrap-up: the model
	// hit max_output_tokens repeatedly, so instead of paying another full
	// generation just to fail again it gets one final tools-denied turn
	// to conclude from what it has (docs/SUBAGENT_DESIGN.md §12).
	dimensionMaxOutput = "max_output"

	// wrapUpGoalTokens marks the goal token budget's wrap-up turn. Unlike
	// the resource dimensions it terminates with OutcomeSucceeded — the
	// run itself is healthy, only the goal's allowance ran out.
	wrapUpGoalTokens = "goal_tokens"
)

// notice level thresholds for the resource dimensions (tokens,
// cost_usd): level 1 advisory at 80%, level 2 wrap-up request at 95%.
const (
	noticeLevel1Ratio = 0.80
	noticeLevel2Ratio = 0.95
)

// microUSD scales a dollar amount into the integer usage/limit counters
// carried by notice payloads.
const microUSD = 1_000_000

// noticeText renders the model-facing reminder for one budget dimension.
func noticeText(dimension string, level int, usage, limit int64) string {
	switch dimension {
	case dimensionOccupancy:
		if level >= 2 {
			return fmt.Sprintf("[budget notice] Context occupancy is ~%s of ~%s tokens and auto-compaction is imminent. In your next visible reply, concisely capture critical state (confirmed findings, file paths, remaining steps) so it survives compaction.",
				humanizeTokens(usage), humanizeTokens(limit))
		}
		return fmt.Sprintf("[budget notice] Context occupancy is ~%s of ~%s tokens. Keep working, but narrow the scope: prefer concise replies and avoid re-reading large outputs.",
			humanizeTokens(usage), humanizeTokens(limit))
	case dimensionTokens:
		if level >= 2 {
			return fmt.Sprintf("[budget notice] The session token budget is nearly exhausted (~%s of ~%s tokens used). Converge now and prepare to wrap up.",
				humanizeTokens(usage), humanizeTokens(limit))
		}
		return fmt.Sprintf("[budget notice] The session token budget is ~%s of ~%s tokens used. Keep working, but converge on the remaining steps.",
			humanizeTokens(usage), humanizeTokens(limit))
	case dimensionCostUSD:
		if level >= 2 {
			return fmt.Sprintf("[budget notice] The cost budget is nearly exhausted ($%.2f of $%.2f used). Converge now and prepare to wrap up.",
				float64(usage)/microUSD, float64(limit)/microUSD)
		}
		return fmt.Sprintf("[budget notice] The cost budget is $%.2f of $%.2f used. Keep working, but converge on the remaining steps.",
			float64(usage)/microUSD, float64(limit)/microUSD)
	}
	return ""
}

// noticeCenter owns the graduated-reminder bookkeeping: each
// dimension+level fires at most once per prompt (compaction re-arms
// occupancy), and detections made at transcript-unsafe points (tool
// routing) queue for injection at the next pairing-safe prepare.
type noticeCenter struct {
	fired   map[string]int
	pending []domain.BudgetNoticePayload
}

// fireOnce records the highest fired level per dimension and reports
// whether the given level is new.
func (n *noticeCenter) fireOnce(dimension string, level int) bool {
	if n.fired == nil {
		n.fired = make(map[string]int)
	}
	if n.fired[dimension] >= level {
		return false
	}
	n.fired[dimension] = level
	return true
}

// queue defers a reminder detected at a transcript-unsafe point to the
// next prepare.
func (n *noticeCenter) queue(payload domain.BudgetNoticePayload) {
	n.pending = append(n.pending, payload)
}

// drain injects queued reminders at the pairing-safe point.
func (n *noticeCenter) drain(run *Run) {
	for _, payload := range n.pending {
		payload.Message.CreatedAt = run.Clock.Now()
		run.AddBudgetNotice(payload)
	}
	n.pending = n.pending[:0]
}

// rearm lets one dimension fire again. Compaction re-arms occupancy (the
// fresh window resets the pressure); other dimensions never re-arm.
func (n *noticeCenter) rearm(dimension string) {
	delete(n.fired, dimension)
}

// inject fires the graduated reminders whose thresholds the current usage
// just crossed: occupancy (window-derived levels), session tokens and
// cost (80%/95%). occupancy is consulted lazily, only with a usable
// window. Callers must be at a transcript-pairing-safe point (prepare).
func (n *noticeCenter) inject(run *Run, window WindowModel, occupancy func() int64) {
	if window.Usable() {
		current := occupancy()
		for level, threshold := range window.NoticeLevels {
			if current >= threshold && n.fireOnce(dimensionOccupancy, level+1) {
				run.AddBudgetNotice(domain.BudgetNoticePayload{
					Dimension: dimensionOccupancy, Level: level + 1,
					Usage: current, Limit: window.Effective,
					Message: noticeMessage(noticeText(dimensionOccupancy, level+1, current, window.Effective), run.Clock.Now()),
				})
				break // at most one level per dimension per injection point
			}
		}
	}
	n.injectScaled(run, dimensionTokens, run.Usage.InputTokens+run.Usage.OutputTokens, run.Limits.MaxTokens)
	n.injectScaled(run, dimensionCostUSD, int64(run.Usage.CostUSD*microUSD), int64(run.Limits.MaxEstimatedCostUSD*microUSD))
}

// injectScaled fires the 80%/95% reminders for one absolute-scaled
// dimension (session tokens, cost micro-USD).
func (n *noticeCenter) injectScaled(run *Run, dimension string, usage, limit int64) {
	if limit <= 0 {
		return
	}
	ratio := float64(usage) / float64(limit)
	var level int
	switch {
	case ratio >= noticeLevel2Ratio:
		level = 2
	case ratio >= noticeLevel1Ratio:
		level = 1
	default:
		return
	}
	if !n.fireOnce(dimension, level) {
		return
	}
	run.AddBudgetNotice(domain.BudgetNoticePayload{
		Dimension: dimension, Level: level, Usage: usage, Limit: limit,
		Message: noticeMessage(noticeText(dimension, level, usage, limit), run.Clock.Now()),
	})
}

// injectBudgetNotices fires the graduated reminders whose thresholds the
// current usage just crossed (see noticeCenter.inject).
func (l *Loop) injectBudgetNotices() {
	l.notices.inject(l.Run, l.Window, l.contextOccupancy)
}

// noticeMessage wraps reminder text in a system-role transcript message.
func noticeMessage(text string, now time.Time) domain.Message {
	return domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleSystem,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		CreatedAt: now,
		Metadata:  map[string]string{"kind": "system_note"},
	}
}

// --- soft landing (docs/CONTEXT_DESIGN.md §4.4.2) ---

// budgetWrapUpPrompt is the injected final-turn instruction: summarize
// instead of being cut off mid-work.
func budgetWrapUpPrompt(dimension string) string {
	if dimension == dimensionStall {
		return `The run appears stalled: no progress signal (no new information, no file changes, no plan movement) for longer than the configured stall timeout. This is your final turn.

Summarize now: what you accomplished, the conclusions you verified (with file paths), what you were stuck on, and a clear next step for the user.

Do not call any tools — further tool calls will be denied outright.`
	}
	if dimension == dimensionMaxOutput {
		return `Your response was cut off by the output token limit more than once — long uninterrupted generations keep failing. This is your final turn.

Conclude NOW with a concise summary based on what you already have: key findings with file paths, what is verified vs. uncertain, and what remains. Keep it short enough to fit comfortably in one response.

Do not call any tools — further tool calls will be denied outright.`
	}
	return fmt.Sprintf(`The run budget (%s) is exhausted. This is your final turn.

Summarize now: what you accomplished, the conclusions you verified (with file paths), what remains, and a clear next step for the user.

Do not call any tools — further tool calls will be denied outright.`, dimension)
}

// startBudgetWrapUp enters the soft-landing wrap-up: mark the run, record
// the auditable event (crash recovery re-arms from it), and inject the
// final-turn instruction. Called only at the PhasePreparing boundary.
func (l *Loop) startBudgetWrapUp(dimensions []string) {
	if len(dimensions) == 0 || l.Run.WrapUpPending != "" {
		return
	}
	dimension := dimensions[0]
	usage, limit := l.budgetDimensionUsage(dimension)
	l.Run.WrapUpPending = dimension
	l.Run.appendEvent(domain.EventBudgetWrapupStarted, domain.BudgetWrapupPayload{
		Dimension: dimension, Usage: usage, Limit: limit,
	})
	l.Run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: budgetWrapUpPrompt(dimension)}},
		CreatedAt: l.Run.Clock.Now(),
		Metadata:  map[string]string{"kind": "budget_wrapup"},
	})
	if l.Logger != nil {
		l.Logger.Warn("run budget exhausted; entering soft-landing wrap-up",
			"dimension", dimension, "usage", usage, "limit", limit)
	}
}

// budgetDimensionUsage reports the current usage and limit of one budget
// dimension in its natural integer scale.
func (l *Loop) budgetDimensionUsage(dimension string) (usage, limit int64) {
	switch dimension {
	case dimensionCostUSD:
		return int64(l.Run.Usage.CostUSD * microUSD), int64(l.Run.Limits.MaxEstimatedCostUSD * microUSD)
	case dimensionStall:
		return int64(l.runaway.stallActiveDuration(l.Run.Clock)), int64(l.runawayConfig().StallTimeout)
	case dimensionMaxOutput:
		return int64(l.maxOutputStops), int64(maxOutputContinuationLimit)
	default: // tokens
		return l.Run.Usage.InputTokens + l.Run.Usage.OutputTokens, l.Run.Limits.MaxTokens
	}
}

// inRunBudgetWrapUp reports whether the run is in the resource-budget
// wrap-up turn (as opposed to the goal wrap-up, which keeps its own
// semantics).
func (l *Loop) inRunBudgetWrapUp() bool {
	return l.Run.WrapUpPending != "" && l.Run.WrapUpPending != wrapUpGoalTokens
}

// wrapUpOutcome maps the wrap-up dimension to the terminal outcome: an
// exhausted resource budget is a normal soft landing, a stall is an
// abnormal ending (docs/CONTEXT_DESIGN.md §4.4.2), an output-cap
// salvage completes unverified — the run was healthy, only its
// generation style was not — and a goal-token wrap-up succeeds: the
// run itself is healthy, only the goal's allowance ran out.
func wrapUpOutcome(dimension string) domain.Outcome {
	switch dimension {
	case dimensionStall:
		return domain.OutcomeFailed
	case dimensionMaxOutput:
		return domain.OutcomeCompletedUnverified
	case wrapUpGoalTokens:
		return domain.OutcomeSucceeded
	}
	return domain.OutcomeBudgetExhausted
}

// humanizeTokens renders a token count in compact form ("152k", "1.5M").
func humanizeTokens(n int64) string {
	switch {
	case n >= 1_000_000:
		return fmt.Sprintf("%.1fM", float64(n)/1_000_000)
	case n >= 1_000:
		return fmt.Sprintf("%dk", n/1_000)
	default:
		return fmt.Sprintf("%d", n)
	}
}
