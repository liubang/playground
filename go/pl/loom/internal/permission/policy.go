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
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PolicyDecision represents allow/deny/ask.
type PolicyDecision = domain.Decision

// Policy evaluates tool calls against security policy.
type Policy struct {
	// AutoApproveR1 automatically approves R0 and R1 risk operations.
	AutoApproveR1 bool
	// AskR2 prompts the user for R2 operations (default: true).
	AskR2 bool
	// DenyR4 denies R4 operations by default.
	DenyR4 bool
}

// DefaultPolicy returns the baseline security policy per §12.1.
func DefaultPolicy() Policy {
	return Policy{
		AutoApproveR1: true,
		AskR2:         true,
		DenyR4:        true,
	}
}

// Evaluate returns the policy decision for a given risk level.
func (p Policy) Evaluate(risk domain.RiskLevel) PolicyDecision {
	switch {
	case risk <= domain.R1 && p.AutoApproveR1:
		return domain.DecisionAllow
	case risk == domain.R2 && p.AskR2:
		return domain.DecisionAsk
	case risk >= domain.R4 && p.DenyR4:
		return domain.DecisionDeny
	case risk == domain.R3:
		return domain.DecisionAsk
	default:
		return domain.DecisionDeny
	}
}
