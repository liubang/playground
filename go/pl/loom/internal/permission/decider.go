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
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// RuleDecider consults the file-layer rule set (builtin + user + project).
// It is first in the chain: an explicit rule — including a deny — always
// wins over heuristics, session memory, and the baseline.
type RuleDecider struct {
	Rules *RuleSet
}

// Evaluate returns the strictest matching rule's verdict, or nil when no
// rule matches. Process-spawning calls (run_cmd, exec_session — anything
// carrying a signed ExecRequest) match argv-prefix rules; web_fetch calls
// match host rules; everything else is not rule-eligible.
func (d RuleDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	if d.Rules == nil {
		return nil
	}
	if _, ok := ExecInfoOf(call); ok {
		return d.evaluateRunCmd(call)
	}
	if call.Call.Name == "web_fetch" {
		return d.evaluateWebFetch(call)
	}
	return nil
}

// evaluateWebFetch matches the call's URL host against the domain rules.
func (d RuleDecider) evaluateWebFetch(call domain.PreparedCall) *domain.Verdict {
	host, ok := ParseWebFetchHost(call.Call.Arguments)
	if !ok {
		return nil
	}
	best, rule := d.Rules.EvaluateDomain(host)
	if best == "" {
		return nil
	}
	return &domain.Verdict{Decision: best, Source: SourceRule, Reason: rule.Justification}
}

// evaluateRunCmd matches the call's argv against the prefix rules.
func (d RuleDecider) evaluateRunCmd(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	if !ok {
		return nil
	}
	best, rule := d.Rules.Evaluate(info.Argv)
	if best == "" {
		// Basename normalization lets absolute paths in trusted system
		// dirs hit bare-name rules (/bin/ls matches [ls]).
		if norm, ok := NormalizeTrustedPath(info.Argv); ok {
			best, rule = d.Rules.Evaluate(norm)
		}
	}
	if best == "" {
		return nil
	}
	grant := rule.Grant.ExecGrant()
	if best == domain.DecisionAllow && !AllowGrantCovers(grant, info) {
		// The matched allow's grant does not cover the capabilities this
		// call declared (an L0/L1 allow never answers an L2 escalation):
		// fall through to the baseline, which asks with the correct grant
		// stamped instead of silently running under-powered.
		return nil
	}
	v := &domain.Verdict{Decision: best, Source: SourceRule, Reason: rule.Justification}
	if best == domain.DecisionAllow {
		v.Grant = grant
	}
	return v
}

// AllowGrantCovers reports whether an allow verdict's grant satisfies the
// capabilities the call declared. Coverage is exact, by design: a lesser
// grant must never answer a bigger request, because the command then runs
// WITHOUT the capability it just said it needs — the failure looks
// identical to the sandbox denial that prompted the request, and the
// model cannot tell its escalation was refused (silent-downgrade
// fail-retry loop). Only an explicit unsandboxed grant covers an
// escalation; only a network (or unsandboxed) grant covers needs_network.
func AllowGrantCovers(grant domain.ExecGrant, info RunCmdCall) bool {
	switch {
	case info.Escalated:
		return grant.Unsandboxed
	case info.NeedsNetwork:
		return grant.Unsandboxed || grant.NetworkFull
	default:
		return true
	}
}

// DangerDecider intercepts commands matching the built-in dangerous
// heuristics. It sits after the rule layer (explicit user intent may
// approve a dangerous-looking command) and before session memory (a
// remembered prefix must never silence the danger screen).
type DangerDecider struct {
	// Mode selects the approval-mode-aware outcome: dangerous commands
	// ask in interactive modes and are denied outright in never mode
	// (an ask would hang an unattended run forever).
	Mode ApprovalMode
}

// Evaluate asks for (or, in never mode, denies) dangerous commands;
// otherwise it has no opinion.
func (d DangerDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	if !ok {
		return nil
	}
	reason := DangerousCommand(info.Argv)
	if reason == "" {
		return nil
	}
	if d.Mode == ModeNever {
		return &domain.Verdict{Decision: domain.DecisionDeny, Source: SourceDanger, Reason: reason}
	}
	v := &domain.Verdict{Decision: domain.DecisionAsk, Source: SourceDanger, Reason: reason}
	if info.Escalated {
		// An approved escalation must execute unsandboxed: the ask has to
		// carry the grant, or approval would silently run the command
		// sandboxed (see AllowGrantCovers).
		v.Grant = domain.ExecGrant{Unsandboxed: true}
	}
	return v
}

// SessionDecider consults prefixes remembered from interactive "allow
// always" decisions. It may only upgrade "no opinion" to allow — it sits
// after the rule and danger layers so it can never override them.
type SessionDecider struct {
	Session *SessionRules
}

// Evaluate returns an allow verdict when a session memory matches:
// remembered argv prefixes (with grant coverage) for run_cmd, remembered
// hosts for web_fetch.
func (d SessionDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	if d.Session == nil {
		return nil
	}
	info, execOk := ExecInfoOf(call)
	switch {
	case execOk:
		grant, ok := d.Session.Match(info.Argv)
		if !ok || !AllowGrantCovers(grant, info) {
			return nil
		}
		return &domain.Verdict{
			Decision: domain.DecisionAllow,
			Grant:    grant,
			Source:   SourceSession,
			Reason:   "remembered from an interactive loom approval",
		}
	case call.Call.Name == "web_fetch":
		host, ok := ParseWebFetchHost(call.Call.Arguments)
		if !ok || !d.Session.MatchDomain(host) {
			return nil
		}
		return &domain.Verdict{
			Decision: domain.DecisionAllow,
			Source:   SourceSession,
			Reason:   "remembered from an interactive loom approval",
		}
	}
	return nil
}

