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

// Capability packages are the single trust object of the permission
// model. Everything that can make a call run without asking — builtin
// read-only sets, user rules, project rules, rule packs, "allow always"
// memory (session or persisted), and even the default sandbox itself —
// is a Package, and the ONLY policy question is: does some allow package
// both BIND this call and COVER its derived Effect (capability.go's
// inclusion test)? The old layering artifact where a remembered approval
// was screened by danger heuristics in-session but bypassed them once
// persisted (PERMISSION_DESIGN §7.0) disappears: consequence coverage is
// uniform across every package source.
package permission

import (
	"path/filepath"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// BindKind identifies which call shape a package binds.
type BindKind int

const (
	// BindArgv is a categorical argv prefix (["go", "test"]): it binds
	// the family of commands sharing the prefix.
	BindArgv BindKind = iota
	// BindArgvExact is a full exact argv. Indicated effects (danger
	// signals attached) may ONLY be covered by exact bindings: a
	// categorical approval must never silently bless the shape the
	// heuristics flagged.
	BindArgvExact
	// BindHost binds a canonical host ("example.com" or "*.example.com").
	BindHost
	// BindPath binds a writable directory (covers writes under it).
	BindPath
	// BindTool binds a tool name (MCP tools, fixed-blast-radius builtins).
	BindTool
)

// Binding is the call-shape a package applies to. Exactly one field is
// meaningful per Kind.
type Binding struct {
	Kind BindKind
	Argv []string // BindArgv: prefix; BindArgvExact: full argv
	Host string   // BindHost: canonical host, optional "*." suffix wildcard
	Path string   // BindPath: canonical absolute directory
	Tool string   // BindTool: canonical tool name
}

// Scope is the trust layer a package lives in. Wider scopes are more
// trusted; the loading rules per scope (project may only tighten,
// builtin never carries grants) are enforced at load time.
type Scope int

const (
	ScopeBuiltin Scope = iota // embedded read-only set
	ScopeProject              // <workspace>/.loom/rules — untrusted, tighten-only
	ScopeUser                 // <loom home>/rules — the user's own policy
	ScopeSession              // in-memory "allow always" of the running session
)

// String renders the scope for `loom rules list` and audits.
func (s Scope) String() string {
	switch s {
	case ScopeBuiltin:
		return "builtin"
	case ScopeProject:
		return "project"
	case ScopeUser:
		return "user"
	case ScopeSession:
		return "session"
	}
	return "unknown"
}

// PackageGrant is the capability content of an allow package (schema v3).
// It mirrors domain.ExecGrant and adds host-granular network: a package
// may open egress to specific hosts only — the policy-layer shape of
// domain-level network authorization.
type PackageGrant struct {
	Unsandboxed   bool     `json:"unsandboxed,omitempty"`
	NetworkFull   bool     `json:"network_full,omitempty"`
	NetworkHosts  []string `json:"network_hosts,omitempty"`
	WritablePaths []string `json:"write,omitempty"`
	GUIOpen       bool     `json:"gui_open,omitempty"`
}

// ExecGrant converts to the execution contract. The sandbox's network
// switch is all-or-nothing, so ANY network allowance (full or
// host-granular) maps to NetworkFull at execution; host granularity is
// enforced by the policy layer before the verdict is issued.
func (g PackageGrant) ExecGrant() domain.ExecGrant {
	return domain.ExecGrant{
		Unsandboxed:   g.Unsandboxed,
		NetworkFull:   g.NetworkFull || len(g.NetworkHosts) > 0,
		WritablePaths: append([]string(nil), g.WritablePaths...),
		GUIOpen:       g.GUIOpen,
	}
}

// IsZero reports whether the grant opens nothing (default sandbox).
func (g PackageGrant) IsZero() bool {
	return !g.Unsandboxed && !g.NetworkFull && len(g.NetworkHosts) == 0 &&
		len(g.WritablePaths) == 0 && !g.GUIOpen
}

// Package is one entry of the capability set: a binding, a decision, and
// (for allow) the granted capabilities plus the consequence ceiling the
// user accepted.
type Package struct {
	Bind     Binding
	Decision domain.Decision `json:"decision"`
	Grant    PackageGrant    `json:"grant,omitempty"`
	// MaxConsequence is the highest consequence class this package
	// covers. An allow package for `git push` carries shared-state; the
	// destructive form (push --force → shared-destructive) is NOT
	// covered by it and keeps asking until the user explicitly accepts
	// that class. This is the uniform mechanism that replaced the
	// danger screen's position in the chain.
	MaxConsequence Consequence `json:"-"`
	// Implicit marks the default-sandbox package: it is not user trust
	// and must never cover effects carrying danger indicators.
	Implicit bool `json:"-"`
	// Scope records the trust layer (loading rules depend on it).
	Scope Scope `json:"-"`
	// Justification is the human-readable rationale (approval prompts,
	// `loom rules list`).
	Justification string `json:"justification,omitempty"`
	// Source is the file/store the package came from (diagnostics).
	Source string `json:"-"`
}

// defaultSandboxPackage is the implicit L0 grant: confined consequences,
// zero capability widenings. Everything the sandbox or the path
// validator confines is covered by it — that is the "sandbox is the
// boundary" baseline, expressed as just another package.
var defaultSandboxPackage = Package{
	Bind:           Binding{Kind: BindTool}, // unused: matched specially
	Decision:       domain.DecisionAllow,
	MaxConsequence: ConsequenceConfined,
	Implicit:       true,
	Source:         "default-sandbox",
}

// covers is THE inclusion test: does this package's grant + consequence
// ceiling answer every requirement of the effect? Bind matching is the
// caller's job (it needs the call's argvs); covers reasons purely about
// capabilities. An indicated effect is never covered by an implicit
// package. Provability is enforced per binding kind by the CALLERS:
// host packages demand a fully proven effect (hostPackageCovers),
// exact argv bindings demand a fully static single-step plan, and
// categorical argv bindings demand a fully static plan — an unprovable
// effect with a static argv (an unrecognized program) may still be
// covered by an argv package, because the sandbox bounds what the
// unknown program can do with the granted capabilities.
func (p Package) covers(e Effect) bool {
	if p.Decision != domain.DecisionAllow {
		return false
	}
	if p.Implicit && len(e.Indicators) > 0 {
		return false
	}
	if e.Consequence > p.MaxConsequence {
		return false
	}
	g := p.Grant
	if g.Unsandboxed {
		// L2 covers every capability requirement; the consequence
		// ceiling above still applies.
		return true
	}
	if e.Unsandboxed {
		return false
	}
	if e.GUIOpen && !g.GUIOpen {
		return false
	}
	switch {
	case e.Network.Any:
		if !g.NetworkFull {
			return false
		}
	default:
		for _, h := range e.Network.Hosts {
			if !g.NetworkFull && !hostCoveredBy(g.NetworkHosts, h) {
				return false
			}
		}
	}
	if e.Writes.Any {
		return false
	}
	writable := g.WritablePaths
	if p.Bind.Kind == BindPath {
		// A path-bound package's grant IS the bound directory:
		// "always allow writing under X" needs no separate grant.
		writable = append(append([]string(nil), g.WritablePaths...), p.Bind.Path)
	}
	for _, w := range e.Writes.Paths {
		if !writablePathCovered(writable, w) {
			return false
		}
	}
	return true
}

// hostCoveredBy reports whether host h matches any of the package's host
// patterns (exact or "*.suffix" wildcard, mirroring domain rule matching).
func hostCoveredBy(patterns []string, host string) bool {
	for _, p := range patterns {
		if hostMatchesPattern(p, host) {
			return true
		}
	}
	return false
}

// hostMatchesPattern implements the domain-rule matching contract: exact
// host (case-insensitive, no port), plus the "*." suffix wildcard which
// matches any subdomain but NOT the apex itself.
func hostMatchesPattern(pattern, host string) bool {
	pattern, host = strings.ToLower(pattern), strings.ToLower(host)
	if strings.HasPrefix(pattern, "*.") {
		suffix := pattern[1:] // ".example.com"
		return strings.HasSuffix(host, suffix) && len(host) > len(suffix)
	}
	return pattern == host
}

// grantUnion merges two package grants (per-step coverage of a composed
// call needs every capability its steps were approved with).
func grantUnion(a, b PackageGrant) PackageGrant {
	return PackageGrant{
		Unsandboxed:   a.Unsandboxed || b.Unsandboxed,
		NetworkFull:   a.NetworkFull || b.NetworkFull,
		NetworkHosts:  unionStrings(a.NetworkHosts, b.NetworkHosts),
		WritablePaths: unionStrings(a.WritablePaths, b.WritablePaths),
		GUIOpen:       a.GUIOpen || b.GUIOpen,
	}
}

// writablePathCovered reports whether the requested path equals or sits
// under one of the granted paths. Both sides are canonical absolute
// paths, so a clean+prefix check is sound.
func writablePathCovered(granted []string, requested string) bool {
	clean := filepath.Clean(requested)
	for _, g := range granted {
		root := filepath.Clean(g)
		if clean == root || strings.HasPrefix(clean, root+string(filepath.Separator)) {
			return true
		}
	}
	return false
}

// PackageSet is the evaluated capability set: every package from every
// layer, plus the running session's in-memory approvals. It is safe for
// concurrent use; session remembers mutate under the lock.
type PackageSet struct {
	mu       sync.RWMutex
	packages []Package
}

// NewPackageSet creates an empty set.
func NewPackageSet() *PackageSet { return &PackageSet{} }

// Add appends packages (load-time assembly).
func (s *PackageSet) Add(pkgs ...Package) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.packages = append(s.packages, pkgs...)
}

