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

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// RuleApprover delegates to the inner approver (the UI) and records
// "allow always" decisions as capability packages — session-scope in
// the shared PackageSet (immediately effective for the policy), plus
// the persistent remembered store when one is configured.
//
// A remembered package normally resolves at policy evaluation time and
// never reaches this approver; the second-line evaluation in
// RequestApproval covers approvals remembered DURING another call's
// pending approval (the policy's shared set already contains them).
type RuleApprover struct {
	inner  domain.Approver
	policy func() permission.Policy
	store  *permission.RememberedStore
}

// NewRuleApprover wraps inner with the policy's shared capability set.
// policy is resolved per call so hot-reloaded modes and freshly
// remembered packages are always seen. A nil inner approver makes
// every call deny (useful in tests).
func NewRuleApprover(inner domain.Approver, policy func() permission.Policy, store *permission.RememberedStore) *RuleApprover {
	return &RuleApprover{inner: inner, policy: policy, store: store}
}

// RequestApproval re-evaluates the call against the (possibly freshly
// enriched) capability set before bothering the user; everything else
// reaches the inner approver as usual.
func (r *RuleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	if r.policy != nil {
		policy := r.policy()
		if policy.Packages != nil {
			d := deriveForApproval(req.Call, policy.Env)
			if v := policy.Packages.Decide(d, policy.Mode, nil); v.Decision == domain.DecisionAllow {
				return domain.DecisionAllow, nil
			}
		}
	}
	if r.inner == nil {
		return domain.DecisionDeny, nil
	}
	return r.inner.RequestApproval(ctx, req)
}

// deriveForApproval derives the call for the approver-layer second
// line. The call arriving from the agent loop carries its real
// Definition (source, risk), so the single DeriveEffect entry classifies
// it exactly as the policy path did — including MCP identity, which a
// raw-arguments re-derivation would lose.
func deriveForApproval(call domain.PreparedCall, env permission.DeriveEnv) permission.Derivation {
	return permission.DeriveEffect(call, env)
}

// ApprovalRuleHint carries the raw call arguments the frontend got with
// the approval request, so a remembered decision can derive packages.
type ApprovalRuleHint struct {
	ToolName  string
	Arguments json.RawMessage
	// Trust selects the remembered flavor: "" remembers the derived
	// minimal-capability grant (recommended), "unsandboxed" remembers
	// L2 full trust (only meaningful for escalated calls).
	Trust string
}

// TrustUnsandboxed is the ApprovalRuleHint.Trust value for L2
// remembrance, re-exported so this layer's callers need no permission
// import for the constant.
const TrustUnsandboxed = permission.TrustUnsandboxed

// RememberedRule is the display-facing summary of what an "allow
// always" approval recorded: a human label plus the packages that were
// written (session scope, and persisted when a store is configured).
type RememberedRule struct {
	Label    string
	Packages []permission.Package
}

// RememberCall derives and stores the capability packages for an
// approved call. ok=false means the call must stay per-call (dynamic
// shells, eval forms, multi-step indicated shapes, ineligible tools).
// The memory shape is derived from the call's DERIVATION
// (permission.DeriveRawArgs) — this layer never switches on tool names.
func (r *RuleApprover) RememberCall(toolName string, arguments json.RawMessage, trust string) (RememberedRule, bool) {
	if r.policy == nil {
		return RememberedRule{}, false
	}
	policy := r.policy()
	if policy.Packages == nil {
		return RememberedRule{}, false
	}
	d := permission.DeriveRawArgs(toolName, arguments, policy.Env)
	label, pkgs, ok := permission.MemoryPreviewLabel(d, trust)
	if !ok {
		return RememberedRule{}, false
	}
	for _, pkg := range pkgs {
		policy.Packages.RememberSession(pkg)
		if r.store != nil {
			// Persistence is best-effort: the session package is
			// already effective, and a store hiccup must not lose the
			// user's approval.
			_ = r.store.Remember(context.Background(), pkg)
		}
	}
	return RememberedRule{Label: label, Packages: pkgs}, true
}

// ApprovalRulePreview renders the packages that "allow always" would
// record for a call, for display in the approval overlay: categorical
// argv prefixes ("go test"), an exact host, a writable directory, or a
// bare tool name. ok=false means the call cannot be remembered — for
// escalated calls the minimal flavor is hidden by design (only the
// explicit unsandboxed trust option is offered).
func ApprovalRulePreview(toolName string, arguments json.RawMessage, env permission.DeriveEnv) (preview string, grant domain.ExecGrant, ok bool) {
	d := permission.DeriveRawArgs(toolName, arguments, env)
	if d.Effect.Unsandboxed {
		return "", domain.ExecGrant{}, false
	}
	label, pkgs, ok := permission.MemoryPreviewLabel(d, "")
	if !ok || len(pkgs) == 0 {
		return "", domain.ExecGrant{}, false
	}
	return label, pkgs[0].Grant.ExecGrant(), true
}

// RunCmdTrustPreview reports whether the approval overlay should offer
// the "always trust (unsandboxed)" option: escalated exec calls whose
// memory shape is derivable.
func RunCmdTrustPreview(toolName string, arguments json.RawMessage, env permission.DeriveEnv) bool {
	d := permission.DeriveRawArgs(toolName, arguments, env)
	if !d.Effect.Unsandboxed {
		return false
	}
	_, ok := permission.MemoryPackages(d, TrustUnsandboxed)
	return ok
}

// approvalConsequence renders the consequence-oriented summary of a
// call's derived effect for the approval card: what the operation DOES
// (plus any danger indicators), empty when the call is fully confined.
func approvalConsequence(toolName string, arguments json.RawMessage, env permission.DeriveEnv) string {
	d := permission.DeriveRawArgs(toolName, arguments, env)
	e := d.Effect
	if !e.CrossesBoundary() && e.Consequence == permission.ConsequenceConfined && len(e.Indicators) == 0 && e.Proven {
		return ""
	}
	desc := e.Describe()
	if len(e.Indicators) > 0 {
		desc += " ⚠ " + e.Indicators[0]
		if len(e.Indicators) > 1 {
			desc += " 等"
		}
	}
	return desc
}
