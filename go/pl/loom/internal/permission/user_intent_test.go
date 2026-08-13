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
// Created: 2026/08/11

package permission

import (
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// userTextMessages builds a transcript of user-role text messages.
func userTextMessages(texts ...string) []domain.Message {
	msgs := make([]domain.Message, 0, len(texts))
	for _, text := range texts {
		msgs = append(msgs, domain.Message{
			ID:        domain.NewMessageID(),
			Role:      domain.RoleUser,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
			CreatedAt: time.Now(),
		})
	}
	return msgs
}

func TestExtractUserIntentHosts(t *testing.T) {
	msgs := userTextMessages(
		"看下 https://Example.COM:8080/path?q=1 这个页面",
		"还有 https://api.foo.com/x), 以及 https://api.foo.com/y", // dedup + trailing punct
		"参考https://cjk.example/docs。",                         // CJK full stop must not pollute the host
		"ftp://ignored.example and bare-host.example are not URLs",
	)
	// Assistant and tool-result content is model-influenced: it must never
	// seed the trust set.
	msgs = append(
		msgs,
		domain.Message{
			ID:    domain.NewMessageID(),
			Role:  domain.RoleAssistant,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "我去抓 https://self-auth.example/"}},
		},
		domain.Message{
			ID:   domain.NewMessageID(),
			Role: domain.RoleUser,
			Parts: []domain.ContentPart{{Kind: domain.PartToolResult, ToolResult: &domain.ToolResult{
				CallID: domain.NewToolCallID(),
				Content: []domain.ContentPart{
					{Kind: domain.PartText, Text: "page links to https://result-embed.example/"},
				},
			}}},
		},
	)
	hosts := ExtractUserIntentHosts(msgs)
	want := map[string]struct{}{"example.com": {}, "api.foo.com": {}, "cjk.example": {}}
	if len(hosts) != len(want) {
		t.Fatalf("hosts = %v, want %v", hosts, want)
	}
	for h := range want {
		if _, ok := hosts[h]; !ok {
			t.Errorf("hosts missing %q (got %v)", h, hosts)
		}
	}
	if got := ExtractUserIntentHosts(userTextMessages("没有链接")); got != nil {
		t.Errorf("no-URL transcript = %v, want nil", got)
	}
}

// userIntentChain builds a user-intent-enabled chain bound to the hosts
// mentioned in msgs — the same shape routeToolCalls produces via
// transcriptPolicy.WithTranscript.
func userIntentChain(t *testing.T, mode ApprovalMode, rules *RuleSet, msgs []domain.Message) Chain {
	t.Helper()
	policy := DefaultPolicy()
	policy.Rules = rules
	policy.Session = NewSessionRules()
	policy.UserIntent = true
	return policy.Decider(mode).WithUserIntent(ExtractUserIntentHosts(msgs))
}

func TestUserIntentAllowsMentionedHost(t *testing.T) {
	msgs := userTextMessages("帮我总结 https://docs.example.com/api 的内容")
	chain := userIntentChain(t, ModeOnRequest, nil, msgs)

	v := chain.Evaluate(webFetchCall(t, "https://docs.example.com/api"))
	if v.Decision != domain.DecisionAllow || v.Source != SourceUserIntent {
		t.Fatalf("mentioned host = %s (%s), want allow from user_intent", v.Decision, v.Source)
	}

	// A host the user never mentioned keeps the baseline behavior.
	v = chain.Evaluate(webFetchCall(t, "https://other.example/"))
	if v.Decision != domain.DecisionAsk || v.Source != SourceBaseline {
		t.Fatalf("unmentioned host = %s (%s), want ask from baseline", v.Decision, v.Source)
	}

	// Subdomains are NOT covered: user-content subdomains are arbitrary.
	v = chain.Evaluate(webFetchCall(t, "https://evil.docs.example.com/"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("subdomain of a mentioned host = %s (%s), want ask (exact-host match only)", v.Decision, v.Source)
	}

	// Exec calls get no user-intent opinion: a sandboxed command's
	// behavior is unchanged (baseline allow here, not user_intent).
	v = chain.Evaluate(runCmdCall(t, "git", "status"))
	if v.Decision != domain.DecisionAllow || v.Source != SourceBaseline {
		t.Fatalf("sandboxed run_cmd = %s (%s), want allow from baseline", v.Decision, v.Source)
	}
}

func TestUserIntentRequiresBinding(t *testing.T) {
	// The decider enters the chain with an empty snapshot; without a
	// WithUserIntent rebind it has no opinion.
	policy := DefaultPolicy()
	policy.UserIntent = true
	chain := policy.Decider(ModeOnRequest)
	v := chain.Evaluate(webFetchCall(t, "https://docs.example.com/api"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("unbound chain = %s (%s), want ask", v.Decision, v.Source)
	}
}

func TestUserIntentRuleDenyWins(t *testing.T) {
	dir := t.TempDir()
	writeRulesFile(t, dir, "rules.json", `{"domains":[
		{"host":"evil.example.com","decision":"deny","justification":"known bad"}
	]}`)
	set, errs := LoadRuleSets(dir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	msgs := userTextMessages("看看 https://evil.example.com/x")
	chain := userIntentChain(t, ModeOnRequest, set, msgs)

	v := chain.Evaluate(webFetchCall(t, "https://evil.example.com/x"))
	if v.Decision != domain.DecisionDeny || v.Source != SourceRule {
		t.Fatalf("denied host = %s (%s), want deny from rule even when user-mentioned", v.Decision, v.Source)
	}
}

func TestUserIntentExcludedInNeverMode(t *testing.T) {
	msgs := userTextMessages("https://docs.example.com/api")
	chain := userIntentChain(t, ModeNever, nil, msgs)

	v := chain.Evaluate(webFetchCall(t, "https://docs.example.com/api"))
	if v.Decision != domain.DecisionDeny || v.Source != SourceBaseline {
		t.Fatalf("never mode = %s (%s), want deny from baseline", v.Decision, v.Source)
	}
}

func TestUserIntentDisabledLeavesBaseline(t *testing.T) {
	policy := DefaultPolicy() // UserIntent defaults off
	chain := policy.Decider(ModeOnRequest)
	bound := chain.WithUserIntent(map[string]struct{}{"docs.example.com": {}})
	if len(bound) != len(chain) {
		t.Fatalf("WithUserIntent on a decider-less chain changed its length: %d -> %d", len(chain), len(bound))
	}
	v := bound.Evaluate(webFetchCall(t, "https://docs.example.com/api"))
	if v.Decision != domain.DecisionAsk {
		t.Fatalf("disabled user-intent = %s (%s), want ask", v.Decision, v.Source)
	}
}