// Packages returns a copy of all packages (display, tests).
func (s *PackageSet) Packages() []Package {
	if s == nil {
		return nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return append([]Package(nil), s.packages...)
}

// HasAny reports whether the set holds at least one package.
func (s *PackageSet) HasAny() bool {
	if s == nil {
		return false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.packages) > 0
}

// matchCall collects every package whose binding applies to the call
// shape described by d (the derivation carries the normalized argvs,
// host, write path, and tool name). Binding semantics per kind:
//
//   - BindArgv: a single-argv call matches on prefix; a composed shell
//     call matches only when EVERY step argv carries the prefix (an
//     allow for `git push` must not bless `git push && curl evil`).
//     Deny/ask decisions match when ANY step carries the prefix —
//     strictest-wins means a deny anywhere must bite.
//   - BindArgvExact: the call is a single-argv invocation equal to the
//     bound argv token-for-token.
//   - BindHost / BindPath / BindTool: match the call's host / write
//     target's directory / tool name.
func (s *PackageSet) matchCall(d Derivation, allowRequiresAllSteps bool) []Package {
	if s == nil {
		return nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	var out []Package
	for _, p := range s.packages {
		if packageBinds(p, d, allowRequiresAllSteps) {
			out = append(out, p)
		}
	}
	return out
}

// packageBinds reports whether p's binding applies to the call.
func packageBinds(p Package, d Derivation, allowRequiresAllSteps bool) bool {
	switch p.Bind.Kind {
	case BindArgv:
		if len(d.Argvs) == 0 || len(p.Bind.Argv) == 0 {
			return false
		}
		if p.Decision == domain.DecisionAllow && allowRequiresAllSteps {
			for _, argv := range d.Argvs {
				if !argvBindsPrefix(argv, p.Bind.Argv) {
					return false
				}
			}
			return true
		}
		for _, argv := range d.Argvs {
			if argvBindsPrefix(argv, p.Bind.Argv) {
				return true
			}
		}
		return false
	case BindArgvExact:
		// An exact binding covers the invocation ONLY when the whole
		// invocation is exactly this argv: a fully static, single-step
		// plan. The creation side (memory.go) enforces the same
		// condition; without it a dynamic step could hide inside the
		// plan while the static subset matched exactly.
		if !d.StaticPlan || len(d.Plan.Steps) != 1 || len(d.Argvs) != 1 || len(p.Bind.Argv) == 0 {
			return false
		}
		if stringSliceEqual(d.Argvs[0], p.Bind.Argv) {
			return true
		}
		if normalized, ok := normalizeTrustedArgv(d.Argvs[0]); ok {
			return stringSliceEqual(normalized, p.Bind.Argv)
		}
		return false
	case BindHost:
		if d.Host != "" && hostMatchesPattern(p.Bind.Host, d.Host) {
			return true
		}
		// An exec effect with enumerated hosts (curl's URL, wget's
		// targets) is bound by host policy too: a deny for an
		// exfiltration channel must bite regardless of which tool
		// speaks to it — even through a proxy (NamedHosts).
		for _, h := range d.Effect.Network.Hosts {
			if hostMatchesPattern(p.Bind.Host, h) {
				return true
			}
		}
		for _, h := range d.Effect.NamedHosts {
			if hostMatchesPattern(p.Bind.Host, h) {
				return true
			}
		}
		return false
	case BindPath:
		return d.WritePath != "" && writablePathCovered([]string{p.Bind.Path}, d.WritePath)
	case BindTool:
		return d.ToolName != "" && p.Bind.Tool == d.ToolName
	}
	return false
}
