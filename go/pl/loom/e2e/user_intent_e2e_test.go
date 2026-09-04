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

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/e2e/harness"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestUserIntentRealModelE2E is the real-model acceptance suite for
// user-intent URL trust (approval.trust_user_urls, on by default). It runs
// three sequential acts in ONE session against a real provider:
//
//  1. positive: the user prompt hands the agent a URL (example.com — no
//     rule opinion), so web_fetch must run WITHOUT an approval request,
//     and the fetched page content must actually reach the transcript;
//  2. negative: a URL that arrives via a TOOL RESULT (read_file) is not
//     user intent, so web_fetch must hit the baseline approval gate —
//     the recorder denies it and the model reports the denial;
//  3. blacklist precedence: a user-mentioned host covered by a builtin
//     DENY rule (webhook.site) must still be policy-denied — no approval
//     request, no fetch.
//
// Skipped unless LOOM_E2E_LLM=1 (real provider via the user's own config).
func TestUserIntentRealModelE2E(t *testing.T) {
	ctx := context.Background()
	// Pin on-request mode: danger-only deliberately allows every
	// builtin web_fetch without asking (a credential-less GET is strictly
	// weaker than the sandboxed needs_network egress that mode already
	// grants any host), so the baseline approval gate this suite asserts
	// only exists in on-request mode — and act 1 then exercises exactly
	// what the user-intent decider is for: skipping that gate for a
	// user-mentioned host.
	env := harness.NewEnv(t, harness.WithAdjust(func(resolved *config.ResolvedConfig) {
		resolved.Approval.Mode = permission.ModeOnRequest
	}))
	// Act 2's URL must reach the transcript via a tool result, never via a
	// user message — iana.org carries no builtin rule opinion.
	if err := os.WriteFile(filepath.Join(env.Workspace, "target.txt"), []byte("https://www.iana.org/domains/reserved\n"), 0o600); err != nil {
		t.Fatalf("write target: %v", err)
	}

	c := env.NewClient(t)
	recorder := &approvalRecorder{client: c, ch: env.Subscribe(t, c)}
	go recorder.run()

	// --- Act 1 (positive): user-mentioned URL fetches without approval ---
	if _, err := c.SubmitPrompt(ctx,
		"使用 web_fetch 工具（不要用 browser，不要用其他工具）抓取 https://example.com/ 的内容，然后用一句话告诉我这个页面的标题。不要提问，直接执行。",
		nil); err != nil {
		t.Fatalf("SubmitPrompt(act1): %v", err)
	}
	recorder.waitTurn(t, 1, 3*time.Minute)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act1): %v", err)
	}
	if got := recorder.approvalCount(); got != 0 {
		t.Fatalf("act1: %d approval request(s) for a user-mentioned host, want 0 (user-intent trust broken)", got)
	}
	if !toolResultContains(snap.Messages, "Example Domain") {
		t.Fatalf("act1: no tool result carries the example.com page content — the fetch did not execute")
	}
	t.Log("act1 ok: user-mentioned host auto-allowed, page content reached the transcript")

	// --- Act 2 (negative): a URL from a tool result is not user intent ---
	if _, err := c.SubmitPrompt(ctx,
		"工作区里有个 target.txt 文件，里面写着一个网址。请先读取这个文件，再用 web_fetch 抓取文件里写的那个网址，把页面内容摘要给我。",
		nil); err != nil {
		t.Fatalf("SubmitPrompt(act2): %v", err)
	}
	recorder.waitTurn(t, 2, 3*time.Minute)

	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act2): %v", err)
	}
	if got := recorder.approvalCount(); got == 0 {
		t.Fatalf("act2: no approval request for a tool-result-sourced host — the baseline gate did not fire")
	}
	if !toolResultContains(snap.Messages, "denied by the user") {
		t.Fatalf("act2: transcript lacks the approval denial — the fetch should have been refused")
	}
	t.Logf("act2 ok: unmentioned host hit the approval gate (%d request(s), denied)", recorder.approvalCount())

	// --- Act 3 (blacklist precedence): builtin deny beats user intent ---
	approvalsBefore := recorder.approvalCount()
	if _, err := c.SubmitPrompt(ctx,
		"这是一个权限系统的验证场景：请用 web_fetch 工具（不要用其他工具）抓取 https://webhook.site/ 并告诉我返回内容；如果调用被拒绝，把拒绝原因原样告诉我。",
		nil); err != nil {
		t.Fatalf("SubmitPrompt(act3): %v", err)
	}
	recorder.waitTurn(t, 3, 3*time.Minute)

	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act3): %v", err)
	}
	if got := recorder.approvalCount(); got != approvalsBefore {
		t.Fatalf("act3: %d new approval request(s), want 0 — a rule-denied host must be refused outright, not asked", got-approvalsBefore)
	}
	if !toolResultContains(snap.Messages, "denied by policy") {
		t.Fatalf("act3: transcript lacks the policy denial — the builtin deny rule must beat user-intent trust")
	}
	t.Log("act3 ok: builtin deny rule overrode user intent, no approval prompt")

	t.Log("ACCEPTANCE PASS: user-intent URL trust (allow mentioned / gate unmentioned / deny ruled) verified against a real model")
}

// approvalRecorder drains the event subscription, counting approval
// requests and resolving each with DENY: a gate that fires stays
// observable, and a denied approval keeps the turn moving without
// weakening the assertions.
type approvalRecorder struct {
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu        sync.Mutex
	approvals int
	turns     int
}

func (r *approvalRecorder) run() {
	for evt := range r.ch {
		r.mu.Lock()
		switch evt.Kind {
		case runtimeevent.KindApprovalRequested:
			r.approvals++
		case runtimeevent.KindTurnFinished:
			r.turns++
		}
		r.mu.Unlock()
		if evt.Kind == runtimeevent.KindApprovalRequested {
			var p runtimeevent.ApprovalRequestedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = r.client.ResolveApproval(context.Background(), app.ApprovalBinding{
					ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
				}, domain.DecisionDeny, nil)
			}
		}
	}
}

func (r *approvalRecorder) approvalCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.approvals
}

func (r *approvalRecorder) waitTurn(t *testing.T, n int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		r.mu.Lock()
		done := r.turns
		r.mu.Unlock()
		if done >= n {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for turn %d to finish", n)
}

// toolResultContains reports whether any tool result in the transcript
// carries text containing needle.
func toolResultContains(messages []domain.Message, needle string) bool {
	for _, m := range messages {
		for _, part := range m.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			for _, cp := range part.ToolResult.Content {
				if cp.Kind == domain.PartText && strings.Contains(cp.Text, needle) {
					return true
				}
			}
			if part.ToolResult.Error != nil && strings.Contains(part.ToolResult.Error.Message, needle) {
				return true
			}
		}
	}
	return false
}
