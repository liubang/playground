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
// Created: 2026/08/17

// Memory shape derivation for interactive "allow always" approvals.
//
// The app layer's RuleApprover needs to derive a categorical memory from an
// approved call so later matching calls can be auto-approved. The shape of
// that memory depends on the TYPE of request the call carries — not the
// tool NAME:
//
//   - ExecRequest (run_cmd, exec_session) → argv-prefix memory with a grant.
//   - URLRequest (web_fetch, browser navigate) → exact-host domain memory.
//   - WriteRequest outside the roots (write, edit) → writable-directory
//     path memory.
//   - Neither (generate_image, web_search, MCP tools) → tool-name memory.
//
// This file is the single place that maps a PreparedCall to a MemoryShape,
// so the app layer never switches on tool names and adding a new tool with
// an existing request type is automatically covered.

package permission

import (
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// TrustUnsandboxed is the trust-flavor value for L2 full-trust memory
// (explicit user opt-in for escalated calls). The app layer's
// ApprovalRuleHint.Trust uses the same string.
const TrustUnsandboxed = "unsandboxed"

// MemoryKind identifies the categorical shape of a remembered approval.
type MemoryKind int

const (
	// MemoryNone means the call cannot be remembered categorically
	// (dynamic shells, eval forms, destructive programs, unmappable URLs).
	MemoryNone MemoryKind = iota
	// MemoryArgv means the call carries an ExecRequest and is remembered
	// by argv prefix with a capability grant (run_cmd, exec_session).
	MemoryArgv
	// MemoryHost means the call carries a URLRequest and is remembered
	// by exact host (web_fetch, browser navigate).
	MemoryHost
	// MemoryPath means the call carries a boundary-crossing WriteRequest
	// and is remembered by writable directory (write, edit): approving
	// "always allow" whitelists the target's PARENT DIRECTORY, so later
	// writes anywhere under it stop prompting.
	MemoryPath
	// MemoryTool means the call carries no typed request and is remembered
	// by tool name (generate_image, web_search, MCP tools).
	MemoryTool
)

// MemoryShape describes the categorical memory derivable from an approved
// call. Kind=MemoryNone means the call must stay per-call.
type MemoryShape struct {
	Kind     MemoryKind
	Info     RunCmdCall // valid when Kind=MemoryArgv
	Host     string     // valid when Kind=MemoryHost
	Dir      string     // valid when Kind=MemoryPath (canonical absolute dir)
	ToolName string     // valid when Kind=MemoryTool (canonical, normalized)
}

// DeriveMemoryShape inspects a prepared call's typed request fields to
// determine which categorical memory shape applies, WITHOUT switching on
// the tool name. The typed ExecRequest and URLRequest (signed by the
// producing tool during Prepare) are authoritative; raw-argument parsing
// is the fallback for calls constructed outside Prepare (tests, approval
// UI boundary).
//
// This is the single function the app layer calls to derive "allow always"
// memory, replacing the per-tool switch that used to live in
// rule_approver.go.
func DeriveMemoryShape(call domain.PreparedCall) MemoryShape {
	// ExecRequest: argv-prefix memory (run_cmd, exec_session, ...).
	info, ok := ExecInfoOf(call)
	if ok {
		// DeriveRunCmdPrefixes determines whether the argv is
		// persistable — dynamic shells, eval forms, and heredocs are
		// not. The app layer still needs the trust/escalation gate,
		// but the shape selection is: if it parses as exec, it's
		// MemoryArgv (DeriveRunCmdPrefixes will report !ok inside the
		// app layer for the non-persistable subset).
		return MemoryShape{Kind: MemoryArgv, Info: info}
	}

	// URLRequest: exact-host domain memory (web_fetch, browser navigate).
	urlInfo, ok := URLInfoOf(call)
	if ok {
		return MemoryShape{Kind: MemoryHost, Host: urlInfo.Host}
	}

	// WriteRequest outside the roots: writable-directory path memory
	// (write, edit). Workspace-confined writes never reach an approval,
	// so they never arrive here.
	if writeInfo, ok := WriteInfoOf(call); ok {
		return MemoryShape{Kind: MemoryPath, Dir: filepath.Dir(writeInfo.Path)}
	}

	// No typed request: tool-name memory (only eligible tools).
	canonical, eligible := ToolMemoryEligible(call.Call.Name)
	if !eligible {
		return MemoryShape{Kind: MemoryNone}
	}
	return MemoryShape{Kind: MemoryTool, ToolName: canonical}
}

// PreviewLabel renders the display form of the memory shape for the
// approval overlay's "allow always" option. ok=false means the call
// cannot be remembered.
func (s MemoryShape) PreviewLabel() (label string, grant domain.ExecGrant, ok bool) {
	switch s.Kind {
	case MemoryArgv:
		prefixes, derivable := DeriveRunCmdPrefixes(s.Info)
		if !derivable {
			return "", domain.ExecGrant{}, false
		}
		labels := make([]string, 0, len(prefixes))
		for _, prefix := range prefixes {
			labels = append(labels, strings.Join(prefix, " "))
		}
		return strings.Join(labels, " && "), DeriveRememberGrant(s.Info, ""), true

	case MemoryHost:
		if s.Host == "" {
			return "", domain.ExecGrant{}, false
		}
		return s.Host, domain.ExecGrant{}, true

	case MemoryPath:
		if s.Dir == "" {
			return "", domain.ExecGrant{}, false
		}
		return s.Dir, domain.ExecGrant{}, true

	case MemoryTool:
		if s.ToolName == "" {
			return "", domain.ExecGrant{}, false
		}
		return s.ToolName, domain.ExecGrant{}, true

	default:
		return "", domain.ExecGrant{}, false
	}
}

// DeriveRememberGrant computes the grant a remembered rule should carry
// for the given call and user-chosen trust flavor. This is a re-export of
// the logic previously embedded in the app layer, moved here so the
// MemoryShape owns the full memory derivation.
//
//   - trust=unsandboxed on an escalated call → L2 full trust (explicit
//     user opt-in only; the ONLY rememberable flavor for escalations).
//   - otherwise → exactly the declared capabilities (network, gui_open);
//     zero grant when nothing was declared.
func DeriveRememberGrant(info RunCmdCall, trust string) domain.ExecGrant {
	if trust == TrustUnsandboxed && info.Escalated {
		return domain.ExecGrant{Unsandboxed: true}
	}
	return DeclaredGrant(info)
}
