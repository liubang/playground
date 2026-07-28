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
	"log/slog"
	"os"
	"path/filepath"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PolicyDecision represents allow/deny/ask.
type PolicyDecision = domain.Decision

// Policy evaluates tool calls against security policy. Evaluation is
// prepared-call aware (not just risk-level): declarative rules and
// session-remembered prefixes are consulted before the risk baseline.
type Policy struct {
	// AutoApproveR1 automatically approves R0 and R1 risk operations.
	AutoApproveR1 bool
	// AskR2 prompts the user for R2 operations (default: true).
	AskR2 bool
	// DenyR4 denies R4 operations by default.
	DenyR4 bool
	// Rules are declarative argv-prefix rules loaded from user/project
	// layers (nil = none).
	Rules *RuleSet
	// Session holds categorical prefixes remembered from interactive
	// "allow always" decisions (nil = none). Only run_cmd calls are
	// session-rule eligible.
	Session *SessionRules
}

// DefaultPolicy returns the baseline security policy per §12.1.
func DefaultPolicy() Policy {
	return Policy{
		AutoApproveR1: true,
		AskR2:         true,
		DenyR4:        true,
	}
}

// Evaluate returns the policy decision for a prepared tool call: exact
// rule/session matches first (strictest wins across ALL sources), then the
// risk baseline.
func (p Policy) Evaluate(call domain.PreparedCall) domain.Decision {
	if call.Call.Name != "run_cmd" {
		return p.evaluateRisk(call.Risk)
	}
	argv, ok := RunCmdArgv(call.Call.Arguments)
	if !ok {
		return p.evaluateRisk(call.Risk)
	}
	// File-layer rules (builtin + user + project) are consulted first;
	// basename normalization lets absolute paths in trusted system dirs hit
	// bare-name rules (/bin/ls matches [ls]).
	best, _ := p.Rules.Evaluate(argv)
	if best == "" {
		if norm, ok := NormalizeTrustedPath(argv); ok {
			best, _ = p.Rules.Evaluate(norm)
		}
	}
	// Session memory may only upgrade "no match" to allow — a remembered
	// prefix must never override a file-layer deny or ask (strictest wins).
	if best == "" && p.Session != nil && p.Session.Matches(argv) {
		best = domain.DecisionAllow
	}
	if best != "" {
		return best
	}
	return p.evaluateRisk(call.Risk)
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

// evaluateRisk returns the baseline decision for a risk level.
func (p Policy) evaluateRisk(risk domain.RiskLevel) PolicyDecision {
	switch {
	case risk <= domain.R1 && p.AutoApproveR1:
		return domain.DecisionAllow
	case risk == domain.R2 && p.AskR2:
		return domain.DecisionAsk
	case risk >= domain.R4 && p.DenyR4:
		return domain.DecisionDeny
	case risk == domain.R3:
		return domain.DecisionAsk
	default:
		return domain.DecisionDeny
	}
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
// embedded builtin set, plus the user layer (~/.loom/rules) and the project
// layer (<workspace>/.loom/rules). Rule loading never fails the agent —
// broken files are logged and skipped.
func AttachRules(policy Policy, workspaceRoot string, loadOpts RuleLoadOptions, logger *slog.Logger) Policy {
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
	var userDir string
	if dir, err := RulesDirUser(); err == nil {
		userDir = dir
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
	if rules.Size() > 0 {
		logger.Info("loom rules loaded", "rules", rules.Size())
	}
	policy.Rules = rules
	return policy
}
