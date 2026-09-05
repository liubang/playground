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
// Created: 2026/08/23

// The decision function. The old strategy chain (Rule → Danger →
// UserIntent → Session → Baseline) is replaced by a single procedure
// over the derived effect: deny bindings → forced-ask bindings → user
// intent → indicator gate → capability inclusion → default sandbox →
// mode-specific residual. The chain's layering artifact — one user
// intent treated differently across sessions (PERMISSION_DESIGN §7.0) —
// cannot recur, because there is no separate danger layer to bypass:
// consequence coverage is one uniform dimension of every package.
package permission

import (
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Verdict provenance labels recorded in audits and surfaced in the UI.
const (
	SourceRule       = "rule"
	SourceSession    = "session"
	SourceBaseline   = "baseline"
	SourceUserIntent = "user_intent"
	SourceIndicator  = "indicator"
)

// ApprovalMode selects the residual strategy consulted when no package
// covers the effect (§4.3 of the design doc).
type ApprovalMode string

const (
	// ModeOnRequest auto-allows everything the default sandbox confines
	// and prompts only for what crosses the boundary or carries danger
	// indicators.
	ModeOnRequest ApprovalMode = "on-request"
	// ModeDangerOnly auto-allows anything without an explicitly
	// dangerous signal: boundary crossings (network, extra writes,
	// GUI, even full escalation) are granted as declared, and only
	// deny rules, danger indicators, and destructive / shared-state
	// consequences still prompt. The permissive end of the spectrum,
	// for trusted development workflows.
	ModeDangerOnly ApprovalMode = "danger-only"
	// ModeNever allows sandbox-confined calls (granting network needs)
	// and denies escalations, GUI, extra writes, indicated shapes, and
	// forced-ask calls outright — for unattended runs.
	ModeNever ApprovalMode = "never"
)

// ParseApprovalMode validates a config value; empty selects the default.
func ParseApprovalMode(s string) (ApprovalMode, error) {
	switch ApprovalMode(s) {
	case "", ModeOnRequest:
		return ModeOnRequest, nil
	case ModeDangerOnly:
		return ModeDangerOnly, nil
	case ModeNever:
		return ModeNever, nil
	}
	return "", fmt.Errorf("approval.mode must be %q, %q, or %q, got %q",
		ModeOnRequest, ModeDangerOnly, ModeNever, s)
}

// Decide resolves the verdict for one derivation. It never returns an
// empty decision. workspace is the canonical root of the deciding
// workspace: workspace-scoped packages (project rules, session memory)
// tagged with a different workspace are invisible to this decision.
func (s *PackageSet) Decide(d Derivation, mode ApprovalMode, intentHosts map[string]struct{}, workspace string) domain.Verdict {
	e := d.Effect

	// 1. Deny bindings always win (any layer).
	if pkg, ok := s.matchDecision(d, domain.DecisionDeny, workspace); ok {
		return domain.Verdict{
			Decision: domain.DecisionDeny,
			Source:   SourceRule,
			Reason:   denyReason(pkg),
		}
	}

	// 2. Forced-ask bindings: explicit user policy that this shape must
	// always be confirmed.
	if pkg, ok := s.matchDecision(d, domain.DecisionAsk, workspace); ok {
		return domain.Verdict{
			Decision: domain.DecisionAsk,
			Grant:    e.GapGrant(),
			Source:   SourceRule,
			Reason:   "explicit ask rule: " + pkg.Justification,
		}
	}

	// 3. User intent: fetching a host the user mentioned in the
	// conversation IS the requested work (interactive modes only —
	// the caller passes nil intentHosts in never mode). Indicated
	// shapes (a real-identity browser fetch, anything carrying danger
	// signals) never take this shortcut: a mentioned URL does not
	// bless the browser's real cookies.
	if d.Host != "" && intentHosts != nil && len(e.Indicators) == 0 {
		if _, ok := intentHosts[d.Host]; ok {
			return domain.Verdict{
				Decision: domain.DecisionAllow,
				Grant:    e.GapGrant(),
				Source:   SourceUserIntent,
				Reason:   "the user mentioned this host in the conversation",
			}
		}
	}

	// 4. The indicator gate: an effect carrying danger signals is never
	// covered by categorical packages or the default sandbox — only by
	// an exact-binding approval of the same shape. danger-only first
	// filters out the one benign indicator (driving the real user
	// browser): normal browsing is legitimate work, while every other
	// indicator names a genuinely dangerous shape.
	indicators := e.Indicators
	if mode == ModeDangerOnly {
		indicators = withoutBenignIndicators(indicators)
	}
	if len(indicators) > 0 {
		if pkg, ok := s.exactCover(d, workspace); ok {
			return allowFromPackage(pkg, e)
		}
		if mode == ModeNever {
			return domain.Verdict{
				Decision: domain.DecisionDeny,
				Source:   SourceIndicator,
				Reason: strings.Join(indicators, "; ") +
					" — denied unattended; approve the exact command interactively or rework the approach",
			}
		}
		return domain.Verdict{
			Decision: domain.DecisionAsk,
			Grant:    e.GapGrant(),
			Source:   SourceIndicator,
			Reason:   strings.Join(indicators, "; "),
		}
	}

	// 5. Forced-ask residuals (third-party MCP tools beyond read-only,
	// provider quota spenders): capability packages may still cover
	// them by tool binding.
	if d.ForcedAsk != "" {
		if pkg, ok := s.categoricalCover(d, workspace); ok {
			return allowFromPackage(pkg, e)
		}
		if mode == ModeNever {
			return domain.Verdict{
				Decision: domain.DecisionDeny,
				Source:   SourceBaseline,
				Reason: d.ForcedAsk +
					" — denied unattended; remember the tool with allow always in an interactive session",
			}
		}
		if mode == ModeDangerOnly {
			// Third-party tools and quota spenders carry no danger
			// signal of their own — the mode's contract allows them.
			return domain.Verdict{
				Decision: domain.DecisionAllow,
				Grant:    e.GapGrant(),
				Source:   SourceBaseline,
				Reason:   "danger-only: " + d.ForcedAsk + " — auto-allowed",
			}
		}
		return domain.Verdict{
			Decision: domain.DecisionAsk,
			Grant:    e.GapGrant(),
			Source:   SourceBaseline,
			Reason:   d.ForcedAsk,
		}
	}

	// 6. THE inclusion test: some allow package binds this call and
	// covers the effect.
	if pkg, ok := s.categoricalCover(d, workspace); ok {
		return allowFromPackage(pkg, e)
	}

	// 7. The default sandbox package: confined consequence, no boundary
	// crossing. This is "the sandbox is the boundary" as a package.
	if defaultSandboxPackage.covers(e) {
		return domain.Verdict{
			Decision: domain.DecisionAllow,
			Source:   SourceBaseline,
			Reason:   baselineConfinedReason(e),
		}
	}

	// 8. Mode-specific residual for uncovered effects.
	return residualVerdict(d, mode)
}

// residualVerdict maps an uncovered boundary-crossing effect onto the
// approval mode's contract.
func residualVerdict(d Derivation, mode ApprovalMode) domain.Verdict {
	e := d.Effect
	switch mode {
	case ModeNever:
		// Unattended runs must never acquire destructive or
		// shared-state effects silently: they are denied outright
		// unless an explicit package covered them (the caller already
		// checked). The network grant below is for otherwise-confined
		// commands only.
		if e.Consequence > ConsequenceConfined {
			return domain.Verdict{
				Decision: domain.DecisionDeny, Source: SourceBaseline,
				Reason: "never mode: " + e.Consequence.String() + " operations are denied unattended; approve interactively or add a user-layer package covering this consequence class",
			}
		}
		switch {
		case e.Unsandboxed:
			return domain.Verdict{
				Decision: domain.DecisionDeny, Source: SourceBaseline,
				Reason: "never mode: escalated (unsandboxed) commands are denied; rework the command to run inside the sandbox",
			}
		case e.GUIOpen:
			return domain.Verdict{
				Decision: domain.DecisionDeny, Source: SourceBaseline,
				Reason: "never mode: GUI-open (macOS open / Apple Events) is denied unattended; add a user-layer package with grant.gui_open, or run without GUI access",
			}
		case len(e.Writes.Paths) > 0 || e.Writes.Any:
			return domain.Verdict{
				Decision: domain.DecisionDeny, Source: SourceBaseline,
				Reason: "never mode: writes outside the workspace roots are denied unattended; add a user-layer paths package for the target directory",
			}
		case !e.Network.IsZero():
			return domain.Verdict{
				Decision: domain.DecisionAllow,
				Grant:    domain.ExecGrant{NetworkFull: true},
				Source:   SourceBaseline,
				Reason:   "never mode: network need granted inside the sandbox",
			}
		}
		return domain.Verdict{
			Decision: domain.DecisionAllow, Source: SourceBaseline,
			Reason: "never mode: sandboxed calls run unattended",
		}

	case ModeDangerOnly:
		// Only the operation's real-world blast radius still prompts:
		// destructive or shared-state consequences. Every other
		// boundary crossing (network, extra writes, GUI, escalation)
		// is granted exactly as the effect declares.
		if e.Consequence > ConsequenceConfined {
			return askGapVerdict(e)
		}
		return domain.Verdict{
			Decision: domain.DecisionAllow,
			Grant:    e.GapGrant(),
			Source:   SourceBaseline,
			Reason:   "danger-only: no danger signal; capabilities granted as declared",
		}

	default: // ModeOnRequest
		return askGapVerdict(e)
	}
}

// withoutBenignIndicators drops the indicators that annotate
// legitimate work rather than danger (currently only the real-identity
// browser signal). It never mutates the input slice.
func withoutBenignIndicators(indicators []string) []string {
	var out []string
	for _, ind := range indicators {
		if ind == realIdentityIndicator {
			continue
		}
		out = append(out, ind)
	}
	return out
}

// askGapVerdict is the interactive ask for an uncovered effect: the
// approval carries exactly the gap grant, so approving runs the call
// with the power it declared — never silently under-powered.
func askGapVerdict(e Effect) domain.Verdict {
	return domain.Verdict{
		Decision: domain.DecisionAsk,
		Grant:    e.GapGrant(),
		Source:   SourceBaseline,
		Reason:   e.Describe(),
	}
}

// ExplainMatch returns the package that would decide this derivation,
// mirroring Decide's match order (deny → forced-ask → indicator-exact →
// categorical cover). It backs `loom rules check`'s diagnostics.
func (s *PackageSet) ExplainMatch(d Derivation, workspace string) (Package, bool) {
	if pkg, ok := s.matchDecision(d, domain.DecisionDeny, workspace); ok {
		return pkg, true
	}
	if pkg, ok := s.matchDecision(d, domain.DecisionAsk, workspace); ok {
		return pkg, true
	}
	if len(d.Effect.Indicators) > 0 {
		return s.exactCover(d, workspace)
	}
	return s.categoricalCover(d, workspace)
}

// matchDecision finds the strictest matching package with the given
// decision. Deny/ask bindings match on ANY visible argv (one matching
// step is enough — strictest-wins must bite).
func (s *PackageSet) matchDecision(d Derivation, decision domain.Decision, workspace string) (Package, bool) {
	if s == nil {
		return Package{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, p := range s.packages {
		if p.Decision != decision || !p.visibleTo(workspace) {
			continue
		}
		if packageBinds(p, d, false) {
			return p, true
		}
	}
	return Package{}, false
}

// categoricalCover finds allow coverage for the call, in three shapes:
//
//  1. a host package answering the (enumerated) network requirement
//     while everything else is default-sandbox confined;
//  2. a single package binding the call categorically and covering the
//     joined effect (categorical argv bindings require a fully static
//     plan — one dynamic step must never hide inside an approved
//     prefix);
//  3. per-step coverage of a composed call: every step's argv is bound
//     by some allow package covering THAT step's effect, and the
//     covering grants' union answers the joined effect (the grant a
//     composed call was approved with).
func (s *PackageSet) categoricalCover(d Derivation, workspace string) (Package, bool) {
	if s == nil {
		return Package{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, p := range s.packages {
		if p.Decision != domain.DecisionAllow || !p.visibleTo(workspace) {
			continue
		}
		if p.Bind.Kind == BindHost {
			// A host package's coverage is host matching, not a
			// capability grant: it answers the network requirement
			// of any call shape (web_fetch's host, curl's URL),
			// provided everything else about the effect is
			// default-sandbox confined.
			if hostPackageCovers(p, d) {
				return p, true
			}
			continue
		}
		if !p.covers(d.Effect) {
			continue
		}
		if p.Bind.Kind == BindArgv && !d.StaticPlan {
			// A categorical argv binding must never hide a dynamic
			// step inside an approved prefix.
			continue
		}
		if packageBinds(p, d, true) {
			return p, true
		}
	}
	if len(d.Argvs) > 1 && d.StaticPlan && len(d.StepEffects) == len(d.Plan.Steps) {
		if pkg, ok := s.stepCoverLocked(d, workspace); ok {
			return pkg, true
		}
	}
	return Package{}, false
}

// stepCoverLocked evaluates per-step coverage of a composed exec call.
// The caller holds the read lock.
func (s *PackageSet) stepCoverLocked(d Derivation, workspace string) (Package, bool) {
	var (
		union      PackageGrant
		maxCeiling Consequence
		covered    int
	)
	for i, step := range d.Plan.Steps {
		if len(step.Argv) == 0 {
			return Package{}, false // a dynamic step can never be covered
		}
		stepOK := false
		for _, p := range s.packages {
			if p.Decision != domain.DecisionAllow || p.Bind.Kind != BindArgv || !p.visibleTo(workspace) {
				continue
			}
			if !argvBindsPrefix(step.Argv, p.Bind.Argv) || !p.covers(d.StepEffects[i]) {
				continue
			}
			union = grantUnion(union, p.Grant)
			if p.MaxConsequence > maxCeiling {
				maxCeiling = p.MaxConsequence
			}
			stepOK = true
			break
		}
		if !stepOK {
			return Package{}, false
		}
		covered++
	}
	if covered == 0 {
		return Package{}, false
	}
	merged := Package{
		Bind:           Binding{Kind: BindArgv},
		Decision:       domain.DecisionAllow,
		Grant:          union,
		MaxConsequence: maxCeiling,
		Scope:          ScopeUser,
		Source:         "per-step coverage",
	}
	// The union grant must answer the JOINED effect (declared needs
	// span the whole call): a lesser union never answers a wider
	// request.
	if !merged.covers(d.Effect) {
		return Package{}, false
	}
	return merged, true
}

// exactCover finds an allow package whose binding is EXACT (the full
// argv, or the exact host/path/tool) and which covers the effect — the
// only coverage an indicated effect accepts.
func (s *PackageSet) exactCover(d Derivation, workspace string) (Package, bool) {
	if s == nil {
		return Package{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, p := range s.packages {
		if p.Decision != domain.DecisionAllow || !p.visibleTo(workspace) {
			continue
		}
		if p.Bind.Kind != BindArgvExact {
			continue
		}
		if packageBinds(p, d, false) && p.covers(d.Effect) {
			return p, true
		}
	}
	return Package{}, false
}

// hostPackageCovers reports whether a host-bound allow package answers
// the call's network requirement while everything else about the effect
// is default-sandbox confined. The effect must be FULLY PROVEN: a host
// package (including the builtin registry allowlist) extends that
// host's trust to the whole call, and a call whose other steps are
// unanalyzable (curl <allowed-host> -o x.sh && bash x.sh) must never
// inherit it.
func hostPackageCovers(p Package, d Derivation) bool {
	e := d.Effect
	if !e.Proven {
		return false
	}
	if e.Unsandboxed || e.GUIOpen || !e.Writes.IsZero() {
		return false
	}
	if e.Consequence > p.MaxConsequence {
		return false
	}
	switch p.Bind.Kind {
	case BindHost:
		if d.Host != "" {
			return hostMatchesPattern(p.Bind.Host, d.Host)
		}
		// Exec effect with enumerated hosts: the package must cover
		// every one of them.
		if len(e.Network.Hosts) == 0 {
			return false
		}
		for _, h := range e.Network.Hosts {
			if !hostMatchesPattern(p.Bind.Host, h) {
				return false
			}
		}
		return true
	}
	return false
}

// allowFromPackage renders the allow verdict a covering package produces.
// A host-bound package's grant is implicit — "egress to that host" — so
// the execution grant is the effect's gap grant (the sandbox's network
// switch is all-or-nothing); every other package executes with the grant
// it carries.
func allowFromPackage(pkg Package, e Effect) domain.Verdict {
	grant := pkg.Grant.ExecGrant()
	if pkg.Bind.Kind == BindHost {
		grant = e.GapGrant()
	}
	source := SourceRule
	if pkg.Scope == ScopeSession {
		source = SourceSession
	}
	reason := "covered by " + pkg.Scope.String() + " package"
	if pkg.Justification != "" {
		reason += ": " + pkg.Justification
	}
	return domain.Verdict{
		Decision: domain.DecisionAllow,
		Grant:    grant,
		Source:   source,
		Reason:   reason,
	}
}

// denyReason renders a deny package's provenance.
func denyReason(pkg Package) string {
	if pkg.Justification != "" {
		return pkg.Justification
	}
	return "denied by " + pkg.Scope.String() + " package"
}

// baselineConfinedReason renders the default-sandbox allow provenance,
// disclosing unprovability when the effect could not be fully derived.
func baselineConfinedReason(e Effect) string {
	if !e.Proven {
		return "behavior not statically provable (" + e.Reason +
			") — sandbox-confined, the sandbox is the boundary"
	}
	return "confined to the default sandbox — the sandbox is the boundary"
}
