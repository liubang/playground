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
	"testing"
	"time"
)

func TestDefaultLimits(t *testing.T) {
	lim := DefaultLimits()
	if lim.MaxTurns <= 0 {
		t.Error("MaxTurns should be positive")
	}
	if lim.MaxParallelTools <= 0 {
		t.Error("MaxParallelTools should be positive")
	}
	if lim.MaxToolOutputBytes <= 0 {
		t.Error("MaxToolOutputBytes should be positive")
	}
}

func TestUsageCheckNoBreach(t *testing.T) {
	lim := Limits{MaxTurns: 100, MaxToolCalls: 200, MaxInputTokens: 100000}
	usage := Usage{Turns: 10, ToolCalls: 20, InputTokens: 1000}
	result := usage.Check(lim)
	if result.HasSoft() {
		t.Errorf("unexpected soft breach: %v", result.SoftBreaches)
	}
	if result.HasHard() {
		t.Errorf("unexpected hard breach: %v", result.HardBreaches)
	}
}

func TestUsageCheckSoftBreach(t *testing.T) {
	lim := Limits{MaxTurns: 100}
	usage := Usage{Turns: 82} // 82% of 100
	result := usage.Check(lim)
	if !result.HasSoft() {
		t.Error("expected soft breach")
	}
	if result.HasHard() {
		t.Error("unexpected hard breach")
	}
}

func TestUsageCheckHardBreach(t *testing.T) {
	lim := Limits{MaxTurns: 100}
	usage := Usage{Turns: 100} // 100% of 100
	result := usage.Check(lim)
	if !result.HasHard() {
		t.Error("expected hard breach")
	}
}

func TestUsageCheckZeroLimit(t *testing.T) {
	lim := Limits{MaxTurns: 0} // zero means unlimited
	usage := Usage{Turns: 999999}
	result := usage.Check(lim)
	if result.HasSoft() || result.HasHard() {
		t.Error("zero limit should not breach")
	}
}

func TestUsageCheckWallTime(t *testing.T) {
	lim := Limits{MaxWallTime: 10 * time.Minute}
	usage := Usage{WallTime: 9 * time.Minute} // 90% → soft
	result := usage.Check(lim)
	if !result.HasSoft() {
		t.Error("expected soft breach for wall time")
	}
	if result.HasHard() {
		t.Error("unexpected hard breach")
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

func TestLimitsFromEnvNoOverrides(t *testing.T) {
	base := DefaultLimits()
	got, err := LimitsFromEnv(base)
	if err != nil {
		t.Fatalf("LimitsFromEnv() error = %v", err)
	}
	if got != base {
		t.Errorf("LimitsFromEnv() = %+v, want unchanged base", got)
	}
}

func TestLimitsFromEnvAppliesOverrides(t *testing.T) {
	t.Setenv(EnvMaxTurns, "99")
	t.Setenv(EnvMaxInputTokens, "500000")
	t.Setenv(EnvMaxCostUSD, "12.5")
	t.Setenv(EnvMaxWallTime, "45m")
	t.Setenv(EnvMaxToolOutputBytes, "131072")

	got, err := LimitsFromEnv(DefaultLimits())
	if err != nil {
		t.Fatalf("LimitsFromEnv() error = %v", err)
	}
	if got.MaxTurns != 99 {
		t.Errorf("MaxTurns = %d, want 99", got.MaxTurns)
	}
	if got.MaxInputTokens != 500000 {
		t.Errorf("MaxInputTokens = %d, want 500000", got.MaxInputTokens)
	}
	if got.MaxEstimatedCostUSD != 12.5 {
		t.Errorf("MaxEstimatedCostUSD = %f, want 12.5", got.MaxEstimatedCostUSD)
	}
	if got.MaxWallTime != 45*time.Minute {
		t.Errorf("MaxWallTime = %v, want 45m", got.MaxWallTime)
	}
	if got.MaxToolOutputBytes != 131072 {
		t.Errorf("MaxToolOutputBytes = %d, want 131072", got.MaxToolOutputBytes)
	}
	// Untouched fields keep the defaults.
	if got.MaxToolCalls != DefaultLimits().MaxToolCalls {
		t.Errorf("MaxToolCalls = %d, want default %d", got.MaxToolCalls, DefaultLimits().MaxToolCalls)
	}
}

func TestLimitsFromEnvRejectsMalformed(t *testing.T) {
	tests := []struct {
		name string
		env  string
		val  string
	}{
		{"non-integer tokens", EnvMaxInputTokens, "lots"},
		{"negative turns", EnvMaxTurns, "-1"},
		{"bad duration", EnvMaxWallTime, "soon"},
		{"negative cost", EnvMaxCostUSD, "-2"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Setenv(tt.env, tt.val)
			if _, err := LimitsFromEnv(DefaultLimits()); err == nil {
				t.Fatalf("expected error for %s=%q", tt.env, tt.val)
			}
		})
	}
}
