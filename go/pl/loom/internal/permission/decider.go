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

// The built-in deciders all implement the single-parse fast path
// (REVIEW M33). These assertions keep a future refactor from silently
// dropping it — behavior would stay correct while the repeated parses
// creep back in.
var (
	_ contextDecider = RuleDecider{}
	_ contextDecider = DangerDecider{}
	_ contextDecider = SessionDecider{}
	_ contextDecider = BaselineDecider{}
)

// Evaluate returns the strictest matching rule's verdict, or nil when no
// rule matches. Process-spawning calls (run_cmd, exec_session — anything
// carrying a signed ExecRequest) match argv-prefix rules; web_fetch calls
// match host rules; other tools match tool-name rules (tool_rules.go) —
// which is why run_cmd/web_fetch can never be remembered by tool name:
// they never reach the tool-rule evaluation.
func (d RuleDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok})
}

// evaluate implements the contextDecider fast path (REVIEW M33): the
// chain hands down the once-parsed exec info instead of re-parsing here.
func (d RuleDecider) evaluate(call domain.PreparedCall, ctx evalContext) *domain.Verdict {
	if d.Rules == nil {
		return nil
	}
	if ctx.execOK {
		return d.evaluateRunCmd(ctx.exec)
	}
	if call.Call.Name == "web_fetch" {
		return d.evaluateWebFetch(call)
	}
	return d.evaluateTool(call)
}

// evaluateTool matches the call's tool name against the tool rules.
func (d RuleDecider) evaluateTool(call domain.PreparedCall) *domain.Verdict {
	best, rule := d.Rules.EvaluateTool(call.Call.Name)
	if best == "" {
		return nil
	}
	return &domain.Verdict{Decision: best, Source: SourceRule, Reason: rule.Justification}
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

// evaluateRunCmd matches the call's argv against the prefix rules. A
// composed shell command is evaluated PER SUBCOMMAND: an allow verdict
// requires every subcommand covered by an allow rule (codex's
// bypass-only-when-all-matched rule), while a single deny/ask hit
// decides the whole script.
func (d RuleDecider) evaluateRunCmd(info RunCmdCall) *domain.Verdict {
	if info.Shell != nil {
		return d.evaluateShellCommands(info)
	}
	best, rule := d.matchArgv(info.Argv)
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

// matchArgv evaluates one argv against the rule set, with trusted-path
// basename normalization as the fallback (/bin/ls matches [ls]).
func (d RuleDecider) matchArgv(argv []string) (domain.Decision, Rule) {
	best, rule := d.Rules.Evaluate(argv)
	if best == "" {
		if norm, ok := NormalizeTrustedPath(argv); ok {
			best, rule = d.Rules.Evaluate(norm)
		}
	}
	return best, rule
}

// evaluateShellCommands applies the prefix rules to every subcommand of a
// statically-analyzed shell script. Dynamic scripts get no rule opinion —
// an unproven argv must never match an allow rule — and fall through to
// the sandbox-backed baseline. Scripts with file-writing redirects also
// get no rule opinion: an argv rule certifies the ARGV only
// ("echo is read-only"), while the script's actual effect includes the
// redirect target — that judgment belongs to the danger screen, which
// runs next in the chain.
func (d RuleDecider) evaluateShellCommands(info RunCmdCall) *domain.Verdict {
	if !info.Shell.Static || info.Shell.DynamicWrites || len(info.Shell.WriteRedirects) > 0 {
		return nil
	}
	argvs, ok := info.ShellCommandArgvs()
	if !ok {
		return nil
	}
	var union domain.ExecGrant
	for _, argv := range argvs {
		best, rule := d.matchArgv(argv)
		switch best {
		case domain.DecisionDeny, domain.DecisionAsk:
			return &domain.Verdict{Decision: best, Source: SourceRule, Reason: rule.Justification}
		case domain.DecisionAllow:
			grant := rule.Grant.ExecGrant()
			union.Unsandboxed = union.Unsandboxed || grant.Unsandboxed
			union.NetworkFull = union.NetworkFull || grant.NetworkFull
			union.WritablePaths = append(union.WritablePaths, grant.WritablePaths...)
		default:
			// One uncovered subcommand keeps the whole script at the
			// baseline verdict.
			return nil
		}
	}
	if !AllowGrantCovers(union, info) {
		return nil
	}
	return &domain.Verdict{
		Decision: domain.DecisionAllow,
		Grant:    union,
		Source:   SourceRule,
		Reason:   "every subcommand matched an allow rule",
	}
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
	return d.evaluate(call, evalContext{exec: info, execOK: ok})
}

// evaluate implements the contextDecider fast path (REVIEW M33).
// Composed shell scripts go through the script-level screen (which also
// runs every subcommand through the argv-level screen), so a dangerous
// literal hiding in a pipe, substitution, or subshell is still caught.
func (d DangerDecider) evaluate(_ domain.PreparedCall, ctx evalContext) *domain.Verdict {
	if !ctx.execOK {
		return nil
	}
	info := ctx.exec
	var reason string
	if info.Shell != nil {
		reason = DangerousScript(info.Shell)
	} else {
		reason = DangerousCommand(info.Argv)
	}
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
// hosts for web_fetch, remembered tool names for the eligible
// fixed-blast-radius tools (SessionRules.RememberTool gates eligibility).
func (d SessionDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok})
}

// evaluate implements the contextDecider fast path (REVIEW M33).
func (d SessionDecider) evaluate(call domain.PreparedCall, ctx evalContext) *domain.Verdict {
	if d.Session == nil {
		return nil
	}
	switch {
	case ctx.execOK:
		var (
			grant domain.ExecGrant
			ok    bool
		)
		if ctx.exec.Shell != nil {
			// A composed command is covered by memory only when fully
			// static and EVERY subcommand is remembered (MatchAll).
			if !ctx.exec.Shell.Static || ctx.exec.Shell.DynamicWrites {
				return nil
			}
			argvs, provable := ctx.exec.ShellCommandArgvs()
			if !provable {
				return nil
			}
			grant, ok = d.Session.MatchAll(argvs)
		} else {
			grant, ok = d.Session.Match(ctx.exec.Argv)
		}
		if !ok || !AllowGrantCovers(grant, ctx.exec) {
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
	default:
		if !d.Session.MatchTool(call.Call.Name) {
			return nil
		}
		return &domain.Verdict{
			Decision: domain.DecisionAllow,
			Source:   SourceSession,
			Reason:   "remembered from an interactive loom approval",
		}
	}
}

// BaselineDecider is the chain's terminal decider: it always has an
// opinion. Its behavior is selected by the approval mode (§4.3).
type BaselineDecider struct {
	Mode ApprovalMode
}

// Evaluate resolves the baseline verdict.
func (d BaselineDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok})
}

