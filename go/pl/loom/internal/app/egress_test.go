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
// Created: 2026/10/03

package app

import (
	"net/netip"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

const (
	workspaceA = "/ws/a"
	workspaceB = "/ws/b"
)

func hostPackage(host string, decision domain.Decision, scope permission.Scope, workspace, why string) permission.Package {
	return permission.Package{
		Bind:          permission.Binding{Kind: permission.BindHost, Host: host},
		Decision:      decision,
		Scope:         scope,
		Workspace:     workspace,
		Justification: why,
	}
}

// TestEgressPolicyWorkspaceIsolation: a package tagged with workspace A
// must not decide workspace B's connections — B falls through to the
// configured unmatched posture.
func TestEgressPolicyWorkspaceIsolation(t *testing.T) {
	set := permission.NewPackageSet()
	set.Add(hostPackage("api.example.com", domain.DecisionAllow, permission.ScopeProject, workspaceA, "project rule of A"))

	forWorkspaceA := egressPolicy{packages: set, workspace: workspaceA}
	if d := forWorkspaceA.Decide("api.example.com", 443); !d.Allow || !d.Matched || d.Rule != "project" {
		t.Fatalf("workspace A decision = %+v, want allowed project hit", d)
	}

	// Workspace B sees no rule: unmatched-allow posture applies, and the
	// decision carries no matched flag.
	forWorkspaceB := egressPolicy{packages: set, workspace: workspaceB, unmatchedAllow: true}
	if d := forWorkspaceB.Decide("api.example.com", 443); !d.Allow || d.Matched || d.Rule != "unmatched-allow" {
		t.Fatalf("workspace B decision = %+v, want unmatched-allow", d)
	}

	// Same for the deny posture: B must NOT inherit A's allow.
	forWorkspaceBDeny := egressPolicy{packages: set, workspace: workspaceB}
	if d := forWorkspaceBDeny.Decide("api.example.com", 443); d.Allow || d.Matched || d.Rule != "unmatched-deny" {
		t.Fatalf("workspace B decision = %+v, want unmatched-deny", d)
	}
}

// TestEgressPolicyGlobalOnly: the SOCKS5 leg evaluates global-scope
// rules only — an unauthenticated client must not ride another
// workspace's project/session approvals.
func TestEgressPolicyGlobalOnly(t *testing.T) {
	set := permission.NewPackageSet()
	set.Add(hostPackage("global.example.com", domain.DecisionAllow, permission.ScopeUser, "", "user rule"))
	set.Add(hostPackage("project.example.com", domain.DecisionAllow, permission.ScopeProject, workspaceA, "project rule of A"))
	set.Add(hostPackage("session.example.com", domain.DecisionAllow, permission.ScopeSession, workspaceA, "remembered in A"))

	socks := egressPolicy{packages: set, workspace: workspaceA, globalOnly: true}

	if d := socks.Decide("global.example.com", 443); !d.Allow || !d.Matched {
		t.Fatalf("global rule decision = %+v, want allowed", d)
	}
	for _, host := range []string{"project.example.com", "session.example.com"} {
		if d := socks.Decide(host, 443); d.Allow || d.Matched || d.Rule != "unmatched-deny" {
			t.Fatalf("socks decision for %s = %+v, want unmatched-deny", host, d)
		}
	}
}

// TestEgressPolicyDenyWins: a deny outranks an allow for the same host
// regardless of package order or workspace tagging.
func TestEgressPolicyDenyWins(t *testing.T) {
	set := permission.NewPackageSet()
	// Allow first, deny second: order must not matter.
	set.Add(hostPackage("webhook.site", domain.DecisionAllow, permission.ScopeUser, "", "user allow"))
	set.Add(hostPackage("webhook.site", domain.DecisionDeny, permission.ScopeBuiltin, "", "request capture service"))

	p := egressPolicy{packages: set, workspace: workspaceA}
	d := p.Decide("webhook.site", 443)
	if d.Allow || !d.Matched || d.Reason != "request capture service" || d.Rule != "builtin" {
		t.Fatalf("decision = %+v, want builtin deny with reason", d)
	}
}

// TestEgressPolicyWildcard: "*.example.com" matches subdomains only,
// never the apex — both through the proxy policy path.
func TestEgressPolicyWildcard(t *testing.T) {
	set := permission.NewPackageSet()
	set.Add(hostPackage("*.example.com", domain.DecisionAllow, permission.ScopeUser, "", "wildcard allow"))

	p := egressPolicy{packages: set, workspace: workspaceA}
	if d := p.Decide("api.example.com", 443); !d.Allow || !d.Matched {
		t.Fatalf("subdomain decision = %+v, want allowed", d)
	}
	if d := p.Decide("example.com", 443); d.Matched {
		t.Fatalf("apex decision = %+v, want unmatched (wildcard does not cover apex)", d)
	}
}

// TestEgressPolicyExemptLiterals: an allow rule bound to an IP literal
// contributes the literal to the guard's exemption snapshot. Collection
// is host-scoped: the exemption applies to connections targeting the
// literal itself — a domain resolving to a denied-class address is
// never exempted. A deny on the same host must yield NO exemption.
func TestEgressPolicyExemptLiterals(t *testing.T) {
	set := permission.NewPackageSet()
	set.Add(hostPackage("127.0.0.1", domain.DecisionAllow, permission.ScopeUser, "", "local dev upstream"))

	p := egressPolicy{packages: set, workspace: workspaceA}
	d := p.Decide("127.0.0.1", 8080)
	if !d.Allow || !d.Matched {
		t.Fatalf("decision = %+v, want allowed", d)
	}
	if len(d.ExemptLiterals) != 1 || d.ExemptLiterals[0] != netip.MustParseAddr("127.0.0.1") {
		t.Fatalf("exempt literals = %v, want [127.0.0.1]", d.ExemptLiterals)
	}

	// Denied hosts never leak an exemption.
	set.Add(hostPackage("10.0.0.8", domain.DecisionDeny, permission.ScopeBuiltin, "", "internal range"))
	if d := p.Decide("10.0.0.8", 80); d.Allow || len(d.ExemptLiterals) != 0 {
		t.Fatalf("denied literal decision = %+v, want deny with no exemption", d)
	}
}

// TestEgressPolicyUnmatchedPostures: the posture switch is the only
// decider when no rule fires, on both legs.
func TestEgressPolicyUnmatchedPostures(t *testing.T) {
	set := permission.NewPackageSet()

	allow := egressPolicy{packages: set, workspace: workspaceA, unmatchedAllow: true}
	if d := allow.Decide("anything.example.org", 443); !d.Allow || d.Matched || d.Rule != "unmatched-allow" {
		t.Fatalf("unmatched-allow decision = %+v", d)
	}

	deny := egressPolicy{packages: set, workspace: workspaceA}
	if d := deny.Decide("anything.example.org", 443); d.Allow || d.Matched || d.Rule != "unmatched-deny" {
		t.Fatalf("unmatched-deny decision = %+v", d)
	}

	// globalOnly ignores the unmatched-allow posture entirely: an
	// unauthenticated SOCKS client gets no ambient network.
	socksAllow := egressPolicy{packages: set, workspace: workspaceA, unmatchedAllow: true, globalOnly: true}
	if d := socksAllow.Decide("anything.example.org", 443); d.Allow || d.Rule != "unmatched-deny" {
		t.Fatalf("socks unmatched decision = %+v, want unmatched-deny", d)
	}
}

// TestEgressPolicyNilPackageSet: bootstrap defends with a nil set (no
// rules loaded); every destination falls to the posture.
func TestEgressPolicyNilPackageSet(t *testing.T) {
	deny := egressPolicy{packages: nil, workspace: workspaceA}
	if d := deny.Decide("example.com", 443); d.Allow || d.Rule != "unmatched-deny" {
		t.Fatalf("nil-set decision = %+v, want unmatched-deny", d)
	}
}
