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
	"strings"
	"testing"
	"time"
	"unicode/utf8"
)

func TestDefaultLimits(t *testing.T) {
	lim := DefaultLimits()
	if lim.MaxTokens != 0 {
		t.Error("MaxTokens should default to 0 (unlimited, opt-in)")
	}
	if lim.MaxEstimatedCostUSD <= 0 {
		t.Error("MaxEstimatedCostUSD should be positive")
	}
	if lim.MaxToolOutputBytes <= 0 {
		t.Error("MaxToolOutputBytes should be positive")
	}
	if lim.MaxInputTokens <= 0 || lim.MaxOutputTokens <= 0 {
		t.Error("token ceilings should be positive")
	}
}

func TestUsageCheckNoBreach(t *testing.T) {
	lim := Limits{MaxTokens: 1_000_000, MaxEstimatedCostUSD: 5.0}
	usage := Usage{Turns: 10, ToolCalls: 20, InputTokens: 1000, WallTime: time.Minute, CostUSD: 1.0}
	result := usage.Check(lim)
	if result.HasSoft() {
		t.Errorf("unexpected soft breach: %v", result.SoftBreaches)
	}
	if result.HasHard() {
		t.Errorf("unexpected hard breach: %v", result.HardBreaches)
	}
}

func TestUsageCheckIgnoresWorkloadCounters(t *testing.T) {
	// Turns/tool calls are per-prompt observability counters, never budget
	// dimensions (docs/CONTEXT_DESIGN.md §4.4.3): no count, however large,
	// may breach. (Tokens ARE a budget dimension — but MaxTokens is
	// unlimited here.)
	lim := Limits{MaxEstimatedCostUSD: 5.0}
	usage := Usage{Turns: 1 << 20, ToolCalls: 1 << 20, InputTokens: 1 << 40, OutputTokens: 1 << 40}
	if result := usage.Check(lim); result.HasSoft() || result.HasHard() {
		t.Errorf("workload counters must not breach budgets: %+v", result)
	}
}

func TestUsageCheckTokens(t *testing.T) {
	lim := Limits{MaxTokens: 100_000}
	usage := Usage{InputTokens: 80_000, OutputTokens: 10_000} // 90% → soft
	result := usage.Check(lim)
	if !result.HasSoft() {
		t.Error("expected soft breach for tokens")
	}
	if result.HasHard() {
		t.Error("unexpected hard breach")
	}

	hard := Usage{InputTokens: 90_000, OutputTokens: 10_000} // 100% → hard
	if result := hard.Check(lim); !result.HasHard() {
		t.Error("expected hard breach at 100%")
	}
}

func TestUsageCheckCost(t *testing.T) {
	lim := Limits{MaxEstimatedCostUSD: 5.0}
	usage := Usage{CostUSD: 4.5} // 90% → soft
	result := usage.Check(lim)
	if !result.HasSoft() {
		t.Error("expected soft breach for cost")
	}

	usage2 := Usage{CostUSD: 5.5} // 110% → hard
	result2 := usage2.Check(lim)
	if !result2.HasHard() {
		t.Error("expected hard breach for cost")
	}
}

func TestUsageCheckZeroLimit(t *testing.T) {
	lim := Limits{} // zero means unlimited on every dimension
	usage := Usage{InputTokens: 1 << 40, OutputTokens: 1 << 40, CostUSD: 99999}
	if result := usage.Check(lim); result.HasSoft() || result.HasHard() {
		t.Error("zero limit should not breach")
	}
}

func TestDefaultContextConfigPassesValidation(t *testing.T) {
	// Regression (CONTEXT_DESIGN review B1): the shipped defaults must
	// satisfy their own ordering invariants — a default that fails
	// validation bricks startup.
	if err := DefaultContextConfig().Validate(); err != nil {
		t.Fatalf("default context config must be valid: %v", err)
	}
}

func TestContextConfigValidate(t *testing.T) {
	valid := DefaultContextConfig()
	cases := []struct {
		name   string
		mutate func(*ContextConfig)
	}{
		{"utilization zero", func(c *ContextConfig) { c.Utilization = 0 }},
		{"utilization above one", func(c *ContextConfig) { c.Utilization = 1.5 }},
		{"trigger zero", func(c *ContextConfig) { c.CompactTriggerRatio = 0 }},
		{"trigger one", func(c *ContextConfig) { c.CompactTriggerRatio = 1 }},
		{"target equals trigger", func(c *ContextConfig) { c.CompactTargetRatio = c.CompactTriggerRatio }},
		{"target above trigger", func(c *ContextConfig) { c.CompactTargetRatio = 0.99 }},
		{"notice at trigger", func(c *ContextConfig) { c.NoticeLevels = []float64{0.60, 0.80} }},
		{"notice above trigger", func(c *ContextConfig) { c.NoticeLevels = []float64{0.90} }},
		{"notice not ascending", func(c *ContextConfig) { c.NoticeLevels = []float64{0.70, 0.60} }},
		{"notice non-positive", func(c *ContextConfig) { c.NoticeLevels = []float64{-0.1} }},
	}
	for _, tc := range cases {
		cfg := valid
		cfg.NoticeLevels = append([]float64(nil), valid.NoticeLevels...)
		tc.mutate(&cfg)
		if err := cfg.Validate(); err == nil {
			t.Errorf("%s: expected validation error", tc.name)
		}
	}
}

func TestDefaultRunawayConfigPassesValidation(t *testing.T) {
	if err := DefaultRunawayConfig().Validate(); err != nil {
		t.Fatalf("default runaway config must be valid: %v", err)
	}
	cfg := DefaultRunawayConfig()
	cfg.MaxRepeatedCalls = -1
	if err := cfg.Validate(); err == nil {
		t.Error("negative threshold must fail validation")
	}
}

func TestTruncateForErrorEcho(t *testing.T) {
	short := "short value"
	if got := TruncateForErrorEcho(short); got != short {
		t.Fatalf("short value = %q, want unchanged", got)
	}
	long := strings.Repeat("x", ToolErrorEchoMaxBytes+10)
	got := TruncateForErrorEcho(long)
	if len(got) != ToolErrorEchoMaxBytes+3 || !strings.HasSuffix(got, "...") {
		t.Fatalf("long value len = %d, want %d with ellipsis", len(got), ToolErrorEchoMaxBytes+3)
	}
	// Multi-byte characters must never be split mid-rune.
	multi := strings.Repeat("中", ToolErrorEchoMaxBytes) // 3 bytes per rune
	got = TruncateForErrorEcho(multi)
	if !utf8.ValidString(strings.TrimSuffix(got, "...")) {
		t.Fatal("truncated output split a multi-byte rune")
	}
}
