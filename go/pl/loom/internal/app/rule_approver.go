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
// layer (SessionDecider), so a remembered prefix normally resolves at
// policy evaluation time and never reaches this approver; the match here
// is a second line of defense for approvals already in flight. Only
// run_cmd calls are rule-eligible: other tools' approvals (e.g.
// edit/write) stay per-call because their blast radius varies by path.
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
	// Trust selects the remembered flavor: "" remembers the derived
	// minimal-capability grant (recommended), "unsandboxed" remembers L2
	// full trust (only meaningful for escalated calls).
	Trust string
}

// TrustUnsandboxed is the ApprovalRuleHint.Trust value for L2 rememberance.
const TrustUnsandboxed = "unsandboxed"

// RememberedRule is the categorical memory created by an interactive
// approval: an argv prefix (with grant) for run_cmd, or an exact host
// for web_fetch. Label is the display form; Prefix/Host carry the
// structured form for persistence (never re-split from the label).
type RememberedRule struct {
	Label  string
	Prefix []string
	Host   string
	Grant  domain.ExecGrant
}

// RememberCall derives and stores the categorical memory for an approved
// call. ok=false means the call must never be remembered (shells, eval
// forms, destructive programs, heredocs, or unmappable URLs).
func (r *RuleApprover) RememberCall(toolName string, arguments json.RawMessage, trust string) (RememberedRule, bool) {
	if r.session == nil {
		return RememberedRule{}, false
	}
	switch toolName {
	case "run_cmd":
		info, parsed := permission.ParseRunCmdCall(arguments)
		if !parsed {
			return RememberedRule{}, false
		}
		if info.Escalated && trust != TrustUnsandboxed {
			// An escalation can only be remembered as explicit full trust:
			// any lesser grant would not cover the next escalated call
			// (permission.AllowGrantCovers), so a "minimal" memory would be
			// dead weight that never fires.
			return RememberedRule{}, false
		}
		grant := DeriveRememberGrant(info, trust)
		prefix, remembered := r.session.RememberRunCmd(info.Argv, grant)
		if !remembered {
			return RememberedRule{}, false
		}
		return RememberedRule{Label: strings.Join(prefix, " "), Prefix: prefix, Grant: grant}, true
	case "web_fetch":
		host, parsed := permission.ParseWebFetchHost(arguments)
		if !parsed {
			return RememberedRule{}, false
		}
		host, remembered := r.session.RememberDomain(host)
		if !remembered {
			return RememberedRule{}, false
		}
		return RememberedRule{Label: host, Host: host}, true
	}
	return RememberedRule{}, false
}

// DeriveRememberGrant computes the grant a remembered rule should carry
// for the given call and user-chosen trust flavor:
//
//   - trust=unsandboxed on an escalated call → L2 full trust (explicit
//     user opt-in only; the ONLY rememberable flavor for escalations).
//   - needs_network calls → sandboxed network grant.
//   - anything else → zero grant (default sandbox).
func DeriveRememberGrant(info permission.RunCmdCall, trust string) domain.ExecGrant {
	switch {
	case trust == TrustUnsandboxed && info.Escalated:
		return domain.ExecGrant{Unsandboxed: true}
	case info.NeedsNetwork:
		return domain.ExecGrant{NetworkFull: true}
	default:
		return domain.ExecGrant{}
	}
}

func (r *RuleApprover) matches(call domain.PreparedCall) bool {
	if r.session == nil {
		return false
	}
	switch call.Call.Name {
	case "run_cmd":
		info, ok := permission.ParseRunCmdCall(call.Call.Arguments)
		if !ok {
			return false
		}
		grant, matched := r.session.Match(info.Argv)
		// A remembered grant only auto-approves what it covers: a plain
		// (zero-grant) memory must not silently approve an escalated or
		// needs_network request — the user approved the sandboxed form.
		return matched && permission.AllowGrantCovers(grant, info)
	case "web_fetch":
		host, ok := permission.ParseWebFetchHost(call.Call.Arguments)
		return ok && r.session.MatchDomain(host)
	}
	return false
}

// RunCmdRuleCount reports how many run_cmd prefixes are remembered (for
// status display and tests).
func (r *RuleApprover) RunCmdRuleCount() int {
	if r.session == nil {
		return 0
	}
	return len(r.session.Prefixes())
}

// ApprovalRulePreview renders the categorical rule that "allow always"
// would create for a call, plus the grant it would carry, for display in
// the approval overlay: an argv prefix ("go test") for run_cmd, an exact
// host ("www.weather.com.cn") for web_fetch. ok=false means the call
// cannot be remembered.
func ApprovalRulePreview(toolName string, arguments json.RawMessage) (preview string, grant domain.ExecGrant, ok bool) {
	switch toolName {
	case "run_cmd":
		info, parsed := permission.ParseRunCmdCall(arguments)
		if !parsed || info.Escalated {
			// Escalated calls offer only "allow once" and "always trust
			// (unsandboxed)" — a minimal-capability memory could never
			// cover the next escalation, so the option is hidden.
			return "", domain.ExecGrant{}, false
		}
		prefix, ok := permission.DeriveRunCmdPrefix(info.Argv)
		if !ok {
			return "", domain.ExecGrant{}, false
		}
		return strings.Join(prefix, " "), DeriveRememberGrant(info, ""), true
	case "web_fetch":
		host, parsed := permission.ParseWebFetchHost(arguments)
		if !parsed {
			return "", domain.ExecGrant{}, false
		}
		return host, domain.ExecGrant{}, true
	}
	return "", domain.ExecGrant{}, false
}

// RunCmdTrustPreview reports whether the approval overlay should offer the
// "always trust (unsandboxed)" option: escalated run_cmd calls whose
// prefix is derivable.
func RunCmdTrustPreview(toolName string, arguments json.RawMessage) bool {
	if toolName != "run_cmd" {
		return false
	}
	info, ok := permission.ParseRunCmdCall(arguments)
	if !ok || !info.Escalated {
		return false
	}
	_, ok = permission.DeriveRunCmdPrefix(info.Argv)
	return ok
}
