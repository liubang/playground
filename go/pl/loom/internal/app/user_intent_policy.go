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

// wirePolicy builds the agent-facing policy: the permission policy
// itself, wrapped in the transcript adapter when user-intent trust is
// enabled (approval.trust_user_urls).
func wirePolicy(policy permission.Policy) agent.Policy {
	if policy.UserIntent {
		return transcriptPolicy{policy: policy}
	}
	return policy
}

// transcriptPolicy adapts a permission policy to
// agent.TranscriptAwarePolicy: each tool-call routing pass rebinds the
// user-intent host snapshot from the live transcript, so a URL the user
// handed the agent is auto-allowed without weakening deny packages or
// the mode residual. The wrapped policy is shared and immutable during
// a pass; binding returns a cheap copy, so concurrent runs never
// observe each other's transcripts.
type transcriptPolicy struct {
	policy permission.Policy
}

// Evaluate implements agent.Policy.
func (p transcriptPolicy) Evaluate(call domain.PreparedCall) domain.Verdict {
	return p.policy.Evaluate(call)
}

// WithTranscript implements agent.TranscriptAwarePolicy.
func (p transcriptPolicy) WithTranscript(messages []domain.Message) agent.Policy {
	return transcriptPolicy{policy: p.policy.WithUserIntent(permission.ExtractUserIntentHosts(messages))}
}
