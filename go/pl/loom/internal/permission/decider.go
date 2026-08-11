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
// carrying a signed ExecRequest) match argv-prefix rules; URL-fetching
// calls (web_fetch, browser navigate — anything carrying a signed
// URLRequest) match host rules; other tools match tool-name rules
// (tool_rules.go) — which is why run_cmd/web_fetch can never be remembered
// by tool name: they never reach the tool-rule evaluation.
func (d RuleDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := ExecInfoOf(call)
	urlInfo, urlOK := URLInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok, url: urlInfo, urlOK: urlOK})
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
	if ctx.urlOK {
		return d.evaluateURL(ctx.url)
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

// evaluateURL matches the call's URL host against the domain rules.
func (d RuleDecider) evaluateURL(info URLInfo) *domain.Verdict {
	best, rule := d.Rules.EvaluateDomain(info.Host)
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
//
// When the call declared needs_network, domain rules are additionally
// applied to any network targets extracted from the argv (URLs or bare
// hostnames): a domain deny or ask overrides an argv allow, so a
// rule-level "allow go mod download" cannot silently reach a denied
// host. This brings run_cmd network egress under the same host
// allow/deny list that governs web_fetch (docs/PERMISSION_DESIGN.md
// §6 — on-request network filtering).
func (d RuleDecider) evaluateRunCmd(info RunCmdCall) *domain.Verdict {
	if info.Shell != nil {
		return d.evaluateShellCommands(info)
	}
	best, rule := d.matchArgv(info.Argv)
	if best == "" {
		// No argv rule matched. When the call declared needs_network,
		// domain rules still apply to any extracted hosts — a deny rule
		// on a host must block even an uncovered command.
		if info.NeedsNetwork {
			if v := d.evaluateNetworkHosts(info); v != nil {
				return v
			}
		}
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
	// When the call declared needs_network, domain rules may override
	// the argv verdict: a deny beats an allow, an ask beats an allow.
	if info.NeedsNetwork {
		if dv := d.evaluateNetworkHosts(info); dv != nil {
			if decisionStrictness(dv.Decision) > decisionStrictness(v.Decision) {
				return dv
			}
		}
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
//
// When the call declared needs_network, domain rules are additionally
// applied to any network targets extracted from the full script (all
// subcommands are scanned). A domain deny/ask overrides the argv allow,
// same as the plain-argv path.
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
			union.GUIOpen = union.GUIOpen || grant.GUIOpen
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
	v := &domain.Verdict{
		Decision: domain.DecisionAllow,
		Grant:    union,
		Source:   SourceRule,
		Reason:   "every subcommand matched an allow rule",
	}
	if info.NeedsNetwork {
		if dv := d.evaluateNetworkHosts(info); dv != nil {
			if decisionStrictness(dv.Decision) > decisionStrictness(v.Decision) {
				return dv
			}
		}
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
// escalation; only a network (or unsandboxed) grant covers needs_network;
// only a gui_open (or unsandboxed) grant covers needs_gui_open. Declared
// capabilities are checked independently so a call may declare several
// at once (needs_network + needs_gui_open).
func AllowGrantCovers(grant domain.ExecGrant, info RunCmdCall) bool {
	if info.Escalated && !grant.Unsandboxed {
		return false
	}
	if info.NeedsNetwork && !grant.Unsandboxed && !grant.NetworkFull {
		return false
	}
	if info.NeedsGUIOpen && !grant.Unsandboxed && !grant.GUIOpen {
		return false
	}
	return true
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
	// An approved ask must execute with the capabilities the call
	// declared: the verdict has to carry the grant, or approval would
	// silently run the command under-powered (see AllowGrantCovers) —
	// the failure then looks identical to the denial that prompted the
	// request and the model retries in a loop. This covers every declared
	// capability, not just escalation (review M9).
	v.Grant = DeclaredGrant(info)
	return v
}

// DeclaredGrant computes the grant covering exactly the capabilities a
// call declared: unsandboxed for escalations, otherwise the declared
// network/gui widenings (zero grant when nothing was declared).
func DeclaredGrant(info RunCmdCall) domain.ExecGrant {
	if info.Escalated {
		return domain.ExecGrant{Unsandboxed: true}
	}
	return domain.ExecGrant{NetworkFull: info.NeedsNetwork, GUIOpen: info.NeedsGUIOpen}
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
	urlInfo, urlOK := URLInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok, url: urlInfo, urlOK: urlOK})
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
	case ctx.urlOK:
		if !d.Session.MatchDomain(ctx.url.Host) {
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
	urlInfo, urlOK := URLInfoOf(call)
	return d.evaluate(call, evalContext{exec: info, execOK: ok, url: urlInfo, urlOK: urlOK})
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
	case d.Mode == ModeUnlessDangerous && unlessDangerousSilent(call):
		v.Decision, v.Reason = domain.DecisionAllow,
			"unless-dangerous: pinned-endpoint network tool runs without prompting"
	case d.Mode == ModeUnlessDangerous && unlessDangerousWebFetch(call):
		v.Decision, v.Reason = domain.DecisionAllow,
			"unless-dangerous: builtin web_fetch runs without prompting (deny/ask domain rules still apply)"
	case d.Mode == ModeNever:
		v.Decision, v.Reason = domain.DecisionDeny,
			"never mode: R3+ operations are denied unattended; rework the approach to use workspace-confined operations"
	case ctx.urlOK:
		v.Decision, v.Reason = domain.DecisionAsk,
			"risk baseline: fetching an unapproved host requires approval (the user can remember the host, or add a *.domain rule)"
	default:
		v.Decision, v.Reason = domain.DecisionAsk, "risk baseline: R3+ operations require approval"
	}
	return v
}

// unlessDangerousSilentTools are the builtin R3+ tools whose ONLY risk is
// network egress to a DEPLOYMENT-PINNED endpoint — the target host is
// chosen by process configuration, never shaped by call arguments. The
// unless-dangerous contract already grants declared network needs
// silently (runCmdBaseline: "the sandbox keeps credential paths
// unreadable, so granting declared network needs inside it adds no
// exfiltration value"), and that argument holds a fortiori here: a
// sandboxed needs_network command may reach ANY host, while these tools
// can only reach their pinned one. on-request still asks (crossing the
// network boundary is a user decision there — rememberable via "allow
// always", and web_search is tool-memory eligible); never still denies
// unattended. generate_image is deliberately ABSENT: its per-call
// provider quota cost is part of what the prompt pays for.
var unlessDangerousSilentTools = map[string]struct{}{
	// The search backend is pinned at process start by env config
	// (BRAVE_SEARCH_API_KEY / TAVILY_API_KEY / LOOM_WEB_SEARCH_PROVIDER →
	// hardcoded provider URLs); arguments are query/count/timeout only and
	// the SSRF dial guard keeps the DNS answer honest.
	"web_search": {},
}

// unlessDangerousSilent reports whether a call is a builtin tool on the
// pinned-endpoint silent list. The source check is defense in depth: a
// same-named tool from another source must never inherit the silence.
func unlessDangerousSilent(call domain.PreparedCall) bool {
	if call.Definition.Source != domain.ToolSourceBuiltin {
		return false
	}
	_, ok := unlessDangerousSilentTools[call.Call.Name]
	return ok
}

// unlessDangerousWebFetch reports whether a builtin web_fetch call may run
// silently in unless-dangerous mode. Unlike web_search, the target host is
// argument-shaped — but the mode's network contract already grants a
// sandboxed needs_network command egress to ANY host silently, and web_fetch
// is strictly weaker than that: a credential-less anonymous GET that never
// carries the user's browser identity or cookies, whose SSRF dial guard
// blocks private/loopback/link-local targets unless the model explicitly
// sets allow_private=true. Deny/ask domain rules are evaluated earlier in
// the chain and keep their force, so the user's explicit blacklist still
// wins. browser navigate is deliberately NOT included: it drives the real
// user browser with its real identity and cookies.
func unlessDangerousWebFetch(call domain.PreparedCall) bool {
	if call.Definition.Source != domain.ToolSourceBuiltin {
		return false
	}
	return call.Call.Name == "web_fetch"
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
//
// declaredGrantReason renders the baseline reason for a capability
// declaration, covering combined declarations (network + gui_open).
func declaredGrantReason(info RunCmdCall) string {
	switch {
	case info.NeedsNetwork && info.NeedsGUIOpen:
		return "command declares it needs outbound network AND GUI application access (macOS open / Apple Events); both granted inside the sandbox on approval"
	case info.NeedsGUIOpen:
		return "command declares it needs to open GUI applications (macOS open / Apple Events, granted inside the sandbox on approval)"
	default:
		return "command declares it needs outbound network (sandboxed, network granted on approval)"
	}
}

func (d BaselineDecider) runCmdBaseline(v *domain.Verdict, info RunCmdCall) *domain.Verdict {
	switch d.Mode {
	case ModeNever:
		switch {
		case info.Escalated:
			v.Decision, v.Reason = domain.DecisionDeny,
				"never mode: escalated (unsandboxed) commands are denied; rework the command to run inside the sandbox"
		case info.NeedsGUIOpen:
			// GUI opening asks in EVERY mode (docs/BROWSER_DESIGN.md §4.2):
			// Apple Events are TCC-attributed to the loom process, so an
			// unattended run must never acquire it. The only unattended
			// path is a user-layer rule carrying grant.gui_open.
			v.Decision, v.Reason = domain.DecisionDeny,
				"never mode: GUI-open (macOS open / Apple Events) is denied unattended; add a user-layer rule with grant.gui_open for this command prefix, or run it without GUI access"
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
		case info.NeedsGUIOpen:
			// Unlike network, GUI-open asks in every mode: Apple Events
			// reach other applications under loom's TCC identity, which a
			// silent grant would hand to any declared call.
			v.Decision, v.Grant = domain.DecisionAsk, DeclaredGrant(info)
			v.Reason = declaredGrantReason(info)
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
		case info.NeedsNetwork || info.NeedsGUIOpen:
			// The ask carries the declared grants: approving this prompt
			// runs the command sandboxed with exactly those capabilities,
			// exactly once.
			v.Decision, v.Grant = domain.DecisionAsk, DeclaredGrant(info)
			v.Reason = declaredGrantReason(info)
		default:
			v.Decision, v.Reason = domain.DecisionAllow, "on-request: sandboxed commands run without prompting"
		}
		return v
	}
}
