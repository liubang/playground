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
// Created: 2026/07/27

package domain

// ExecGrant describes the execution capabilities a policy verdict grants to
// one tool call (docs/PERMISSION_DESIGN.md §3.2). It is the single contract
// between the policy layer and the execution layer: the zero value is the
// default sandbox (loopback-only network, workspace+tmp writes), and grants
// only ever widen it, never narrow it.
type ExecGrant struct {
	// Unsandboxed runs the call outside the sandbox entirely (L2 trust):
	// full user environment, network, and credentials. The remaining
	// fields are meaningless when this is set.
	Unsandboxed bool `json:"unsandboxed,omitempty"`
	// NetworkFull allows outbound network and DNS inside the sandbox (L1).
	NetworkFull bool `json:"network_full,omitempty"`
	// WritablePaths are additional absolute paths writable inside the
	// sandbox (L1). Protected subpaths (.git/hooks, .loom) stay excluded.
	WritablePaths []string `json:"writable_paths,omitempty"`
	// GUIOpen lets sandboxed commands drive macOS GUI applications via
	// `open` (LaunchServices + Apple Events; docs/BROWSER_DESIGN.md §4).
	// Unlike network, this asks in EVERY approval mode: Apple Events are
	// TCC-attributed to the loom process itself, so loom-level approval is
	// the only per-call gate.
	GUIOpen bool `json:"gui_open,omitempty"`
}

// IsZero reports whether the grant is the default sandbox (no widenings).
func (g ExecGrant) IsZero() bool {
	return !g.Unsandboxed && !g.NetworkFull && len(g.WritablePaths) == 0 && !g.GUIOpen
}

// Summary renders the grant for approval prompts and audit logs, e.g.
// "+网络, +写 ~/.myapp" or "出沙箱". Empty for the zero grant.
func (g ExecGrant) Summary() string {
	if g.Unsandboxed {
		return "出沙箱（完整权限）"
	}
	out := ""
	if g.NetworkFull {
		out += "+网络"
	}
	if g.GUIOpen {
		if out != "" {
			out += ", "
		}
		out += "+GUI 打开"
	}
	for _, p := range g.WritablePaths {
		if out != "" {
			out += ", "
		}
		out += "+写 " + p
	}
	return out
}

// Verdict is the policy layer's judgment on a prepared call; a resolved
// verdict always carries a valid Decision.
type Verdict struct {
	Decision Decision
	// Grant is the execution capability contract the verdict carries: on
	// allow it is the covering package's grant (or the sandbox baseline);
	// on ask it is the gap grant the call declared, so an approved call
	// runs with exactly the power the user was shown. It rides the
	// prepared call into execution either way.
	Grant ExecGrant
	// Source identifies the deciding factor for audits and the UI:
	// "rule" (a declarative deny/ask/exact package), "session" (a
	// session-remembered package), "baseline" (the default sandbox or
	// the approval-mode residual), "user_intent", or "indicator".
	Source string
	// Reason is the human-readable provenance (rule justification,
	// indicator description, baseline mode).
	Reason string
}
