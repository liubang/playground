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
// Created: 2026/08/17

// Memory derivation for interactive "allow always" approvals. The shape
// of the memory depends on the call's DERIVATION, not the tool name:
//
//   - exec calls with danger indicators → an EXACT argv package (a
//     categorical prefix must never silently bless a flagged shape);
//   - exec calls without indicators → categorical prefix packages, one
//     per plan step, carrying the effect's consequence ceiling;
//   - URL calls → an exact-host package;
//   - boundary-crossing writes → a writable-directory package;
//   - fixed-blast-radius tools → a tool-name package.
//
// Every remembered package carries the effect's consequence ceiling, so
// approving `git push` (shared-state) never silently covers
// `git push --force` (shared-destructive) — in-session or persisted.
package permission

import (
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// TrustUnsandboxed is the trust-flavor value for L2 full-trust memory
// (explicit user opt-in for escalated calls). The app layer's
// ApprovalRuleHint.Trust uses the same string.
const TrustUnsandboxed = "unsandboxed"

// MemoryPackages computes the capability packages an "allow always"
// approval of this call should record. ok=false means the call must
// stay per-call (dynamic shells, eval forms, multi-step indicated
// shapes, ineligible tools).
//
//   - trust=unsandboxed on an escalated call → L2 full trust (the ONLY
//     rememberable flavor that answers an escalation);
//   - otherwise → exactly the effect's gap grant (an escalated call's
//     minimal flavor approximates to the network grant: the most common
//     escalation cause, and the package then covers the model's
//     sandboxed needs_network retry).
func MemoryPackages(d Derivation, trust string) ([]Package, bool) {
	e := d.Effect
	grant := packageGrantFromEffect(e, trust)
	switch {
	case len(d.Plan.Steps) > 0:
		return execMemoryPackages(d, grant)
	case d.Host != "":
		if len(e.Indicators) > 0 {
			// An indicated URL shape (the real-identity browser fetch)
			// stays per-call: there is no honest standing approval for
			// speaking with the user's cookies.
			return nil, false
		}
		return []Package{{
			Bind:           Binding{Kind: BindHost, Host: d.Host},
			Decision:       domain.DecisionAllow,
			MaxConsequence: e.Consequence,
		}}, true
	case d.WritePath != "":
		dir := filepath.Dir(d.WritePath)
		if workspacepkg.CoversSensitiveLocation(dir) {
			// A directory that COVERS a sensitive location must never
			// become a standing write grant: remembering ~/notes must
			// not silently open ~/.zshrc.
			return nil, false
		}
		return []Package{{
			Bind:           Binding{Kind: BindPath, Path: dir},
			Decision:       domain.DecisionAllow,
			MaxConsequence: e.Consequence,
		}}, true
	default:
		canonical, eligible := ToolMemoryEligible(d.ToolName)
		if !eligible {
			return nil, false
		}
		return []Package{{
			Bind:           Binding{Kind: BindTool, Tool: canonical},
			Decision:       domain.DecisionAllow,
			MaxConsequence: e.Consequence,
		}}, true
	}
}

// execMemoryPackages derives argv bindings for an exec call.
func execMemoryPackages(d Derivation, grant PackageGrant) ([]Package, bool) {
	e := d.Effect
	if len(e.Indicators) > 0 {
		// An indicated shape is remembered EXACTLY, and only when it is
		// a single static command whose argv IS the whole invocation —
		// a step fed by a pipe/heredoc, or any redirect, makes the argv
		// an incomplete description of the invocation (a later different
		// body would spuriously match), so those stay per-call.
		if len(d.Argvs) != 1 || !d.StaticPlan || len(d.Plan.Steps) != 1 {
			return nil, false
		}
		step := d.Plan.Steps[0]
		if step.Stdin != process.StdinNone || len(d.Plan.WriteRedirects) > 0 {
			return nil, false
		}
		return []Package{{
			Bind:           Binding{Kind: BindArgvExact, Argv: append([]string(nil), d.Argvs[0]...)},
			Decision:       domain.DecisionAllow,
			Grant:          grant,
			MaxConsequence: e.Consequence,
		}}, true
	}
	prefixes, ok := DeriveMemoryPrefixes(d)
	if !ok {
		return nil, false
	}
	out := make([]Package, 0, len(prefixes))
	for _, prefix := range prefixes {
		out = append(out, Package{
			Bind:           Binding{Kind: BindArgv, Argv: prefix},
			Decision:       domain.DecisionAllow,
			Grant:          grant,
			MaxConsequence: e.Consequence,
		})
	}
	return out, true
}

// packageGrantFromEffect computes the grant a remembered package should
// carry for the given trust flavor.
func packageGrantFromEffect(e Effect, trust string) PackageGrant {
	if trust == TrustUnsandboxed && e.Unsandboxed {
		return PackageGrant{Unsandboxed: true}
	}
	if e.Unsandboxed {
		// The minimal flavor of an escalated call approximates to the
		// network grant (the most common escalation cause).
		return PackageGrant{
			NetworkFull:   true,
			WritablePaths: append([]string(nil), e.Writes.Paths...),
			GUIOpen:       e.GUIOpen,
		}
	}
	return PackageGrant{
		NetworkFull:   !e.Network.IsZero(),
		NetworkHosts:  append([]string(nil), e.Network.Hosts...),
		WritablePaths: append([]string(nil), e.Writes.Paths...),
		GUIOpen:       e.GUIOpen,
	}
}

// MemoryPreviewLabel renders the display form of the memory for the
// approval overlay's "allow always" option. ok=false means the call
// cannot be remembered.
func MemoryPreviewLabel(d Derivation, trust string) (label string, pkgs []Package, ok bool) {
	pkgs, ok = MemoryPackages(d, trust)
	if !ok {
		return "", nil, false
	}
	var parts []string
	for _, p := range pkgs {
		parts = append(parts, bindingLabel(p.Bind))
	}
	label = strings.Join(parts, " && ")
	if grant := pkgs[0].Grant; !grant.IsZero() {
		label += " (" + grantLabel(grant) + ")"
	}
	return label, pkgs, true
}

// bindingLabel renders a binding for display.
func bindingLabel(b Binding) string {
	switch b.Kind {
	case BindArgv:
		return strings.Join(b.Argv, " ")
	case BindArgvExact:
		return "exact: " + strings.Join(b.Argv, " ")
	case BindHost:
		return b.Host
	case BindPath:
		return b.Path
	case BindTool:
		return b.Tool
	}
	return ""
}

// grantLabel renders a package grant for display ("+网络, +写 ~/.mycli").
func grantLabel(g PackageGrant) string {
	if g.Unsandboxed {
		return "出沙箱（完整权限）"
	}
	var parts []string
	if g.NetworkFull {
		parts = append(parts, "+网络")
	}
	for _, h := range g.NetworkHosts {
		parts = append(parts, "+网络 "+h)
	}
	if g.GUIOpen {
		parts = append(parts, "+GUI 打开")
	}
	for _, p := range g.WritablePaths {
		parts = append(parts, "+写 "+p)
	}
	return strings.Join(parts, ", ")
}
