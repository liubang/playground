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

// Policy is the agent-loop facade of the permission model: it owns the
// capability set, the derivation environment, and the approval mode,
// and answers the loop's single question — Evaluate — by deriving the
// call's effect and running the inclusion decision (decide.go).
package permission

import (
	"context"
	"log/slog"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PolicyDecision represents allow/deny/ask.
type PolicyDecision = domain.Decision

// Policy is the assembled permission policy.
type Policy struct {
	// Packages is the evaluated capability set: builtin + user +
	// project + remembered packages, plus the running session's
	// in-memory approvals (scope session).
	Packages *PackageSet
	// Env carries the canonical roots the default sandbox confines.
	Env DeriveEnv
	// Mode is the approval mode (residual strategy).
	Mode ApprovalMode
	// UserIntent enables the user-intent allowance: URL calls targeting
	// a host the user mentioned in the conversation are auto-allowed
	// (interactive modes only). The host snapshot is rebound from the
	// live transcript once per routing pass via WithUserIntent.
	UserIntent bool
	// intentHosts is the current host snapshot (nil until rebound).
	intentHosts map[string]struct{}
}

// DefaultPolicy returns the empty policy: no packages, on-request mode,
// the default sandbox as the only grant.
func DefaultPolicy() Policy {
	return Policy{Packages: NewPackageSet(), Mode: ModeOnRequest}
}

// Evaluate resolves the verdict for one prepared call: derive the
// effect, then decide by inclusion. It satisfies the agent loop's
// Policy interface and never blocks.
func (p Policy) Evaluate(call domain.PreparedCall) domain.Verdict {
	d := DeriveEffect(call, p.Env)
	var hosts map[string]struct{}
	if p.UserIntent && p.Mode != ModeNever {
		hosts = p.intentHosts
	}
	return p.Packages.Decide(d, p.Mode, hosts)
}

// DeriveCall exposes the derivation for the approval flow (the ask
// verdict's UI wants the effect's description and indicators without
// re-deriving).
func (p Policy) DeriveCall(call domain.PreparedCall) Derivation {
	return DeriveEffect(call, p.Env)
}

// WithUserIntent returns a copy of the policy with the user-intent host
// snapshot rebound. The copy is cheap (the capability set is shared and
// immutable during a routing pass), so concurrent runs never observe
// each other's transcripts.
func (p Policy) WithUserIntent(hosts map[string]struct{}) Policy {
	p.intentHosts = hosts
	return p
}

// PackageLoadOptions selects which declarative layers load onto the
// capability set. Values come from the config file's rules.* section.
type PackageLoadOptions struct {
	// Enabled=false skips all loading (including the builtin set).
	Enabled bool
	// Builtin=false skips only the embedded set.
	Builtin bool
	// Project=false skips the project layer (<workspace>/.loom/rules).
	Project bool
	// ProjectAllow lets project packages say "allow" (off by default:
	// an untrusted checkout may only tighten policy, never loosen it).
	ProjectAllow bool
}

// AttachPackages (re)loads the declarative layers of the given
// capability set — the embedded builtin set, the user layer (userDir,
// i.e. <loom home>/rules), the project layer
// (<workspace>/.loom/rules), and the SQLite remembered store under
// userDir — atomically replacing every non-session package, so config
// hot-reloads and rule-file edits take effect while the session's
// in-memory approvals survive. Loading never fails the agent — broken
// files/stores are logged and skipped.
func AttachPackages(ctx context.Context, set *PackageSet, workspaceRoot, userDir string, opts PackageLoadOptions, logger *slog.Logger) {
	if set == nil {
		return
	}
	if !opts.Enabled {
		set.ReplaceLayers()
		return
	}
	var loaded []Package
	if opts.Builtin {
		if builtin, err := LoadBuiltinPackages(); err != nil {
			// Broken embedded packages are a build-time bug; never
			// break the agent.
			logger.Warn("loom packages: builtin set rejected", "error", err)
		} else {
			loaded = append(loaded, builtin...)
		}
	}
	projectDir := ""
	if opts.Project {
		projectDir = RulesDirProject(workspaceRoot)
	}
	filePackages, errs := LoadPackageSets(userDir, projectDir, LoadOptions{ProjectAllows: opts.ProjectAllow})
	for _, err := range errs {
		logger.Warn("loom packages: skipped a package source", "error", err)
	}
	loaded = append(loaded, filePackages...)
	if userDir != "" {
		if remembered, err := LoadRememberedPackages(ctx, RememberedDBPath(userDir)); err != nil {
			logger.Warn("loom packages: remembered store unreadable", "error", err)
		} else {
			loaded = append(loaded, remembered...)
		}
	}
	set.ReplaceLayers(loaded...)
	if len(loaded) > 0 {
		logger.Info("loom packages loaded", "packages", len(loaded))
	}
}

// ReplaceLayers atomically swaps every non-session package (the
// declarative layers) while keeping the session's in-memory approvals.
func (s *PackageSet) ReplaceLayers(pkgs ...Package) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	kept := s.packages[:0]
	for _, p := range s.packages {
		if p.Scope == ScopeSession {
			kept = append(kept, p)
		}
	}
	s.packages = append(kept, pkgs...)
}

// RememberSession records an interactive approval as a session-scope
// package (latest approval wins on the same binding). Only allow
// packages are accepted: deny/ask policy belongs in files, not in
// interactive memory.
func (s *PackageSet) RememberSession(pkg Package) bool {
	if s == nil || pkg.Decision != domain.DecisionAllow {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	pkg.Scope = ScopeSession
	for i, existing := range s.packages {
		if bindingsEqual(existing.Bind, pkg.Bind) && existing.Scope == ScopeSession {
			s.packages[i] = pkg // latest approval wins
			return true
		}
	}
	s.packages = append(s.packages, pkg)
	return true
}

// ForgetSession removes a session-scope package by binding. ok=false
// means the binding was not remembered this session.
func (s *PackageSet) ForgetSession(bind Binding) bool {
	if s == nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, p := range s.packages {
		if p.Scope == ScopeSession && bindingsEqual(p.Bind, bind) {
			s.packages = append(s.packages[:i], s.packages[i+1:]...)
			return true
		}
	}
	return false
}

// bindingsEqual compares two bindings exactly (kind plus content).
func bindingsEqual(a, b Binding) bool {
	if a.Kind != b.Kind {
		return false
	}
	switch a.Kind {
	case BindArgv, BindArgvExact:
		return stringSliceEqual(a.Argv, b.Argv)
	case BindHost:
		return a.Host == b.Host
	case BindPath:
		return a.Path == b.Path
	case BindTool:
		return a.Tool == b.Tool
	}
	return false
}
