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
// Created: 2026/08/18

package permission

import (
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// --- ExtractNetworkHosts unit tests ---

func TestExtractNetworkHostsFromURL(t *testing.T) {
	info := RunCmdCall{Argv: []string{"curl", "https://api.example.com/v1/data"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 1 || hosts[0] != "api.example.com" {
		t.Fatalf("got %v, want [api.example.com]", hosts)
	}
}

func TestExtractNetworkHostsFromBareHost(t *testing.T) {
	info := RunCmdCall{Argv: []string{"ping", "example.com"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 1 || hosts[0] != "example.com" {
		t.Fatalf("got %v, want [example.com]", hosts)
	}
}

func TestExtractNetworkHostsFromHostWithPort(t *testing.T) {
	info := RunCmdCall{Argv: []string{"nc", "example.com:443"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 1 || hosts[0] != "example.com" {
		t.Fatalf("got %v, want [example.com]", hosts)
	}
}

func TestExtractNetworkHostsDeduplicates(t *testing.T) {
	info := RunCmdCall{Argv: []string{"curl", "https://api.example.com", "http://api.example.com/other"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 1 {
		t.Fatalf("got %v, want deduplicated single host", hosts)
	}
}

func TestExtractNetworkHostsIgnoresNonHostTokens(t *testing.T) {
	info := RunCmdCall{Argv: []string{"go", "mod", "download"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 0 {
		t.Fatalf("got %v, want empty (no network hosts)", hosts)
	}
}

func TestExtractNetworkHostsFromShellScript(t *testing.T) {
	// Shell analysis is needed for composed scripts. For unit testing,
	// simulate via ShellCommandArgvs (plain argv path).
	info := RunCmdCall{Argv: []string{"curl", "https://api.example.com", "&&", "wget", "http://other.test.net"}}
	hosts := ExtractNetworkHosts(info)
	if len(hosts) != 2 {
		t.Fatalf("got %v, want 2 hosts", hosts)
	}
	want := map[string]bool{"api.example.com": false, "other.test.net": false}
	for _, h := range hosts {
		want[h] = true
	}
	for h, found := range want {
		if !found {
			t.Errorf("missing host %s", h)
		}
	}
}

func TestExtractNetworkHostsIgnoresNonHTTPSchemes(t *testing.T) {
	info := RunCmdCall{Argv: []string{"git", "clone", "git+ssh://git@github.com/repo.git"}}
	hosts := ExtractNetworkHosts(info)
	// git+ssh is not http/https, so no host extracted from URL path.
	// "github.com" in "git+ssh://git@github.com/repo.git" is not a bare token
	// either, so result should be empty.
	if len(hosts) != 0 {
		t.Fatalf("got %v, want empty (non-http scheme ignored)", hosts)
	}
}

// --- evaluateNetworkHosts integration tests ---

func TestEvaluateNetworkHostsDenyBeatsArgvAllow(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionAllow),
			Grant:      &RuleGrant{Network: "full"},
		}},
		domains: []DomainRule{{
			Host:     "evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	d := RuleDecider{Rules: rules}
	info := RunCmdCall{
		Argv:         []string{"curl", "https://evil.example.com"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionDeny {
		t.Fatalf("got %+v, want deny (domain rule beats argv allow)", v)
	}
}

func TestEvaluateNetworkHostsAskBeatsArgvAllow(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionAllow),
			Grant:      &RuleGrant{Network: "full"},
		}},
		domains: []DomainRule{{
			Host:     "ask.example.com",
			Decision: string(domain.DecisionAsk),
		}},
	})
	d := RuleDecider{Rules: rules}
	info := RunCmdCall{
		Argv:         []string{"curl", "https://ask.example.com"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionAsk {
		t.Fatalf("got %+v, want ask (domain ask beats argv allow)", v)
	}
}

func TestEvaluateNetworkHostsAllowDoesNotOverrideArgvDeny(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionDeny),
		}},
		domains: []DomainRule{{
			Host:     "ok.example.com",
			Decision: string(domain.DecisionAllow),
		}},
	})
	d := RuleDecider{Rules: rules}
	info := RunCmdCall{
		Argv:         []string{"curl", "https://ok.example.com"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionDeny {
		t.Fatalf("got %+v, want deny (argv deny beats domain allow)", v)
	}
}

func TestEvaluateNetworkHostsNoNetworkNoFiltering(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionAllow),
		}},
		domains: []DomainRule{{
			Host:     "evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	d := RuleDecider{Rules: rules}
	// NeedsNetwork is false: domain rules must NOT apply.
	info := RunCmdCall{
		Argv: []string{"curl", "https://evil.example.com"},
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionAllow {
		t.Fatalf("got %+v, want allow (no needs_network → no domain filter)", v)
	}
}

func TestEvaluateNetworkHostsWildcardDomain(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionAllow),
			Grant:      &RuleGrant{Network: "full"},
		}},
		domains: []DomainRule{{
			Host:     "*.evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	d := RuleDecider{Rules: rules}
	info := RunCmdCall{
		Argv:         []string{"curl", "https://subdomain.evil.example.com"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionDeny {
		t.Fatalf("got %+v, want deny (wildcard domain rule matches)", v)
	}
}

func TestEvaluateNetworkHostsNoHostsExtracted(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"go"},
			Decision:   string(domain.DecisionAllow),
			Grant:      &RuleGrant{Network: "full"},
		}},
		domains: []DomainRule{{
			Host:     "evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	d := RuleDecider{Rules: rules}
	// No network host in argv; domain rules should not trigger.
	info := RunCmdCall{
		Argv:         []string{"go", "mod", "download"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionAllow {
		t.Fatalf("got %+v, want allow (no hosts extracted, domain rules silent)", v)
	}
}

func TestEvaluateNetworkHostsDenyWhenNoArgvRuleMatches(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		domains: []DomainRule{{
			Host:     "evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	d := RuleDecider{Rules: rules}
	// No argv rule matches, but domain deny should still block.
	info := RunCmdCall{
		Argv:         []string{"curl", "https://evil.example.com"},
		NeedsNetwork: true,
	}
	v := d.evaluateRunCmd(info)
	if v == nil || v.Decision != domain.DecisionDeny {
		t.Fatalf("got %+v, want deny (domain deny applies even without argv rule)", v)
	}
}

// --- full chain integration tests ---

func TestChainDomainDenyBeatsArgvAllowAndBaseline(t *testing.T) {
	rules := &RuleSet{}
	rules.merge(&RuleSet{
		rules: []Rule{{
			ArgvPrefix: []string{"curl"},
			Decision:   string(domain.DecisionAllow),
			Grant:      &RuleGrant{Network: "full"},
		}},
		domains: []DomainRule{{
			Host:     "evil.example.com",
			Decision: string(domain.DecisionDeny),
		}},
	})
	p := Policy{Rules: rules}
	d := p.Decider(ModeOnRequest)
	call := domain.PreparedCall{
		Call: domain.ToolCall{
			Name:      "run_cmd",
			Arguments: []byte(`{"program":"curl","args":["https://evil.example.com"],"needs_network":true}`),
		},
		Risk: domain.R2,
		ExecRequest: &domain.ExecRequest{
			Argv:         []string{"curl", "https://evil.example.com"},
			NeedsNetwork: true,
		},
	}
	v := d.Evaluate(call)
	if v.Decision != domain.DecisionDeny {
		t.Fatalf("got %s, want deny (domain deny beats argv allow + baseline)", v.Decision)
	}
	if v.Source != SourceRule {
		t.Errorf("source = %q, want %q", v.Source, SourceRule)
	}
}
