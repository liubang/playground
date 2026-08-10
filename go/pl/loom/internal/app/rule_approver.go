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
// approval: argv prefixes (with grant) for run_cmd — one per subcommand
// for a composed shell command — an exact host for web_fetch, or a bare
// tool name for the eligible fixed-blast-radius tools
// (permission.ToolMemoryEligible). Label is the display form;
// Prefixes/Host/Tool carry the structured form for persistence (never
// re-split from the label).
type RememberedRule struct {
	Label    string
	Prefixes [][]string
	Host     string
	Tool     string
	Grant    domain.ExecGrant
}

// RememberCall derives and stores the categorical memory for an approved
// call. ok=false means the call must never be remembered (shells, eval
// forms, destructive programs, heredocs, or unmappable URLs).
func (r *RuleApprover) RememberCall(toolName string, arguments json.RawMessage, trust string) (RememberedRule, bool) {
	if r.session == nil {
		return RememberedRule{}, false
	}
	switch toolName {
	case "run_cmd", "exec_session":
		// exec_session shares run_cmd's program/args/sandbox_permissions
		// argument shape, so the same parser and prefix derivation apply.
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
		prefixes, remembered := r.session.RememberRunCmd(info, grant)
		if !remembered {
			return RememberedRule{}, false
		}
		labels := make([]string, 0, len(prefixes))
		for _, prefix := range prefixes {
			labels = append(labels, strings.Join(prefix, " "))
		}
		return RememberedRule{Label: strings.Join(labels, " && "), Prefixes: prefixes, Grant: grant}, true
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
	default:
		// Tool-name memory: SessionRules.RememberTool gates eligibility, so
		// path/host-varying tools (edit, write, view_image, ...) keep their
		// per-call approvals.
		name, remembered := r.session.RememberTool(toolName)
		if !remembered {
			return RememberedRule{}, false
		}
		return RememberedRule{Label: name, Tool: name}, true
	}
}

// DeriveRememberGrant computes the grant a remembered rule should carry
// for the given call and user-chosen trust flavor:
//
//   - trust=unsandboxed on an escalated call → L2 full trust (explicit
//     user opt-in only; the ONLY rememberable flavor for escalations).
//   - otherwise → exactly the declared capabilities (network, gui_open);
//     zero grant when nothing was declared.
func DeriveRememberGrant(info permission.RunCmdCall, trust string) domain.ExecGrant {
	if trust == TrustUnsandboxed && info.Escalated {
		return domain.ExecGrant{Unsandboxed: true}
	}
	return permission.DeclaredGrant(info)
}

func (r *RuleApprover) matches(call domain.PreparedCall) bool {
	if r.session == nil {
		return false
	}
	if info, ok := permission.ExecInfoOf(call); ok {
		var (
			grant   domain.ExecGrant
			matched bool
		)
		if info.Shell != nil {
			if !info.Shell.Static || info.Shell.DynamicWrites {
				return false
			}
			argvs, provable := info.ShellCommandArgvs()
			if !provable {
				return false
			}
			grant, matched = r.session.MatchAll(argvs)
		} else {
			grant, matched = r.session.Match(info.Argv)
		}
		// A remembered grant only auto-approves what it covers: a plain
		// (zero-grant) memory must not silently approve an escalated or
		// needs_network request — the user approved the sandboxed form.
		return matched && permission.AllowGrantCovers(grant, info)
	}
	if call.Call.Name == "web_fetch" {
		host, ok := permission.ParseWebFetchHost(call.Call.Arguments)
		return ok && r.session.MatchDomain(host)
	}
	return r.session.MatchTool(call.Call.Name)
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
// host ("www.weather.com.cn") for web_fetch, the bare tool name for the
// eligible fixed-blast-radius tools. ok=false means the call cannot be
// remembered.
func ApprovalRulePreview(toolName string, arguments json.RawMessage) (preview string, grant domain.ExecGrant, ok bool) {
	switch toolName {
	case "run_cmd", "exec_session":
		info, parsed := permission.ParseRunCmdCall(arguments)
		if !parsed || info.Escalated {
			// Escalated calls offer only "allow once" and "always trust
			// (unsandboxed)" — a minimal-capability memory could never
			// cover the next escalation, so the option is hidden.
			return "", domain.ExecGrant{}, false
		}
		prefixes, ok := permission.DeriveRunCmdPrefixes(info)
		if !ok {
			return "", domain.ExecGrant{}, false
		}
		labels := make([]string, 0, len(prefixes))
		for _, prefix := range prefixes {
			labels = append(labels, strings.Join(prefix, " "))
		}
		return strings.Join(labels, " && "), DeriveRememberGrant(info, ""), true
	case "web_fetch":
		host, parsed := permission.ParseWebFetchHost(arguments)
		if !parsed {
			return "", domain.ExecGrant{}, false
		}
		return host, domain.ExecGrant{}, true
	default:
		canonical, ok := permission.ToolMemoryEligible(toolName)
		if !ok {
			return "", domain.ExecGrant{}, false
		}
		return canonical, domain.ExecGrant{}, true
	}
}

// RunCmdTrustPreview reports whether the approval overlay should offer the
// "always trust (unsandboxed)" option: escalated run_cmd calls whose
// prefix is derivable.
func RunCmdTrustPreview(toolName string, arguments json.RawMessage) bool {
	if toolName != "run_cmd" && toolName != "exec_session" {
		return false
	}
	info, ok := permission.ParseRunCmdCall(arguments)
	if !ok || !info.Escalated {
		return false
	}
	_, ok = permission.DeriveRunCmdPrefixes(info)
	return ok
}
