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

package permission

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PolicyDecision represents allow/deny/ask.
type PolicyDecision = domain.Decision

// Policy is the assembly parameter pack for the decider chain
// (docs/PERMISSION_DESIGN.md §4): declarative rules plus session memory.
// Evaluation itself lives in the deciders (decider.go); Policy.Decider
// wires them in strictest-first order.
type Policy struct {
	// Rules are declarative argv-prefix rules loaded from user/project
	// layers (nil = none).
	Rules *RuleSet
	// Session holds categorical prefixes remembered from interactive
	// "allow always" decisions (nil = none). Only run_cmd calls are
	// session-rule eligible.
	Session *SessionRules
	// UserIntent enables the user-intent decider: URL calls targeting a
	// host the user mentioned in the conversation are auto-allowed
	// (interactive modes only — never mode keeps its strict unattended
	// contract). The decider enters the chain with an empty snapshot;
	// each routing pass rebinds it from the live transcript via
	// Chain.WithUserIntent.
	UserIntent bool
}

// DefaultPolicy returns the baseline security policy per §12.1.
func DefaultPolicy() Policy {
	return Policy{}
}

// Decider assembles the strategy chain for the given approval mode:
//
//	rules (strictest wins, incl. deny) → danger heuristics → user intent
//	(hosts the user mentioned; interactive modes only, when enabled) →
//	session memory → mode-aware baseline (always terminal)
//
// Swapping a strategy means swapping one chain element. The returned
// Chain satisfies the agent's Policy interface (Evaluate → domain.Verdict)
// and never produces a nil verdict.
func (p Policy) Decider(mode ApprovalMode) Chain {
	chain := Chain{
		RuleDecider{Rules: p.Rules},
		DangerDecider{Mode: mode},
	}
	if p.UserIntent && mode != ModeNever {
		chain = append(chain, UserIntentDecider{})
	}
	return append(chain,
		SessionDecider{Session: p.Session},
		BaselineDecider{Mode: mode},
	)
}

// trustedProgramDirs are candidate system directories whose executables
// may resolve through basename rules (/bin/ls → ls) — but only after a
// runtime check proves the directory is trustworthy (isTrustedProgramDir).
// Anything else must match rules by full path: an attacker-writable
// directory must never gain basename trust (an evil /tmp/ls is NOT ls).
var trustedProgramDirs = []string{
	"/bin", "/sbin", "/usr/bin", "/usr/sbin",
	"/usr/local/bin", "/usr/local/sbin", "/opt/homebrew/bin",
}

// isTrustedProgramDir reports whether dir is a candidate directory whose
// ownership and permissions actually justify basename trust: it must be
// root-owned and not writable by group or others. A static allowlist is
// not enough — /opt/homebrew/bin is owned by the daily login user (and
// group-writable) on any Homebrew machine, so a trojaned binary there
// would otherwise inherit the trust of bare-name rules.
func isTrustedProgramDir(dir string) bool {
	listed := false
	for _, d := range trustedProgramDirs {
		if dir == d {
			listed = true
			break
		}
	}
	if !listed {
		return false
	}
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		return false
	}
	return info.Mode().Perm()&0o022 == 0 && dirOwnedByRoot(info)
}

// NormalizeTrustedPath rewrites argv[0] to its basename when it lives in a
// verified trusted system directory, so bare-name rules match absolute
// invocations.
func NormalizeTrustedPath(argv []string) ([]string, bool) {
	if len(argv) == 0 {
		return nil, false
	}
	dir := filepath.Dir(argv[0])
	base := filepath.Base(argv[0])
	if base == argv[0] {
		return nil, false // already bare
	}
	if isTrustedProgramDir(dir) {
		return append([]string{base}, argv[1:]...), true
	}
	return nil, false
}

// RuleLoadOptions selects which declarative rule layers load onto the
// policy baseline. Values come from the config file's rules.* section
// (resolved by internal/config).
type RuleLoadOptions struct {
	// Enabled=false skips all rule loading (including the builtin set).
	Enabled bool
	// Builtin=false skips only the embedded read-only set.
	Builtin bool
	// Project=false skips the project layer (<workspace>/.loom/rules).
	Project bool
	// ProjectAllow lets project rules say "allow" (off by default: an
	// untrusted checkout may only tighten policy, never loosen it).
	ProjectAllow bool
}

// AttachRules loads declarative rules onto the given baseline policy: the
// embedded builtin set, plus the user layer (userDir, i.e. <loom home>/rules)
// and the project layer (<workspace>/.loom/rules), plus the SQLite
// remembered store under userDir. Rule loading never fails the agent —
// broken files/stores are logged and skipped.
func AttachRules(ctx context.Context, policy Policy, workspaceRoot, userDir string, loadOpts RuleLoadOptions, logger *slog.Logger) Policy {
	if !loadOpts.Enabled {
		return policy
	}
	rules := &RuleSet{}
	if loadOpts.Builtin {
		if builtin, err := LoadBuiltinRules(); err != nil {
			// Broken embedded rules are a build-time bug; never break the agent.
			logger.Warn("loom rules: builtin set rejected", "error", err)
		} else {
			rules.merge(builtin)
		}
	}
	projectDir := ""
	if loadOpts.Project {
		projectDir = RulesDirProject(workspaceRoot)
	}
	opts := LoadOptions{ProjectAllows: loadOpts.ProjectAllow}
	fileRules, errs := LoadRuleSets(userDir, projectDir, opts)
	for _, err := range errs {
		logger.Warn("loom rules: skipped a rule source", "error", err)
	}
	rules.merge(fileRules)
	// Remembered store (SQLite): machine-managed "allow always" memory.
	if userDir != "" {
		if remembered, err := LoadRememberedRules(ctx, RememberedDBPath(userDir)); err != nil {
			logger.Warn("loom rules: remembered store unreadable", "error", err)
		} else if remembered.HasAny() {
			rules.merge(remembered)
		}
	}
	if rules.HasAny() {
		logger.Info("loom rules loaded", "rules", len(rules.Rules()), "domains", len(rules.Domains()), "tools", len(rules.Tools()))
	}
	policy.Rules = rules
	return policy
}
