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
// Created: 2026/08/23

// Effect derivation is the foundation of the permission model: instead of
// classifying command TEXT against a blacklist, every prepared call is
// translated into an Effect — the capabilities it needs, the consequence
// class of what it does, and whether that translation is provable. The
// policy decision is then a machine-checkable inclusion test (Effect ⊆
// granted capability set), and "we cannot prove this" is an explicit,
// first-class outcome rather than a silent pass.
package permission

import (
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// HostSet is a network-egress requirement. The zero value means "no
// network needed". Any marks an underivable requirement (the command may
// reach arbitrary hosts); Hosts lists canonical hostnames (lowercase, no
// port) when the targets are statically provable. Any and Hosts are
// mutually exclusive — a requirement is either fully enumerated or it is
// not, never both.
type HostSet struct {
	Any   bool     `json:"any,omitempty"`
	Hosts []string `json:"hosts,omitempty"`
}

// IsZero reports whether no network is required.
func (h HostSet) IsZero() bool { return !h.Any && len(h.Hosts) == 0 }

// PathSet is a write-path requirement with the same zero/Any/enumerated
// contract as HostSet. Paths are canonical absolute paths.
type PathSet struct {
	Any   bool     `json:"any,omitempty"`
	Paths []string `json:"paths,omitempty"`
}

// IsZero reports whether no writes outside the default sandbox are
// required. Writes INSIDE the sandbox boundary (workspace, scratch,
// toolchain caches) are not requirements at all — the default sandbox
// already covers them — so they never appear here.
func (p PathSet) IsZero() bool { return !p.Any && len(p.Paths) == 0 }

// Consequence classifies what an operation does to the world, ordered by
// severity. Unlike the old blacklist's text patterns, the class is
// derived from command SEMANTICS (subcommand + flags + target shape), so
// textual obfuscation cannot lower it.
type Consequence int

const (
	// ConsequenceConfined is fully contained by the default sandbox:
	// workspace/scratch-confined, recoverable (build artifacts, reads,
	// ordinary writes). Runs silently in every approval mode.
	ConsequenceConfined Consequence = iota
	// ConsequenceLocalDestructive destroys local data irreversibly
	// (rm of critical targets, git reset --hard, git clean -f, dd,
	// privilege-escalating programs).
	ConsequenceLocalDestructive
	// ConsequenceSharedState mutates state others observe (git push,
	// package publish, remote API writes) — recoverable only through
	// another shared-state operation, if at all.
	ConsequenceSharedState
	// ConsequenceSharedDestructive rewrites or destroys shared state
	// (git push --force/--delete, force-deleting remote branches).
	ConsequenceSharedDestructive
)

// String renders the class for rules, audits, and the approval UI.
func (c Consequence) String() string {
	switch c {
	case ConsequenceConfined:
		return "confined"
	case ConsequenceLocalDestructive:
		return "local-destructive"
	case ConsequenceSharedState:
		return "shared-state"
	case ConsequenceSharedDestructive:
		return "shared-destructive"
	}
	return fmt.Sprintf("consequence(%d)", int(c))
}

// ParseConsequence parses a rule-file consequence value; empty selects
// the default (confined).
func ParseConsequence(s string) (Consequence, error) {
	switch s {
	case "", "confined":
		return ConsequenceConfined, nil
	case "local-destructive":
		return ConsequenceLocalDestructive, nil
	case "shared-state":
		return ConsequenceSharedState, nil
	case "shared-destructive":
		return ConsequenceSharedDestructive, nil
	}
	return 0, fmt.Errorf("consequence must be confined|local-destructive|shared-state|shared-destructive, got %q", s)
}

// Effect is the derived consequence of one prepared call: the execution
// capabilities it requires, the consequence class of what it does, and
// the provenance of that judgment. Effects are produced by DeriveEffect
// (derive.go) and consumed by the inclusion check (capability.go) — the
// ONLY two places that reason about what a call means.
type Effect struct {
	// Network is the egress requirement (zero = none).
	Network HostSet
	// NamedHosts are the hosts the command NAMES explicitly (curl's URL
	// arguments), independent of the coverage requirement: a proxied
	// curl needs Network=Any for coverage, but a deny rule for the
	// named host must still bite. Allow-side host coverage never
	// consults NamedHosts — only deny matching does.
	NamedHosts []string
	// Writes lists write targets outside the default sandbox boundary.
	Writes PathSet
	// GUIOpen requires driving macOS GUI applications (open / Apple
	// Events) — asks in every approval mode (TCC attribution).
	GUIOpen bool
	// Unsandboxed requires execution outside the sandbox entirely (the
	// model's require_escalated). Only an unsandboxed grant covers it.
	Unsandboxed bool
	// Consequence is the severity ceiling of the operation.
	Consequence Consequence
	// Proven reports that the effect is a COMPLETE account of what the
	// call can do: the inclusion check may decide it. When false, Reason
	// explains why the call defies proof (dynamic shell, unanalyzable
	// interpreter input, third-party tool, ...), and the capability
	// fields are a best-effort LOWER bound for grant purposes only.
	Proven bool
	// Reason is the human-readable provenance: the unprovable cause, or
	// a short semantic summary when proven.
	Reason string
	// Indicators are heuristic danger signals attached to the effect
	// (the demoted blacklist: pipe-into-interpreter, credential-exfil
	// shape, sensitive redirect targets, indirect executors). An effect
	// carrying indicators is never covered silently by categorical
	// packages — only by an exact-binding approval of the same shape.
	Indicators []string
}

// ZeroEffect is the fully-confined effect: no capabilities, confined
// consequence, proven. It is what workspace-internal operations derive to.
var ZeroEffect = Effect{Proven: true, Reason: "confined to the default sandbox"}

// CrossesBoundary reports whether the effect needs anything beyond the
// default sandbox (network, extra writes, GUI, or no sandbox at all).
func (e Effect) CrossesBoundary() bool {
	return e.Unsandboxed || e.GUIOpen || !e.Network.IsZero() || !e.Writes.IsZero()
}

// GapGrant computes the execution grant that answers this effect's
// capability needs — what an approval must carry so the approved call
// runs with exactly the power it declared (AllowGrantCovers' successor).
// Domain-granular network approvals still execute with a full in-sandbox
// network grant: the sandbox's network switch is all-or-nothing; host
// granularity lives in the policy layer (capability packages).
func (e Effect) GapGrant() domain.ExecGrant {
	if e.Unsandboxed {
		return domain.ExecGrant{Unsandboxed: true}
	}
	g := domain.ExecGrant{
		NetworkFull:   !e.Network.IsZero(),
		GUIOpen:       e.GUIOpen,
		WritablePaths: append([]string(nil), e.Writes.Paths...),
	}
	return g
}

// Describe renders the consequence-oriented approval summary: what the
// operation DOES, not what its text looks like.
func (e Effect) Describe() string {
	var parts []string
	switch e.Consequence {
	case ConsequenceLocalDestructive:
		parts = append(parts, "将不可恢复地破坏本地数据")
	case ConsequenceSharedState:
		parts = append(parts, "将变更他人可见的共享状态")
	case ConsequenceSharedDestructive:
		parts = append(parts, "将覆写或删除共享状态（他人可见，不可恢复）")
	}
	if e.Unsandboxed {
		parts = append(parts, "将在沙箱外以完整用户权限执行")
	} else {
		if e.Network.Any {
			parts = append(parts, "需要访问外网（目标不可静态证明）")
		} else if len(e.Network.Hosts) > 0 {
			parts = append(parts, "需要访问 "+strings.Join(e.Network.Hosts, ", "))
		}
		if e.Writes.Any {
			parts = append(parts, "需要写入工作区外的路径（目标不可静态证明）")
		} else if len(e.Writes.Paths) > 0 {
			parts = append(parts, "需要写入 "+strings.Join(e.Writes.Paths, ", "))
		}
		if e.GUIOpen {
			parts = append(parts, "需要驱动 GUI 应用（macOS open / Apple Events）")
		}
	}
	if !e.Proven {
		parts = append(parts, "行为不可静态证明（"+e.Reason+"）")
	}
	if len(parts) == 0 {
		return "操作完全限制在默认沙箱边界内"
	}
	return strings.Join(parts, "；")
}

// joinEffects merges the effects of a composed invocation (every step of
// a shell script): capabilities union, consequence takes the max, proof
// requires ALL steps proven, indicators union. The zero value is a valid
// identity, so a single-step plan joins trivially.
func joinEffects(steps []Effect) Effect {
	var out Effect
	out.Proven = true
	var reasons []string
	seenIndicator := map[string]struct{}{}
	for _, s := range steps {
		if s.Network.Any {
			out.Network = HostSet{Any: true}
		} else if !out.Network.Any {
			out.Network.Hosts = unionStrings(out.Network.Hosts, s.Network.Hosts)
		}
		out.NamedHosts = unionStrings(out.NamedHosts, s.NamedHosts)
		if s.Writes.Any {
			out.Writes = PathSet{Any: true}
		} else if !out.Writes.Any {
			out.Writes.Paths = unionStrings(out.Writes.Paths, s.Writes.Paths)
		}
		out.GUIOpen = out.GUIOpen || s.GUIOpen
		out.Unsandboxed = out.Unsandboxed || s.Unsandboxed
		if s.Consequence > out.Consequence {
			out.Consequence = s.Consequence
		}
		if !s.Proven {
			out.Proven = false
			if s.Reason != "" {
				reasons = append(reasons, s.Reason)
			}
		}
		for _, ind := range s.Indicators {
			if _, dup := seenIndicator[ind]; !dup {
				seenIndicator[ind] = struct{}{}
				out.Indicators = append(out.Indicators, ind)
			}
		}
	}
	if !out.Proven {
		out.Reason = strings.Join(reasons, "; ")
	}
	return out
}

// unionStrings appends items of b missing from a, preserving order.
func unionStrings(a, b []string) []string {
	for _, s := range b {
		found := false
		for _, existing := range a {
			if existing == s {
				found = true
				break
			}
		}
		if !found {
			a = append(a, s)
		}
	}
	return a
}
