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
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestNewWindowModelDerivesThresholds(t *testing.T) {
	model := NewWindowModel(200_000, 0, domain.DefaultContextConfig())
	if model.Nominal != 200_000 {
		t.Fatalf("Nominal = %d, want 200000", model.Nominal)
	}
	if model.Effective != 190_000 {
		t.Fatalf("Effective = %d, want 190000 (0.95 × nominal)", model.Effective)
	}
	if model.CompactTrigger != 152_000 {
		t.Fatalf("CompactTrigger = %d, want 152000 (0.80 × effective)", model.CompactTrigger)
	}
	if model.CompactTarget != 95_000 {
		t.Fatalf("CompactTarget = %d, want 95000 (0.50 × effective)", model.CompactTarget)
	}
	if len(model.NoticeLevels) != 2 || model.NoticeLevels[0] != 114_000 || model.NoticeLevels[1] != 142_500 {
		t.Fatalf("NoticeLevels = %v, want [114000 142500]", model.NoticeLevels)
	}
	for _, level := range model.NoticeLevels {
		if level >= model.CompactTrigger {
			t.Fatalf("notice level %d must sit below the trigger %d", level, model.CompactTrigger)
		}
	}
}

func TestNewWindowModelFallsBackToLimitsWindow(t *testing.T) {
	model := NewWindowModel(0, 128_000, domain.DefaultContextConfig())
	if model.Nominal != 128_000 {
		t.Fatalf("Nominal = %d, want the fallback 128000", model.Nominal)
	}
	if !model.Usable() {
		t.Fatal("fallback window must produce a usable model")
	}
}

func TestNewWindowModelZeroWhenNothingKnown(t *testing.T) {
	model := NewWindowModel(0, 0, domain.DefaultContextConfig())
	if model.Usable() {
		t.Fatal("zero windows must yield an unusable model")
	}
	// A zero model keeps the condenser bounded through the fallback.
	if model.targetOrFallback() != fallbackCompactTargetTokens {
		t.Fatalf("targetOrFallback = %d, want %d", model.targetOrFallback(), fallbackCompactTargetTokens)
	}
}

func TestNewWindowModelScalesWithSmallWindows(t *testing.T) {
	// The core design claim: thresholds follow the model, never a fixed
	// constant — a 65k model compacts proportionally earlier.
	model := NewWindowModel(65_536, 0, domain.DefaultContextConfig())
	if model.CompactTrigger >= 152_000 {
		t.Fatalf("small-window trigger = %d must scale below the 200k-model trigger", model.CompactTrigger)
	}
	want := int64(65_536*95/100) * 80 / 100
	if model.CompactTrigger < want-1 || model.CompactTrigger > want+1 {
		t.Fatalf("CompactTrigger = %d, want ~%d", model.CompactTrigger, want)
	}
}
