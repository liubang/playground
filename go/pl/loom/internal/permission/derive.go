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

// The derivation pipeline: PreparedCall → Derivation. It is the ONLY
// place that turns a call into an Effect, and it is a pure function of
// the signed Prepare contract (ExecRequest / URLRequest / WriteRequest),
// so the derived effect inherits the signature's integrity — the model
// cannot shape its own classification. Evidence sources in reliability
// order: tool self-declaration (the typed contracts) → semantic command
// tables → shell normalization → heuristic indicators → the explicit
// unprovable state.
package permission

import (
	"encoding/json"
	"net/url"
	"path/filepath"
	"slices"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// DeriveEnv carries the environment facts derivation needs: the
// canonical roots the default sandbox already confines (workspace roots
// plus scratch/toolchain dirs), so absolute write targets can be
// classified confined vs boundary-crossing. Both layers draw from the
// same source (workspace.Canonicalize), so run_cmd and the file tools
// never disagree.
type DeriveEnv struct {
	Roots []string
}

// Derivation is the full policy-relevant account of one prepared call:
// the derived effect plus the normalized shapes binding and memory
// matching consume.
type Derivation struct {
	Call   domain.PreparedCall
	Effect Effect
	// Plan is the normalized execution shape (exec calls only).
	Plan ExecPlan
	// Argvs are the statically-resolved command argvs (binding input).
	Argvs [][]string
	// StepEffects are the per-step effects of an exec call (same length
	// and order as Plan.Steps; empty for non-exec shapes). The joined
	// Effect is their union; per-step coverage checks consult these.
	StepEffects []Effect
	// StaticPlan reports that every execution step resolved statically
	// with no dynamic writes: categorical allow bindings may only match
	// when this holds (one dynamic step must never hide inside an
	// approved prefix).
	StaticPlan bool
	// Host is the URL contract's canonical host (web_fetch, browser).
	Host string
	// WritePath is the write contract's canonical target (write, edit).
	WritePath string
	// ToolName is the call's tool name (tool bindings, MCP handling).
	ToolName string
	// Source is the tool's origin (builtin / MCP / subagent).
	Source domain.ToolSource
	// Risk is the Prepare-assigned risk tier (MCP read-only handling,
	// the no-contract R3+ residual).
	Risk domain.RiskLevel
	// ForcedAsk is set for calls whose cost cannot be modeled as
	// capabilities (third-party MCP tools beyond read-only, provider
	// quota spenders): they always ask in interactive modes.
	ForcedAsk string
}

// DeriveEffect is the single entry point: prepared call → derivation.
// The typed Prepare contract is authoritative; a call constructed
// outside Prepare (tests, the approval-UI boundary) has its raw
// arguments parsed as the fallback — the same single entry, so no
// consumer ever picks the wrong derivation path.
func DeriveEffect(call domain.PreparedCall, env DeriveEnv) Derivation {
	if call.ExecRequest == nil && call.URLRequest == nil && call.WriteRequest == nil &&
		call.Definition.Source != domain.ToolSourceMCP {
		fillRawContracts(&call)
	}
	d := Derivation{
		Call:     call,
		ToolName: call.Call.Name,
		Source:   call.Definition.Source,
		Risk:     call.Risk,
	}
	switch {
	case call.ExecRequest != nil:
		d.deriveExec(call.ExecRequest, env)
	case call.URLRequest != nil:
		d.deriveURL(call.URLRequest)
	case call.WriteRequest != nil:
		d.deriveWrite(call.WriteRequest, env)
	case call.Definition.Source == domain.ToolSourceMCP:
		d.deriveMCP(call)
	default:
		d.derivePlainBuiltin(call)
	}
	return d
}

// fillRawContracts synthesizes the typed contracts from raw arguments
// for calls constructed outside Prepare. The exec shape is recognized
// structurally (a program field), the URL shape by a url field, the
// write shape by an absolute path field — never by tool name, except
// for the real-identity marker, which only the producing tool can
// honestly declare ("browser" drives the user's real browser).
func fillRawContracts(call *domain.PreparedCall) {
	raw := call.Call.Arguments
	if len(raw) == 0 {
		return
	}
	if argv, escalated, needsNetwork, needsGUI, writable, ok := ParseRunCmdCall(raw); ok {
		call.ExecRequest = &domain.ExecRequest{
			Argv:          argv,
			Escalated:     escalated,
			NeedsNetwork:  needsNetwork,
			NeedsGUIOpen:  needsGUI,
			WritablePaths: writable,
		}
		return
	}
	var urlArgs struct {
		URL string `json:"url"`
	}
	if err := json.Unmarshal(raw, &urlArgs); err == nil && urlArgs.URL != "" {
		if u, err := url.Parse(urlArgs.URL); err == nil && u.Hostname() != "" &&
			(u.Scheme == "http" || u.Scheme == "https") {
			call.URLRequest = &domain.URLRequest{
				Host:         strings.ToLower(u.Hostname()),
				RealIdentity: call.Call.Name == "browser",
			}
			return
		}
	}
	var pathArgs struct {
		Path string `json:"path"`
	}
	if err := json.Unmarshal(raw, &pathArgs); err == nil && filepath.IsAbs(pathArgs.Path) {
		call.WriteRequest = &domain.WriteRequest{Path: pathArgs.Path, OutsideRoots: true}
	}
}

// deriveExec lowers an exec-shape call: model-declared needs are merged
// as a FLOOR (they are requests, not evidence), the plan's steps are
// semantically derived, and plan-level indicators are attached.
func (d *Derivation) deriveExec(req *domain.ExecRequest, env DeriveEnv) {
	argv := append([]string(nil), req.Argv...)
	d.Plan = NormalizeExec(argv)
	d.Argvs = d.Plan.Argvs()
	d.StaticPlan = d.Plan.Unanalyzable == "" && !d.Plan.DynamicWrites &&
		len(d.Argvs) == len(d.Plan.Steps)

	steps := make([]Effect, 0, len(d.Plan.Steps)+1)
	for _, step := range d.Plan.Steps {
		steps = append(steps, deriveStep(step))
	}
	d.StepEffects = steps
	e := joinEffects(steps)
	if d.Plan.Unanalyzable != "" {
		e.Proven = false
		if e.Reason == "" {
			e.Reason = d.Plan.Unanalyzable
		}
	}

	// Write redirects are effects of the plan itself.
	redirectEffect := d.redirectEffect(env)
	e = joinEffects([]Effect{e, redirectEffect})

	// Cross-command indicators (pipe into interpreter, etc.).
	e.Indicators = unionStrings(e.Indicators, planIndicators(d.Plan))

	// The model's declared needs are a floor, never a ceiling.
	if req.Escalated {
		e.Unsandboxed = true
	}
	if req.NeedsNetwork && e.Network.IsZero() {
		e.Network = HostSet{Any: true}
	} else if req.NeedsNetwork && !e.Network.Any {
		// A declared network need the semantics could not enumerate
		// widens the requirement to Any.
		e.Network = HostSet{Any: true}
	}
	if req.NeedsGUIOpen {
		e.GUIOpen = true
	}
	for _, w := range req.WritablePaths {
		e.Writes.Paths = unionStrings(e.Writes.Paths, []string{w})
	}
	if e.Reason == "" && e.Proven {
		e.Reason = programBase(argv[0])
	}
	d.Effect = e
}

// redirectEffect classifies the plan's file-writing redirects: targets
// under the sandbox roots are confined; absolute targets outside them
// are boundary-crossing writes; sensitive targets (shell startup files,
// credential locations, git metadata, loom state) additionally carry a
// persistence indicator.
func (d *Derivation) redirectEffect(env DeriveEnv) Effect {
	var e Effect
	e.Proven = true
	for _, target := range d.Plan.WriteRedirects {
		// ~-prefixed targets are home-relative by construction — never
		// workspace-confined, even when the home directory cannot be
		// resolved (test sandboxes may scrub HOME).
		homeRelative := target == "~" || strings.HasPrefix(target, "~/")
		expanded := expandTilde(target)
		if !filepath.IsAbs(expanded) && !homeRelative {
			// Relative targets write into the sandboxed cwd — but a
			// relative target can still be a persistence vector
			// (.git/hooks, .loom inside the workspace), so the
			// sensitive check runs against the workspace-joined path.
			if len(env.Roots) > 0 {
				joined := filepath.Join(env.Roots[0], filepath.Clean(target))
				if reason := sensitiveRedirectTarget(joined); reason != "" {
					e.Indicators = append(e.Indicators, reason)
				}
			}
			continue
		}
		// Canonicalize, not just Clean: the roots are canonical (macOS
		// /tmp and /var are symlinks into /private), so a literal /tmp
		// target must resolve before the prefix check or every scratch
		// write looks boundary-crossing.
		clean := workspacepkg.Canonicalize(expanded)
		// The sensitive check runs BEFORE the confined check: a redirect
		// into the workspace's own .git/hooks is sandbox-blocked, but it
		// is a persistence ATTEMPT and must be surfaced, not just
		// confined.
		if reason := sensitiveRedirectTarget(clean); reason != "" {
			e.Indicators = append(e.Indicators, reason)
		}
		if slices.Contains(process.SandboxWritableLiterals, clean) {
			continue // device sinks the sandbox always allows (2>/dev/null)
		}
		if pathUnderRoots(env.Roots, clean) {
			continue
		}
		e.Writes.Paths = unionStrings(e.Writes.Paths, []string{clean})
	}
	if d.Plan.DynamicWrites {
		e.Writes.Any = true
		e.Proven = false
		e.Reason = "a write redirect has a dynamic target"
	}
	return e
}

// deriveURL lowers a URL-shape call: enumerated egress to the contract's
// host. A real-identity fetch (browser) carries a standing indicator —
// it speaks with the user's cookies, so it keeps per-call approval and
// may only be remembered exactly.
func (d *Derivation) deriveURL(req *domain.URLRequest) {
	d.Host = req.Host
	e := Effect{
		Proven:      true,
		Consequence: ConsequenceConfined,
		Reason:      "fetch " + req.Host,
	}
	// Loopback egress is permitted by the default sandbox — it is not a
	// capability requirement. The host still lands in d.Host for deny
	// bindings and user-intent matching.
	if !isLoopbackHost(req.Host) {
		e.Network = HostSet{Hosts: []string{req.Host}}
	}
	if req.RealIdentity {
		e.Indicators = append(e.Indicators, realIdentityIndicator)
	}
	d.Effect = e
}

// deriveWrite lowers a write-shape call: workspace-confined writes are
// the zero effect; outside the roots the target is the requirement.
// The target is canonicalized so binding and memory speak the same
// path form as the sandbox layers (workspace.Canonicalize).
func (d *Derivation) deriveWrite(req *domain.WriteRequest, env DeriveEnv) {
	clean := workspacepkg.Canonicalize(req.Path)
	d.WritePath = clean
	if pathUnderRoots(env.Roots, clean) {
		d.Effect = ZeroEffect
		return
	}
	e := Effect{
		Proven: true,
		Writes: PathSet{Paths: []string{clean}},
		Reason: "writes outside the workspace roots: " + clean,
	}
	// A direct write to a sensitive target carries the same persistence
	// indicator as a shell redirect into one: the attempt is surfaced,
	// never silently coverable by a categorical package.
	if reason := sensitiveRedirectTarget(clean); reason != "" {
		e.Indicators = append(e.Indicators, reason)
	}
	d.Effect = e
}

// deriveMCP lowers a third-party MCP call: its behavior is not
// auditable, so it is unprovable by construction. Read-only-hinted tools
// (Risk ≤ R1) still auto-allow — that residual lives in the decision
// layer via d.Risk.
func (d *Derivation) deriveMCP(call domain.PreparedCall) {
	d.Effect = Effect{
		Proven: false,
		Reason: "third-party MCP tool " + call.Call.Name + " — behavior not auditable",
	}
	if call.Risk > domain.R1 {
		d.ForcedAsk = "third-party (MCP) tools require approval"
	}
}

// derivePlainBuiltin lowers a builtin tool with no typed contract. R0–R2
// tools are workspace-confined by the path validator (the zero effect);
// R3+ tools whose cost is not a capability (provider quota spenders like
// generate_image) keep a forced ask.
func (d *Derivation) derivePlainBuiltin(call domain.PreparedCall) {
	if call.Risk <= domain.R2 {
		d.Effect = ZeroEffect
		return
	}
	d.Effect = ZeroEffect
	d.ForcedAsk = "R3+ builtin operation requires approval"
}

// derivePlan computes a nested plan's effect (recursive sh -c analysis).
func derivePlan(plan ExecPlan, depth int) Effect {
	if depth > maxDeriveDepth {
		return Effect{Proven: false, Reason: "command nesting too deep to analyze"}
	}
	steps := make([]Effect, 0, len(plan.Steps))
	for _, step := range plan.Steps {
		steps = append(steps, deriveStepRec(step, depth))
	}
	e := joinEffects(steps)
	if plan.Unanalyzable != "" {
		e.Proven = false
		if e.Reason == "" {
			e.Reason = plan.Unanalyzable
		}
	}
	e.Indicators = unionStrings(e.Indicators, planIndicators(plan))
	return e
}

// pathUnderRoots reports whether path sits under one of the canonical
// roots (workspace + scratch + toolchain caches).
func pathUnderRoots(roots []string, path string) bool {
	clean := filepath.Clean(path)
	for _, root := range roots {
		r := filepath.Clean(root)
		if clean == r || len(clean) > len(r) && clean[len(r)] == filepath.Separator &&
			clean[:len(r)] == r {
			return true
		}
	}
	return false
}

// expandTilde resolves a leading ~ against the user's home directory.
func expandTilde(p string) string {
	if p == "~" || len(p) > 1 && p[:2] == "~/" {
		if home, err := homeDir(); err == nil {
			return filepath.Join(home, p[1:])
		}
	}
	return p
}

// DeriveRawArgs derives the call shape from raw arguments — the
// approval-UI boundary, where only the event payload survives. It is
// DeriveEffect with a minimal call wrapper: the raw fallback lives in
// the single entry point, so both paths classify identically. The tool
// source must be carried through from the producing tool: without it an
// MCP call would be re-derived from its argument shape (a "program"
// field becomes an exec effect, a "url" field a host binding) instead
// of keeping its third-party identity.
func DeriveRawArgs(toolName string, source domain.ToolSource, raw json.RawMessage, env DeriveEnv) Derivation {
	return DeriveEffect(domain.PreparedCall{
		Call:       domain.ToolCall{Name: toolName, Arguments: raw},
		Definition: domain.ToolDefinition{Name: toolName, Source: source},
	}, env)
}

// ParseRunCmdCall extracts the exec shape from raw run_cmd arguments —
// the fallback for calls constructed outside Prepare (tests, the
// approval UI boundary where only the event payload's raw arguments
// survive). The typed ExecRequest is authoritative when present.
func ParseRunCmdCall(raw json.RawMessage) (argv []string, escalated, needsNetwork, needsGUI bool, writable []string, ok bool) {
	var args struct {
		Command            string   `json:"command"`
		SandboxPermissions string   `json:"sandbox_permissions"`
		NeedsNetwork       bool     `json:"needs_network"`
		NeedsGUIOpen       bool     `json:"needs_gui_open"`
		WritablePaths      []string `json:"writable_paths"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.Command == "" {
		return nil, false, false, false, nil, false
	}
	return []string{"sh", "-c", args.Command},
		args.SandboxPermissions == "require_escalated",
		args.NeedsNetwork, args.NeedsGUIOpen,
		args.WritablePaths, true
}
