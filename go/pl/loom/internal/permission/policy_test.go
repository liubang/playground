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

package permission

import (
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestDefaultPolicyEvaluate(t *testing.T) {
	p := DefaultPolicy()

	tests := []struct {
		risk domain.RiskLevel
		want domain.Decision
	}{
		{domain.R0, domain.DecisionAllow},
		{domain.R1, domain.DecisionAllow},
		{domain.R2, domain.DecisionAsk},
		{domain.R3, domain.DecisionAsk},
		{domain.R4, domain.DecisionDeny},
	}

	for _, tt := range tests {
		got := p.Evaluate(tt.risk)
		if got != tt.want {
			t.Errorf("Evaluate(R%d) = %s, want %s", tt.risk, got, tt.want)
		}
	}
}

func TestPolicyAutoApproveDisabled(t *testing.T) {
	p := Policy{AutoApproveR1: false, AskR2: true, DenyR4: true}

	// With AutoApproveR1 disabled, R0/R1 should fall through to default (deny)
	got := p.Evaluate(domain.R0)
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R0) with AutoApproveR1=false = %s, want deny", got)
	}
}

func TestPolicyDenyR4Disabled(t *testing.T) {
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: false}

	// With DenyR4 disabled, R4 should fall through to default (deny)
	got := p.Evaluate(domain.R4)
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R4) with DenyR4=false = %s, want deny", got)
	}
}

func TestPolicyR3AlwaysAsk(t *testing.T) {
	p := Policy{AutoApproveR1: true, AskR2: true, DenyR4: true}
	got := p.Evaluate(domain.R3)
	if got != domain.DecisionAsk {
		t.Errorf("Evaluate(R3) = %s, want ask", got)
	}
}

func TestPolicyAskR2Disabled(t *testing.T) {
	p := Policy{AutoApproveR1: true, AskR2: false, DenyR4: true}

	// R2 with AskR2=false falls through to default (deny)
	got := p.Evaluate(domain.R2)
	if got != domain.DecisionDeny {
		t.Errorf("Evaluate(R2) with AskR2=false = %s, want deny", got)
	}
}
