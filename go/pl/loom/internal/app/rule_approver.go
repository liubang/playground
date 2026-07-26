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
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// RuleApprover auto-approves prepared calls that match session-persisted
// rules before delegating to the inner approver (the UI). Rules are created
// when the user picks "allow always" on an approval, and are categorical
// command prefixes like ["go", "test"] — never raw full commands.
//
// The store lives in permission.SessionRules and is shared with the policy
// layer, so a remembered prefix also short-circuits policy evaluation
// directly. Only run_cmd calls are rule-eligible: other tools' approvals
// (e.g. edit/write) stay per-call because their blast radius varies by path.
type RuleApprover struct {
	inner   domain.Approver
	session *permission.SessionRules
}

// NewRuleApprover wraps inner with session rule matching. A nil inner
// approver makes every unmatched call deny, which is useful in tests. A nil
// session store disables remembering (rules still match when present).
func NewRuleApprover(inner domain.Approver, session *permission.SessionRules) *RuleApprover {
	return &RuleApprover{inner: inner, session: session}
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
// rule-persisted: shells, eval-form interpreters, destructive programs,
// heredocs, and escalated (no-sandbox) runs.
func (r *RuleApprover) RememberRunCmd(toolName string, arguments json.RawMessage) (prefix []string, ok bool) {
	if toolName != "run_cmd" || r.session == nil {
		return nil, false
	}
	argv, ok := permission.RunCmdArgv(arguments)
	if !ok {
		return nil, false
	}
	return r.session.RememberRunCmd(argv)
}

func (r *RuleApprover) matches(call domain.PreparedCall) bool {
	if call.Call.Name != "run_cmd" || r.session == nil {
		return false
	}
	argv, ok := permission.RunCmdArgv(call.Call.Arguments)
	return ok && r.session.Matches(argv)
}

// RunCmdRuleCount reports how many run_cmd prefixes are remembered (for
// status display and tests).
func (r *RuleApprover) RunCmdRuleCount() int {
	if r.session == nil {
		return 0
	}
	return len(r.session.Prefixes())
}

// RunCmdRulePreview renders the categorical rule that "allow always" would
// create for a run_cmd call (e.g. "go test"), for display in the approval
// overlay. ok=false means the call cannot be remembered (shell, eval-form
// interpreter, heredoc, escalation, or a non-run_cmd tool).
func RunCmdRulePreview(toolName string, arguments json.RawMessage) (string, bool) {
	if toolName != "run_cmd" {
		return "", false
	}
	argv, ok := permission.RunCmdArgv(arguments)
	if !ok {
		return "", false
	}
	prefix, ok := permission.DeriveRunCmdPrefix(argv)
	if !ok {
		return "", false
	}
	return strings.Join(prefix, " "), true
}
