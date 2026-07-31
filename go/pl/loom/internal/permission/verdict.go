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
// Created: 2026/07/27

package permission

import (
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Verdict provenance labels recorded in audits and surfaced in the UI.
const (
	SourceRule     = "rule"
	SourceSession  = "session"
	SourceDanger   = "danger"
	SourceBaseline = "baseline"
)

// Decider judges a prepared call (docs/PERMISSION_DESIGN.md §4.1). A nil
// verdict means "no opinion" — the chain consults the next decider. A
// non-nil verdict is final: chains are ordered strictest-first.
type Decider interface {
	Evaluate(call domain.PreparedCall) *domain.Verdict
}

// Chain consults its deciders in order and returns the first non-nil
// verdict. A well-formed chain always ends with a BaselineDecider, which
// never returns nil; the deny fallback below only fires for a misassembled
// chain (fail closed).
type Chain []Decider

// Evaluate resolves the chain's verdict for a prepared call.
func (c Chain) Evaluate(call domain.PreparedCall) domain.Verdict {
	for _, d := range c {
		if d == nil {
			continue
		}
		if v := d.Evaluate(call); v != nil {
			return *v
		}
	}
	return domain.Verdict{
		Decision: domain.DecisionDeny,
		Source:   SourceBaseline,
		Reason:   "policy chain produced no verdict (fail closed)",
	}
}

// ApprovalMode selects the baseline strategy consulted when no rule,
// danger heuristic, or session memory has an opinion (§4.3).
type ApprovalMode string

const (
	// ModeOnRequest auto-allows sandboxed, non-dangerous commands: the
	// sandbox is the boundary, so asking the user adds no safety.
	ModeOnRequest ApprovalMode = "on-request"
	// ModeUnlessTrusted asks for every unmatched R2+ call (legacy default).
	ModeUnlessTrusted ApprovalMode = "unless-trusted"
	// ModeNever allows sandboxed calls (granting declared network needs)
	// and denies escalations outright — for unattended/CI runs.
	ModeNever ApprovalMode = "never"
)

// ParseApprovalMode validates a config value; empty selects the default.
func ParseApprovalMode(s string) (ApprovalMode, error) {
	switch ApprovalMode(s) {
	case "", ModeOnRequest:
		return ModeOnRequest, nil
	case ModeUnlessTrusted:
		return ModeUnlessTrusted, nil
	case ModeNever:
		return ModeNever, nil
	}
	return "", fmt.Errorf("approval.mode must be %q, %q, or %q, got %q",
		ModeOnRequest, ModeUnlessTrusted, ModeNever, s)
}
