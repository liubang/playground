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
	dimensionWallTime  = "wall_time"
	dimensionCostUSD   = "cost_usd"
	dimensionRunaway   = "runaway"

	// wrapUpGoalTokens marks the goal token budget's wrap-up turn. Unlike
	// the resource dimensions it terminates with OutcomeSucceeded — the
	// run itself is healthy, only the goal's allowance ran out.
	wrapUpGoalTokens = "goal_tokens"
)

// notice level thresholds for the resource dimensions (wall_time,
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
	case dimensionWallTime:
		if level >= 2 {
			return fmt.Sprintf("[budget notice] The wall-clock budget is nearly exhausted (%s of %s used). Converge now and prepare to wrap up.",
				formatDurationShort(time.Duration(usage)), formatDurationShort(time.Duration(limit)))
		}
		return fmt.Sprintf("[budget notice] The wall-clock budget is %s of %s used. Keep working, but converge on the remaining steps.",
			formatDurationShort(time.Duration(usage)), formatDurationShort(time.Duration(limit)))
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

// injectBudgetNotices fires the graduated reminders whose thresholds the
// current usage just crossed: occupancy (window-derived levels), wall
// time and cost (80%/95%). Each dimension+level fires at most once per
// prompt; compaction re-arms occupancy. Callers must be at a
// transcript-pairing-safe point (prepare).
func (l *Loop) injectBudgetNotices() {
	if l.Window.Usable() {
		occupancy := l.contextOccupancy()
		for level, threshold := range l.Window.NoticeLevels {
			if occupancy >= threshold && l.fireNoticeOnce(dimensionOccupancy, level+1) {
				l.Run.AddBudgetNotice(domain.BudgetNoticePayload{
					Dimension: dimensionOccupancy, Level: level + 1,
					Usage: occupancy, Limit: l.Window.Effective,
					Message: noticeMessage(noticeText(dimensionOccupancy, level+1, occupancy, l.Window.Effective), l.Run.Clock.Now()),
				})
				break // at most one level per dimension per injection point
			}
		}
	}
	l.Run.touchWallTime()
	l.injectScaledNotice(dimensionWallTime, int64(l.Run.Usage.WallTime), int64(l.Run.Limits.MaxWallTime))
	l.injectScaledNotice(dimensionCostUSD, int64(l.Run.Usage.CostUSD*microUSD), int64(l.Run.Limits.MaxEstimatedCostUSD*microUSD))
}

// injectScaledNotice fires the 80%/95% reminders for one
// absolute-scaled dimension (wall time nanoseconds, cost micro-USD).
func (l *Loop) injectScaledNotice(dimension string, usage, limit int64) {
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
	if !l.fireNoticeOnce(dimension, level) {
		return
	}
	l.Run.AddBudgetNotice(domain.BudgetNoticePayload{
		Dimension: dimension, Level: level, Usage: usage, Limit: limit,
		Message: noticeMessage(noticeText(dimension, level, usage, limit), l.Run.Clock.Now()),
	})
}

// fireNoticeOnce records the highest fired level per dimension and
// reports whether the given level is new.
func (l *Loop) fireNoticeOnce(dimension string, level int) bool {
	if l.noticeFired == nil {
		l.noticeFired = make(map[string]int)
	}
	if l.noticeFired[dimension] >= level {
		return false
	}
	l.noticeFired[dimension] = level
	return true
}

// queueNotice defers a reminder detected at a transcript-unsafe point
// (tool routing) to the next prepare.
func (l *Loop) queueNotice(payload domain.BudgetNoticePayload) {
	l.pendingNotices = append(l.pendingNotices, payload)
}

// drainPendingNotices injects queued reminders at the pairing-safe point.
func (l *Loop) drainPendingNotices() {
	for _, payload := range l.pendingNotices {
		payload.Message.CreatedAt = l.Run.Clock.Now()
		l.Run.AddBudgetNotice(payload)
	}
	l.pendingNotices = l.pendingNotices[:0]
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
	l.Run.touchWallTime()
	switch dimension {
	case dimensionCostUSD:
		return int64(l.Run.Usage.CostUSD * microUSD), int64(l.Run.Limits.MaxEstimatedCostUSD * microUSD)
	default: // wall_time
		return int64(l.Run.Usage.WallTime), int64(l.Run.Limits.MaxWallTime)
	}
}

// inRunBudgetWrapUp reports whether the run is in the resource-budget
// wrap-up turn (as opposed to the goal wrap-up, which keeps its own
// semantics).
func (l *Loop) inRunBudgetWrapUp() bool {
	return l.Run.WrapUpPending != "" && l.Run.WrapUpPending != wrapUpGoalTokens
}

// formatDurationShort renders a duration in compact form ("12m30s", "1h5m").
func formatDurationShort(d time.Duration) string {
	if d < 0 {
		d = 0
	}
	return d.Truncate(time.Second).String()
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
