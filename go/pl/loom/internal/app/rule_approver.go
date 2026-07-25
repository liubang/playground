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
// Created: 2026/07/25

package app

import (
	"context"
	"encoding/json"
	"regexp"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// RuleApprover auto-approves prepared calls that match session-persisted
// rules before delegating to the inner approver (the UI). Rules are created
// when the user picks "allow always" on an approval, and are categorical
// command prefixes like ["go", "test"] — never raw full commands.
//
// Persistence is session-scoped; durable cross-session rules are a separate
// follow-up. Only run_cmd calls are rule-eligible: other tools' approvals
// (e.g. edit/write) stay per-call because their blast radius varies by path.
type RuleApprover struct {
	inner domain.Approver

	mu             sync.RWMutex
	runCmdPrefixes [][]string
}

// NewRuleApprover wraps inner with session rule matching. A nil inner
// approver makes every unmatched call deny, which is useful in tests.
func NewRuleApprover(inner domain.Approver) *RuleApprover {
	return &RuleApprover{inner: inner}
}

// RequestApproval auto-allows rule-matching calls; everything else reaches
// the inner approver (and thus the user) as usual.
func (r *RuleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	if r.matches(req.Call) {
		return domain.DecisionAllow, nil
	}
	if r.inner == nil {
		return domain.DecisionDeny, nil
	}
	return r.inner.RequestApproval(ctx, req)
}

// ApprovalRuleHint carries the raw call arguments the frontend got with the
// approval request, so a remembered decision can derive a categorical rule.
type ApprovalRuleHint struct {
	ToolName  string
	Arguments json.RawMessage
}

// RememberRunCmd derives and stores a categorical prefix rule for a run_cmd
// call, returning the stored prefix. ok=false means the call must never be
// rule-persisted: shells, destructive programs, generic interpreters without
// a subcommand, heredocs, and escalated (no-sandbox) runs.
func (r *RuleApprover) RememberRunCmd(toolName string, arguments json.RawMessage) (prefix []string, ok bool) {
	if toolName != "run_cmd" {
		return nil, false
	}
	argv, ok := runCmdArgv(arguments)
	if !ok {
		return nil, false
	}
	prefix, ok = DeriveRunCmdPrefix(argv)
	if !ok {
		return nil, false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, existing := range r.runCmdPrefixes {
		if stringSliceEqual(existing, prefix) {
			return prefix, true
		}
	}
	r.runCmdPrefixes = append(r.runCmdPrefixes, prefix)
	return prefix, true
}

func (r *RuleApprover) matches(call domain.PreparedCall) bool {
	if call.Call.Name != "run_cmd" {
		return false
	}
	argv, ok := runCmdArgv(call.Call.Arguments)
	if !ok {
		return false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, prefix := range r.runCmdPrefixes {
		if argvHasPrefix(argv, prefix) {
			return true
		}
	}
	return false
}

// RunCmdRuleCount reports how many run_cmd prefixes are remembered (for
// status display and tests).
func (r *RuleApprover) RunCmdRuleCount() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.runCmdPrefixes)
}

// subcommandToken matches simple subcommand words (test, run, vet, download,
// pr, --free-form not allowed) used to widen a rule from ["go"] to
// ["go", "test"] without allowing arbitrary scripting.
var subcommandToken = regexp.MustCompile(`^[a-z][a-z0-9_+-]*$`)

// neverPersistPrograms must never start a persisted rule: shells compose
// arbitrary commands, interpreters execute arbitrary scripts, and destructive
// programs do not deserve a standing approval.
var neverPersistPrograms = map[string]struct{}{
	"rm": {}, "sudo": {}, "su": {}, "dd": {}, "mkfs": {}, "shred": {},
	"python": {}, "python3": {}, "node": {}, "ruby": {}, "perl": {},
}

// subcommandedPrograms are the known subcommand-style tools for which the
// first positional argument is a categorical subcommand worth including in
// the rule (["go", "test"] instead of the too-broad ["go"]). For every
// other program the first argument is data, not a subcommand
// ("rg pattern", "cat file"), so the rule stays [program].
var subcommandedPrograms = map[string]struct{}{
	"go": {}, "npm": {}, "npx": {}, "pnpm": {}, "yarn": {}, "cargo": {},
	"git": {}, "bazel": {}, "docker": {}, "kubectl": {}, "helm": {}, "gh": {},
	"golangci-lint": {}, "ruff": {}, "eslint": {}, "tsc": {},
}

// DeriveRunCmdPrefix computes the categorical rule prefix for a run_cmd argv
// ([program, ...args]): [program] plus the first subcommand-like positional
// argument, e.g. ["go", "test"] from ["go", "test", "./..."]. ok=false when
// the call must not be persisted (see neverPersistPrograms, shells, heredocs,
// escalated runs).
func DeriveRunCmdPrefix(argv []string) ([]string, bool) {
	if len(argv) == 0 {
		return nil, false
	}
	program := argv[0]
	if process.IsShellProgram(program) {
		return nil, false
	}
	base := program
	if idx := strings.LastIndexAny(base, `/\\`); idx >= 0 {
		base = base[idx+1:]
	}
	if _, banned := neverPersistPrograms[strings.ToLower(base)]; banned {
		return nil, false
	}
	for _, arg := range argv[1:] {
		if strings.Contains(arg, "<<") {
			return nil, false
		}
	}
	prefix := []string{program}
	if _, subcommanded := subcommandedPrograms[strings.ToLower(base)]; !subcommanded {
		return prefix, true
	}
	for _, arg := range argv[1:] {
		if strings.HasPrefix(arg, "-") {
			break
		}
		if subcommandToken.MatchString(arg) {
			prefix = append(prefix, arg)
		}
		break
	}
	return prefix, true
}

// RunCmdRulePreview renders the categorical rule that "allow always" would
// create for a run_cmd call (e.g. "go test"), for display in the approval
// overlay. ok=false means the call cannot be remembered (shell, interpreter
// without subcommand, heredoc, escalation, or a non-run_cmd tool).
func RunCmdRulePreview(toolName string, arguments json.RawMessage) (string, bool) {
	if toolName != "run_cmd" {
		return "", false
	}
	argv, ok := runCmdArgv(arguments)
	if !ok {
		return "", false
	}
	prefix, ok := DeriveRunCmdPrefix(argv)
	if !ok {
		return "", false
	}
	return strings.Join(prefix, " "), true
}

// runCmdArgv extracts [program, ...args] from run_cmd call arguments.
// Escalated calls are never rule-eligible.
func runCmdArgv(raw json.RawMessage) ([]string, bool) {
	var args struct {
		Program            string   `json:"program"`
		Args               []string `json:"args"`
		SandboxPermissions string   `json:"sandbox_permissions"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.Program == "" {
		return nil, false
	}
	if args.SandboxPermissions == "require_escalated" {
		return nil, false
	}
	return append([]string{args.Program}, args.Args...), true
}

func argvHasPrefix(argv, prefix []string) bool {
	if len(prefix) == 0 || len(argv) < len(prefix) {
		return false
	}
	for i := range prefix {
		if argv[i] != prefix[i] {
			return false
		}
	}
	return true
}

func stringSliceEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
