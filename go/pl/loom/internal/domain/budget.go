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

package domain

import (
	"fmt"
	"time"
)

// ContextConfig tunes how the agent derives context-compaction thresholds
// from a model's context window (docs/CONTEXT_DESIGN.md §4.1). Every
// threshold is a ratio of the effective window — absolute token constants
// are forbidden by design, because a fixed target misfires badly on models
// with much larger windows.
type ContextConfig struct {
	// Utilization is the fraction of the nominal context window treated as
	// safely usable (the rest is headroom for system prompt, tool
	// definitions and output).
	Utilization float64 `json:"utilization"`
	// CompactTriggerRatio is the occupancy fraction of the effective
	// window that triggers automatic compaction.
	CompactTriggerRatio float64 `json:"compact_trigger_ratio"`
	// CompactTargetRatio is the occupancy fraction a compaction pass aims
	// to get below.
	CompactTargetRatio float64 `json:"compact_target_ratio"`
	// NoticeLevels are the graduated occupancy-reminder thresholds, in
	// strictly ascending order and all below CompactTriggerRatio (a level
	// at or above the trigger could never fire: compaction happens first).
	NoticeLevels []float64 `json:"notice_levels"`
}

// DefaultContextConfig returns the standard context configuration. The
// defaults must pass Validate — that invariant is covered by tests.
func DefaultContextConfig() ContextConfig {
	return ContextConfig{
		Utilization:         0.95,
		CompactTriggerRatio: 0.80,
		CompactTargetRatio:  0.50,
		NoticeLevels:        []float64{0.60, 0.75},
	}
}

// Validate enforces the ordering invariants; violations are hard config
// errors at startup.
func (c ContextConfig) Validate() error {
	if c.Utilization <= 0 || c.Utilization > 1 {
		return fmt.Errorf("context.utilization must be in (0, 1], got %v", c.Utilization)
	}
	if c.CompactTriggerRatio <= 0 || c.CompactTriggerRatio >= 1 {
		return fmt.Errorf("context.compact_trigger_ratio must be in (0, 1), got %v", c.CompactTriggerRatio)
	}
	if c.CompactTargetRatio <= 0 || c.CompactTargetRatio >= c.CompactTriggerRatio {
		return fmt.Errorf("context.compact_target_ratio must be in (0, compact_trigger_ratio), got %v (trigger %v)",
			c.CompactTargetRatio, c.CompactTriggerRatio)
	}
	for i, level := range c.NoticeLevels {
		if level <= 0 || level >= c.CompactTriggerRatio {
			return fmt.Errorf("context.notice_levels[%d] must be in (0, compact_trigger_ratio), got %v", i, level)
		}
		if i > 0 && level <= c.NoticeLevels[i-1] {
			return fmt.Errorf("context.notice_levels must be strictly ascending, got %v", c.NoticeLevels)
		}
	}
	return nil
}

// RunawayConfig tunes behavior-based runaway detection
// (docs/CONTEXT_DESIGN.md §4.4.3). These are not budgets: they catch
// pathological behavior patterns (loops, stalls) instead of punishing
// workload size.
type RunawayConfig struct {
	// MaxRepeatedCalls is the consecutive repeat count of one
	// (tool, args_hash) signature that terminates the run. The repeat
	// before it earns a warning notice. Counts prepare failures too.
	MaxRepeatedCalls int `json:"max_repeated_calls"`
	// MaxConsecutiveFailures is the number of consecutive execution-phase
	// tool failures (prepare failures excluded) that terminates the run.
	MaxConsecutiveFailures int `json:"max_consecutive_failures"`
	// StallWarnTurns is the number of consecutive turns without any
	// progress signal that injects a converge reminder (0 disables).
	StallWarnTurns int `json:"stall_warn_turns"`
	// StallTimeout is the stall watchdog (docs/CONTEXT_DESIGN.md §4.4.3):
	// the maximum ACTIVE time since the last progress signal before the
	// run enters the soft-landing wrap-up and fails. Suspended time
	// (awaiting approval, waiting for user input) never counts. Zero
	// disables. This is the only role time plays — it answers "are you
	// stuck", never "how long have you worked".
	StallTimeout time.Duration `json:"stall_timeout"`
}

// DefaultRunawayConfig returns the standard runaway-detection thresholds.
func DefaultRunawayConfig() RunawayConfig {
	return RunawayConfig{
		MaxRepeatedCalls:       3,
		MaxConsecutiveFailures: 5,
		StallWarnTurns:         10,
		StallTimeout:           15 * time.Minute,
	}
}

// Validate rejects negative thresholds.
func (c RunawayConfig) Validate() error {
	if c.MaxRepeatedCalls < 0 || c.MaxConsecutiveFailures < 0 || c.StallWarnTurns < 0 || c.StallTimeout < 0 {
		return fmt.Errorf("runaway thresholds must be >= 0, got %+v", c)
	}
	return nil
}