// evaluate implements the contextDecider fast path (REVIEW M33).
//
// The governing principle: the sandbox and the path validator are the
// boundary, so operations confined by them never prompt — approvals are
// reserved for what crosses the boundary (escalation, network widening)
// or what the danger screen flagged. Built-in file tools are confined to
// the workspace by the path validator (.git/.loom stay protected), so
// their blast radius is bounded the same way a sandboxed command's is:
// R0–R2 run without prompting in every mode. MCP tools are third-party
// code whose arguments shape their effect; they keep per-call approvals
// (rememberable by tool name) and are denied in never mode.
func (d BaselineDecider) evaluate(call domain.PreparedCall, ctx evalContext) *domain.Verdict {
	v := &domain.Verdict{Source: SourceBaseline}
	if ctx.execOK {
		return d.runCmdBaseline(v, ctx.exec)
	}
	if call.Definition.Source == domain.ToolSourceMCP {
		switch {
		case call.Risk <= domain.R1:
			v.Decision, v.Reason = domain.DecisionAllow, "baseline: read-only MCP tool"
		case d.Mode == ModeNever:
			v.Decision, v.Reason = domain.DecisionDeny,
				"never mode: third-party (MCP) tools are denied unattended; remember the tool with allow always in an interactive session"
		default:
			v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: third-party (MCP) tools require approval"
		}
		return v
	}
	switch {
	case call.Risk <= domain.R2:
		v.Decision, v.Reason = domain.DecisionAllow, "baseline: workspace-confined tool runs without prompting"
	case d.Mode == ModeNever:
		v.Decision, v.Reason = domain.DecisionDeny,
			"never mode: R3+ operations are denied unattended; rework the approach to use workspace-confined operations"
	case call.Call.Name == "web_fetch":
		v.Decision, v.Reason = domain.DecisionAsk,
			"risk baseline: fetching an unapproved host requires approval (the user can remember the host, or add a *.domain rule)"
	default:
		v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: R3+ operations require approval"
	}
	return v
}

// runCmdBaseline maps (mode, request shape) onto a verdict.
//
//   - escalated requests (require_escalated) run outside the sandbox, so
//     they always ask — except in never mode, which denies them outright.
//   - needs_network requests ask in on-request mode (widening the
//     sandbox's network boundary is a user decision, rememberable via
//     "allow always"); unless-dangerous and never grant it silently —
//     the sandbox keeps credential paths unreadable either way.
//   - everything else runs sandboxed without prompting in every mode:
//     the sandbox is the boundary. Dangerous commands never reach here
//     (the DangerDecider already asked or denied them).
func (d BaselineDecider) runCmdBaseline(v *domain.Verdict, info RunCmdCall) *domain.Verdict {
	switch d.Mode {
	case ModeNever:
		switch {
		case info.Escalated:
			v.Decision, v.Reason = domain.DecisionDeny,
				"never mode: escalated (unsandboxed) commands are denied; rework the command to run inside the sandbox"
		case info.NeedsNetwork:
			v.Decision, v.Grant, v.Reason = domain.DecisionAllow, domain.ExecGrant{NetworkFull: true}, "never mode: declared network need granted"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "never mode: sandboxed commands run unattended"
		}
		return v

	case ModeUnlessDangerous:
		switch {
		case info.Escalated:
			// Leaving the sandbox always asks — the danger list is a
			// heuristic screen and cannot be the only line of defense
			// outside the sandbox.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{Unsandboxed: true}
			v.Reason = "command requests execution OUTSIDE the sandbox (full environment, network, credentials)"
		case info.NeedsNetwork:
			// The sandbox keeps credential paths unreadable, so granting
			// declared network needs inside it adds no exfiltration value.
			v.Decision, v.Grant, v.Reason = domain.DecisionAllow, domain.ExecGrant{NetworkFull: true},
				"unless-dangerous: declared network need granted inside the sandbox"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "unless-dangerous: sandboxed commands run without prompting"
		}
		return v

	default: // ModeOnRequest
		switch {
		case info.Escalated:
			// The ask carries the unsandboxed grant: approving this prompt
			// executes outside the sandbox exactly once.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{Unsandboxed: true}
			v.Reason = "command requests execution OUTSIDE the sandbox (full environment, network, credentials)"
		case info.NeedsNetwork:
			// The ask carries the network grant: approving this prompt runs
			// the command sandboxed with outbound network, exactly once.
			v.Decision, v.Grant = domain.DecisionAsk, domain.ExecGrant{NetworkFull: true}
			v.Reason = "command declares it needs outbound network (sandboxed, network granted on approval)"
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "on-request: sandboxed commands run without prompting"
		}
		return v
	}
}