// BaselineDecider is the chain's terminal decider: it always has an
// opinion. Its behavior is selected by the approval mode (§4.3).
type BaselineDecider struct {
	Mode ApprovalMode
	// AutoApproveR1 automatically approves R0/R1 operations.
	AutoApproveR1 bool
	// AskR2 prompts for R2 operations (unless-trusted mode).
	AskR2 bool
	// DenyR4 denies R4 operations outright.
	DenyR4 bool
}

// Evaluate resolves the baseline verdict. run_cmd calls get mode-aware
// handling. In unless-dangerous mode every other tool is auto-allowed up
// to R2 as well: the built-in write/edit tools are confined to the
// workspace by the path validator (.git/.loom stay protected), so their
// blast radius is bounded the same way a sandboxed command's is.
func (d BaselineDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	v := &domain.Verdict{Source: SourceBaseline}
	if info, ok := ExecInfoOf(call); ok {
		return d.runCmdBaseline(v, call.Risk, info)
	}
	if d.Mode == ModeUnlessDangerous {
		switch {
		case call.Risk >= domain.R4 && d.DenyR4:
			v.Decision, v.Reason = domain.DecisionDeny, "R4 operations are denied by policy"
		case call.Risk >= domain.R3:
			v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: R3 operations require approval"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "unless-dangerous: workspace-confined tools run without prompting"
		}
		return v
	}
	v.Decision = d.riskBaseline(call.Risk)
	v.Reason = "risk baseline"
	return v
}

// runCmdBaseline maps (mode, request shape, risk) onto a verdict.
//
//   - escalated requests (require_escalated) run outside the sandbox, so
//     they always ask — except in never mode, which denies them outright.
//   - needs_network requests ask in interactive modes: widening the
//     sandbox's network boundary is a user decision (rememberable via
//     "allow always", which records the network grant). Never mode grants
//     it silently: unattended runs opted into autonomy.
//   - plain sandboxed commands (R0–R2) are auto-allowed in on-request and
//     never modes — the sandbox is the boundary.
func (d BaselineDecider) runCmdBaseline(v *domain.Verdict, risk domain.RiskLevel, info RunCmdCall) *domain.Verdict {
	switch d.Mode {
	case ModeNever:
		switch {
		case info.Escalated:
			v.Decision, v.Reason = domain.DecisionDeny, "never mode: escalated (unsandboxed) commands are denied"
		case risk >= domain.R3:
			v.Decision, v.Reason = domain.DecisionDeny, "never mode: R3+ commands are denied"
		case info.NeedsNetwork:
			v.Decision, v.Grant, v.Reason = domain.DecisionAllow, domain.ExecGrant{NetworkFull: true}, "never mode: declared network need granted"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "never mode: sandboxed commands run unattended"
		}
		return v

	case ModeUnlessDangerous:
		switch {
		case risk >= domain.R4 && d.DenyR4:
			v.Decision, v.Reason = domain.DecisionDeny, "R4 operations are denied by policy"
		case info.Escalated:
			// Leaving the sandbox always asks — the danger list is a
			// heuristic screen and cannot be the only line of defense
			// outside the sandbox.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{Unsandboxed: true}
			v.Reason = "command requests execution OUTSIDE the sandbox (full environment, network, credentials)"
		case risk >= domain.R3:
			v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: R3 operations require approval"
		case info.NeedsNetwork:
			// The sandbox keeps credential paths unreadable, so granting
			// declared network needs inside it adds no exfiltration value.
			v.Decision, v.Grant, v.Reason = domain.DecisionAllow, domain.ExecGrant{NetworkFull: true},
				"unless-dangerous: declared network need granted inside the sandbox"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "unless-dangerous: sandboxed commands run without prompting"
		}
		return v

	case ModeOnRequest:
		switch {
		case risk >= domain.R4 && d.DenyR4:
			v.Decision, v.Reason = domain.DecisionDeny, "R4 operations are denied by policy"
		case info.Escalated:
			// The ask carries the unsandboxed grant: approving this prompt
			// executes outside the sandbox exactly once.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{Unsandboxed: true}
			v.Reason = "command requests execution OUTSIDE the sandbox (full environment, network, credentials)"
		case risk >= domain.R3:
			v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: R3 operations require approval"
		case info.NeedsNetwork:
			// The ask carries the network grant: approving this prompt runs
			// the command sandboxed with outbound network, exactly once.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{NetworkFull: true}
			v.Reason = "command declares it needs outbound network (sandboxed, network granted on approval)"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "on-request: sandboxed commands run without prompting"
		}
		return v
	default: // ModeUnlessTrusted — the legacy behavior
		switch {
		case info.Escalated:
			// Escalated requests always ask (riskForArgs already rated the
			// call R3); the ask carries the unsandboxed grant.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{Unsandboxed: true}
			v.Reason = "command requests execution OUTSIDE the sandbox"
		case info.NeedsNetwork:
			// Declared network needs always ask in interactive modes —
			// allowing them with a zero grant would run the command without
			// the network it said it needs (fail-retry loop).
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{NetworkFull: true}
			v.Reason = "command declares it needs outbound network"
		default:
			v.Decision = d.riskBaseline(risk)
			v.Reason = "risk baseline"
		}
		return v
	}
}

// riskBaseline is the classic R0/R1 allow, R2/R3 ask, R4 deny mapping.
func (d BaselineDecider) riskBaseline(risk domain.RiskLevel) domain.Decision {
	switch {
	case risk <= domain.R1 && d.AutoApproveR1:
		return domain.DecisionAllow
	case risk == domain.R2 && d.AskR2:
		return domain.DecisionAsk
	case risk >= domain.R4 && d.DenyR4:
		return domain.DecisionDeny
	case risk == domain.R3:
		return domain.DecisionAsk
	default:
		return domain.DecisionDeny
	}
}
