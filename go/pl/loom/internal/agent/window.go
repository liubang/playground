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
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// fallbackCompactTargetTokens bounds the post-compaction transcript when no
// usable window is known (zero WindowModel, e.g. a model that declares no
// context window and no fallback configured). It exists solely to keep the
// zero value of Condenser safe; every production Loop builds a WindowModel
// from the model metadata or the limits fallback.
const fallbackCompactTargetTokens = 32_000

// WindowModel derives every context threshold from a model's context
// window (docs/CONTEXT_DESIGN.md §4.1). It is the single source of truth
// shared by the loop (compaction trigger, graduated notices) and the
// condenser (compaction target), replacing the former split between a
// hardcoded 32k condenser target and ad-hoc occupancy ratios.
//
// All thresholds scale with the model: a 200k-window model compacts at
// ~152k occupancy, a 65k-window model at ~49k — fixed absolute targets
// cannot express both.
type WindowModel struct {
	// Nominal is the model's declared context window in tokens (after the
	// fallback to Limits.MaxInputTokens when undeclared).
	Nominal int64
	// Effective is the safely usable window: Nominal × utilization.
	Effective int64
	// CompactTrigger is the occupancy at which automatic compaction fires.
	CompactTrigger int64
	// CompactTarget is the occupancy a compaction pass aims to get below.
	CompactTarget int64
	// NoticeLevels are the graduated occupancy-reminder thresholds,
	// ascending and all below CompactTrigger.
	NoticeLevels []int64
}

// NewWindowModel derives the thresholds. nominalWindow is the model's
// declared context window (0 = undeclared → fallbackWindow, typically
// Limits.MaxInputTokens). A zero result (both unknown) disables
// occupancy-driven compaction and notices; forced compaction after a
// provider overflow still works.
func NewWindowModel(nominalWindow, fallbackWindow int64, cfg domain.ContextConfig) WindowModel {
	nominal := nominalWindow
	if nominal <= 0 {
		nominal = fallbackWindow
	}
	if nominal <= 0 {
		return WindowModel{}
	}
	effective := int64(float64(nominal) * cfg.Utilization)
	model := WindowModel{
		Nominal:        nominal,
		Effective:      effective,
		CompactTrigger: int64(float64(effective) * cfg.CompactTriggerRatio),
		CompactTarget:  int64(float64(effective) * cfg.CompactTargetRatio),
		NoticeLevels:   make([]int64, 0, len(cfg.NoticeLevels)),
	}
	for _, level := range cfg.NoticeLevels {
		model.NoticeLevels = append(model.NoticeLevels, int64(float64(effective)*level))
	}
	return model
}

// Usable reports whether the model carries a usable window. A zero
// WindowModel disables occupancy-driven decisions (triggers, notices)
// while forced compaction keeps working.
func (m WindowModel) Usable() bool { return m.Effective > 0 }

// targetOrFallback returns the compaction target, keeping a zero
// WindowModel bounded instead of unbounded.
func (m WindowModel) targetOrFallback() int64 {
	if m.CompactTarget > 0 {
		return m.CompactTarget
	}
	return fallbackCompactTargetTokens
}
