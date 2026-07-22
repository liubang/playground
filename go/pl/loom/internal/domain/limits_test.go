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
