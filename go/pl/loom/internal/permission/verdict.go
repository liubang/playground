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
	SourceRule       = "rule"
	SourceSession    = "session"
	SourceDanger     = "danger"
	SourceBaseline   = "baseline"
	SourceUserIntent = "user_intent"
)

// Decider judges a prepared call (docs/PERMISSION_DESIGN.md §4.1). A nil
// verdict means "no opinion" — the chain consults the next decider. A
// non-nil verdict is final: chains are ordered strictest-first.
type Decider interface {
	Evaluate(call domain.PreparedCall) *domain.Verdict
}

// evalContext carries the per-evaluation parse result shared across the
// chain (REVIEW M33): resolving the exec shape of a run_cmd/exec_session
// call costs a JSON unmarshal plus a shell unwrap, and the built-in
// deciders would otherwise each repeat that work for the same call (up to
// four parses per policy evaluation). The URL host is similarly parsed
// once from the typed URLRequest field and shared across the chain
// (docs/BROWSER_DESIGN.md §5.3).
type evalContext struct {
	exec   RunCmdCall
	execOK bool
	url    URLInfo
	urlOK  bool
}

// contextDecider is the internal single-parse fast path: the built-in
// deciders implement it so Chain.Evaluate can hand down the once-parsed
// exec info. The exported Decider interface is unchanged — third-party
// deciders keep working and parse for themselves.
type contextDecider interface {
	evaluate(call domain.PreparedCall, ctx evalContext) *domain.Verdict
}

// Chain consults its deciders in order and returns the first non-nil
// verdict. A well-formed chain always ends with a BaselineDecider, which
// never returns nil; the deny fallback below only fires for a misassembled
// chain (fail closed).
type Chain []Decider

// Evaluate resolves the chain's verdict for a prepared call. The exec
// shape is parsed lazily and at most once per evaluation, then shared by
// every decider implementing the contextDecider fast path.
func (c Chain) Evaluate(call domain.PreparedCall) domain.Verdict {
	var (
		ctx        evalContext
		parsedExec bool
		parsedURL  bool
	)
	for _, d := range c {
		if d == nil {
			continue
		}
		var v *domain.Verdict
		if cd, ok := d.(contextDecider); ok {
			if !parsedExec {
				ctx.exec, ctx.execOK = ExecInfoOf(call)
				parsedExec = true
			}
			if !parsedURL {
				ctx.url, ctx.urlOK = URLInfoOf(call)
				parsedURL = true
			}
			v = cd.evaluate(call, ctx)
		} else {
			v = d.Evaluate(call)
		}
		if v != nil {
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
	// ModeOnRequest auto-allows everything the sandbox or the path
	// validator confines — sandboxed commands, workspace-confined file
	// writes — and prompts only for what crosses the boundary (escalation
	// out of the sandbox, network widening) or matches the danger screen.
	ModeOnRequest ApprovalMode = "on-request"
	// ModeUnlessDangerous additionally grants declared network needs
	// silently — including builtin tools whose only risk is egress to a
	// deployment-pinned endpoint (unlessDangerousSilentTools): the danger
	// screen plus per-call prompts for argument-shaped targets (hosts,
	// escalations, gui_open) are the only routine prompt sources.
	ModeUnlessDangerous ApprovalMode = "unless-dangerous"
	// ModeNever allows sandboxed calls (granting declared network needs)
	// and denies escalations, R3+ tools, and dangerous commands outright
	// — for unattended/CI runs. No verdict ever asks, so a run cannot
	// hang on a prompt nobody will answer.
	ModeNever ApprovalMode = "never"
)

// ParseApprovalMode validates a config value; empty selects the default.
func ParseApprovalMode(s string) (ApprovalMode, error) {
	switch ApprovalMode(s) {
	case "", ModeOnRequest:
		return ModeOnRequest, nil
	case ModeUnlessDangerous:
		return ModeUnlessDangerous, nil
	case ModeNever:
		return ModeNever, nil
	}
	return "", fmt.Errorf("approval.mode must be %q, %q, or %q, got %q",
		ModeOnRequest, ModeUnlessDangerous, ModeNever, s)
}
