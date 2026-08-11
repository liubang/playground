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

// User-intent trust: when the user hands the agent a URL in the
// conversation, fetching that host IS the requested work, so an approval
// prompt is pure friction. The UserIntentDecider auto-allows URL calls
// (web_fetch, browser navigate — anything carrying a signed URLRequest)
// whose host the user mentioned, while rule-layer denies keep blocking
// even user-mentioned hosts.
package permission

import (
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// UserIntentDecider auto-allows URL calls whose host the user mentioned
// in the conversation transcript. Chain placement — after Rule, after
// Danger, before Session — gives the intended precedence:
//
//   - A rule verdict always wins: an explicit deny keeps blocking a host
//     the user mentioned, and an explicit allow/ask needs no shortcut.
//   - Session memory and the mode baseline only see calls targeting hosts
//     the user never mentioned.
//
// The decider carries an immutable host snapshot, rebound from the live
// transcript once per tool-call routing pass via Chain.WithUserIntent —
// there is no shared mutable store, so concurrent runs sharing one chain
// never observe each other's transcripts.
//
// Matching is EXACT host only: subdomains are deliberately not covered —
// "the user mentioned github.com" must not bless evil.github.com, whose
// content is arbitrary user data. Policy.Decider excludes the decider in
// never mode: unattended runs keep the strict "deny R3+" contract because
// their prompt may come from an untrusted source.
type UserIntentDecider struct {
	// Hosts are the canonical hosts extracted from user messages by
	// ExtractUserIntentHosts (nil = no opinion on anything).
	Hosts map[string]struct{}
}

var _ contextDecider = UserIntentDecider{}

// Evaluate implements Decider (the standalone, non-fast path).
func (d UserIntentDecider) Evaluate(call domain.PreparedCall) *domain.Verdict {
	info, ok := URLInfoOf(call)
	return d.evaluate(call, evalContext{url: info, urlOK: ok})
}

// evaluate implements the contextDecider fast path (REVIEW M33). Exec
// calls get no opinion: a sandboxed command's network needs are governed
// by the domain rules (evaluateNetworkHosts), a different contract.
func (d UserIntentDecider) evaluate(_ domain.PreparedCall, ctx evalContext) *domain.Verdict {
	if !ctx.urlOK {
		return nil
	}
	if _, ok := d.Hosts[ctx.url.Host]; !ok {
		return nil
	}
	return &domain.Verdict{
		Decision: domain.DecisionAllow,
		Source:   SourceUserIntent,
		Reason:   "the user mentioned this host in the conversation",
	}
}

// WithUserIntent returns a copy of the chain with the user-intent decider
// rebound to the given host snapshot; chains without a user-intent
// decider are returned unchanged. The copy is cheap (deciders are small
// value structs) and leaves the shared chain immutable, so concurrent
// runs never observe each other's transcripts.
func (c Chain) WithUserIntent(hosts map[string]struct{}) Chain {
	for i, d := range c {
		if _, ok := d.(UserIntentDecider); ok {
			out := make(Chain, len(c))
			copy(out, c)
			out[i] = UserIntentDecider{Hosts: hosts}
			return out
		}
	}
	return c
}
