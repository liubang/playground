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
// Created: 2026/07/30

package permission

import (
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func webFetchCall(t *testing.T, url string) domain.PreparedCall {
	t.Helper()
	raw, err := json.Marshal(map[string]string{"url": url})
	if err != nil {
		t.Fatal(err)
	}
	return domain.PreparedCall{
		Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_fetch", Arguments: raw},
		Risk: domain.R3,
	}
}

func TestNormalizeDomainHost(t *testing.T) {
	valid := map[string]string{
		"Weather.com.CN":  "weather.com.cn",
		" example.com.":   "example.com",
		"127.0.0.1":       "127.0.0.1",
		"localhost":       "localhost",
		"a-b.c_d.example": "a-b.c_d.example",
	}
	for in, want := range valid {
		got, err := normalizeDomainHost(in)
		if err != nil || got != want {
			t.Errorf("normalizeDomainHost(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	for _, bad := range []string{"", ".", "https://x.com", "x.com/", "x.com:8080", "user@x.com", ".x.com", "x..com", "x.com."} {
		if bad == "x.com." {
			continue // trailing dot is canonicalized away, valid
		}
		if _, err := normalizeDomainHost(bad); err == nil {
			t.Errorf("normalizeDomainHost(%q) must fail", bad)
		}
	}
}

func TestParseWebFetchHost(t *testing.T) {
	host, ok := ParseWebFetchHost([]byte(`{"url":"https://WWW.weather.com.cn/a?b=c"}`))
	if !ok || host != "www.weather.com.cn" {
		t.Fatalf("host = %q ok=%v", host, ok)
	}
	// Port is not part of the remembered identity.
	host, ok = ParseWebFetchHost([]byte(`{"url":"http://localhost:8080/x"}`))
	if !ok || host != "localhost" {
		t.Fatalf("host = %q ok=%v", host, ok)
	}
	for _, raw := range []string{`{"url":"ftp://x/y"}`, `{"url":""}`, `{}`, `nope`} {
		if _, ok := ParseWebFetchHost([]byte(raw)); ok {
			t.Errorf("ParseWebFetchHost(%s) must fail", raw)
		}
	}
}

func TestDomainRuleSetLayersAndStrictest(t *testing.T) {
	user := t.TempDir()
	project := t.TempDir()
	writeRulesFile(t, user, "u.json", `{"domains":[
		{"host":"www.weather.com.cn","decision":"allow","justification":"weather"},
		{"host":"api.evil.com","decision":"allow"}
	]}`)
	writeRulesFile(t, project, "p.json", `{"domains":[
		{"host":"api.evil.com","decision":"deny","justification":"known bad"},
		{"host":"project-loosen.com","decision":"allow"}
	]}`)
	set, errs := LoadRuleSets(user, project, LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	// User allow matches exactly (case-insensitive).
	if d, _ := set.EvaluateDomain("WWW.weather.com.cn"); d != domain.DecisionAllow {
		t.Fatalf("weather host = %v, want allow", d)
	}
	// Project deny (tightening) wins over user allow (strictest).
	if d, r := set.EvaluateDomain("api.evil.com"); d != domain.DecisionDeny || r.Justification != "known bad" {
		t.Fatalf("evil host = %v (%v), want deny from project layer", d, r)
	}
	// Project allow (loosening) is dropped by default.
	if d, _ := set.EvaluateDomain("project-loosen.com"); d != "" {
		t.Fatalf("project allow must be dropped, got %v", d)
	}
	// Subdomains are NOT covered by exact-host rules.
	if d, _ := set.EvaluateDomain("api.weather.com.cn"); d != "" {
		t.Fatalf("subdomain must not match exact-host rule, got %v", d)
	}
}

func TestDomainChainEvaluation(t *testing.T) {
	dir := t.TempDir()
	writeRulesFile(t, dir, "rules.json", `{"domains":[
		{"host":"www.weather.com.cn","decision":"allow"}
	]}`)
	set, errs := LoadRuleSets(dir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	policy := DefaultPolicy()
	policy.Rules = set
	policy.Session = NewSessionRules()
	d := policy.Decider(ModeOnRequest)

	// File rule allow.
	v := d.Evaluate(webFetchCall(t, "https://www.weather.com.cn/weather/1.shtml"))
	if v.Decision != domain.DecisionAllow || v.Source != SourceRule {
		t.Fatalf("ruled host = %s (%s), want allow from rule", v.Decision, v.Source)
	}
	// Session memory allow.
	if _, ok := policy.Session.RememberDomain("example.com"); !ok {
		t.Fatal("remember failed")
	}
	v = d.Evaluate(webFetchCall(t, "https://example.com/x"))
	if v.Decision != domain.DecisionAllow || v.Source != SourceSession {
		t.Fatalf("remembered host = %s (%s), want allow from session", v.Decision, v.Source)
	}
	// Unremembered host falls to the R3 baseline ask in every mode.
	v = d.Evaluate(webFetchCall(t, "https://unknown.example/"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("unknown host = %s, want ask (R3 baseline)", v.Decision)
	}
}


