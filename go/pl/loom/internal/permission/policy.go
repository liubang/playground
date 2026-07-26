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
// rule/session matches first (strictest wins), then the risk baseline.
func (p Policy) Evaluate(call domain.PreparedCall) domain.Decision {
	if argv, ok := RunCmdArgv(call.Call.Arguments); ok && call.Call.Name == "run_cmd" {
		if p.Session != nil && p.Session.Matches(argv) {
			return domain.DecisionAllow
		}
		if d, _ := p.Rules.Evaluate(argv); d != "" {
			return d
		}
	}
	return p.evaluateRisk(call.Risk)
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

// PolicyFromEnv builds the effective policy for a workspace: the risk
// baseline plus declarative rules from the user layer (~/.loom/rules) and
// the project layer (<workspace>/.loom/rules).
func PolicyFromEnv(workspaceRoot string, logger *slog.Logger) Policy {
	return AttachRules(DefaultPolicy(), workspaceRoot, logger)
}

// AttachRules loads declarative rules from the user layer (~/.loom/rules)
// and the project layer (<workspace>/.loom/rules) onto the given baseline
// policy. Rule loading never fails the agent — broken files are logged and
// skipped.
//
//	LOOM_RULES=0                 — disable all rule loading
//	LOOM_PROJECT_RULES=0         — disable the project layer entirely
//	LOOM_PROJECT_RULES_ALLOW=1   — let project rules say "allow" (off by
//	                               default: an untrusted checkout may only
//	                               tighten policy, never loosen it)
func AttachRules(policy Policy, workspaceRoot string, logger *slog.Logger) Policy {
	if os.Getenv("LOOM_RULES") == "0" {
		return policy
	}
	var userDir string
	if dir, err := RulesDirUser(); err == nil {
		userDir = dir
	}
	projectDir := ""
	if os.Getenv("LOOM_PROJECT_RULES") != "0" {
		projectDir = RulesDirProject(workspaceRoot)
	}
	opts := LoadOptions{ProjectAllows: os.Getenv("LOOM_PROJECT_RULES_ALLOW") == "1"}
	rules, errs := LoadRuleSets(userDir, projectDir, opts)
	for _, err := range errs {
		logger.Warn("loom rules: skipped a rule source", "error", err)
	}
	if rules.Size() > 0 {
		logger.Info("loom rules loaded", "rules", rules.Size())
	}
	policy.Rules = rules
	return policy
}
