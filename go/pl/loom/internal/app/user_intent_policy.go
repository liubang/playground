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

package app

import (
	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// wirePolicy builds the agent-facing policy for a permission policy and
// approval mode: the bare decider chain, wrapped in the transcript
// adapter when user-intent trust is enabled (approval.trust_user_urls).
func wirePolicy(policy permission.Policy, mode permission.ApprovalMode) agent.Policy {
	chain := policy.Decider(mode)
	if policy.UserIntent {
		return transcriptPolicy{chain: chain}
	}
	return chain
}

// transcriptPolicy adapts a permission chain to agent.TranscriptAwarePolicy:
// each tool-call routing pass rebinds the chain's user-intent decider with
// the hosts the user mentioned in the live transcript, so a URL the user
// handed the agent is auto-allowed without weakening rule denies or the
// mode baseline. The wrapped chain is shared and immutable; binding
// returns a cheap copy, so concurrent runs never observe each other's
// transcripts.
type transcriptPolicy struct {
	chain permission.Chain
}

// Evaluate implements agent.Policy.
func (p transcriptPolicy) Evaluate(call domain.PreparedCall) domain.Verdict {
	return p.chain.Evaluate(call)
}

// WithTranscript implements agent.TranscriptAwarePolicy.
func (p transcriptPolicy) WithTranscript(messages []domain.Message) agent.Policy {
	return transcriptPolicy{chain: p.chain.WithUserIntent(permission.ExtractUserIntentHosts(messages))}
}
