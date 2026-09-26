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
// Created: 2026/09/26

package e2e

import (
	"context"
	"encoding/json"
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

// TestPermissionRealModelE2E is the real-model acceptance suite for the
// dev-mode (danger-only) approval-necessity work: every prompt the user
// sees must be necessary, and every decision must carry its provenance.
// Five acts run in ONE session against a real provider:
//
//  1. a STATIC heredoc feeding an interpreter (python3 - <<'EOF') runs
//     with ZERO approval requests — its body is inline in the command
//     text, exactly as reviewable as -c inline code, so it no longer
//     carries the stdin-execution indicator that forced a prompt;
//  2. a PIPE feeding an interpreter (echo ... | python3) still asks —
//     runtime-delivered program text is genuinely unscreenable — and
//     the ask carries a non-empty ask_reason the card can show;
//  3. sudo still asks in dev mode (privilege-escalation indicator),
//     with the reason naming the escalation;
//  4. a DENIED approval feeds the model the verdict's reason plus an
//     explicit "do not retry the same shape" reroute hint;
//  5. "always allow" persists workspace-scoped (remembered store v4):
//     the stored package carries the workspace tag, and after a full
//     stack restart the same command runs without asking.
//
// The cross-workspace half of act 5 (the same binding must ask again in
// a DIFFERENT workspace) is covered at the unit level
// (TestRememberedStoreWorkspaceScope): the harness brings up one
// singleton workspace per env, and a second env would get its own
// isolated remembered store, so it cannot observe the scoping.
//
// Skipped unless LOOM_E2E_LLM=1 (real provider via the user's own config).
func TestPermissionRealModelE2E(t *testing.T) {
	ctx := context.Background()
	env := harness.NewEnv(t, harness.WithAdjust(func(resolved *config.ResolvedConfig) {
		resolved.Approval.Mode = permission.ModeDangerOnly
	}))

	c := env.NewClient(t)
	recorder := newAskRecorder(c, env.Subscribe(t, c))
	go recorder.run()

	// --- Act 1: static heredoc → no approval, sandboxed execution ---
	const heredocCode = "loom-heredoc-codeword-11"
	recorder.setPolicy(askAllow)
	submitVerbatimCmd(t, ctx, c, "python3 - <<'EOF'\nprint(\""+heredocCode+"\")\nEOF")
	waitTurnOrDump(t, recorder, 1, 5*time.Minute, c)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act1): %v", err)
	}
	if got := recorder.askCount(); got != 0 {
		t.Fatalf("act1: %d approval request(s) for a static heredoc, want 0 — %s",
			got, recorder.askSummary())
	}
	if !toolResultContains(snap.Messages, heredocCode) {
		dumpTranscript(t, snap.Messages)
		t.Fatalf("act1: heredoc output never reached the transcript (the command did not run?)")
	}
	t.Log("act1 ok: static heredoc ran sandboxed with zero approvals")

	// --- Act 2: pipe into interpreter → ask WITH ask_reason, then run ---
	const pipeCode = "loom-pipe-codeword-22"
	asksBefore := recorder.askCount()
	submitVerbatimCmd(t, ctx, c, "echo 'print(\""+pipeCode+"\")' | python3")
	waitTurnOrDump(t, recorder, 2, 5*time.Minute, c)

	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act2): %v", err)
	}
	ask, ok := recorder.lastAsk(asksBefore)
	if !ok {
		t.Fatalf("act2: no approval request for a pipe-fed interpreter — the stdin indicator must still fire")
	}
	if ask.AskReason == "" {
		t.Fatalf("act2: approval request carries no ask_reason — the card cannot explain itself: %+v", ask)
	}
	if !strings.Contains(ask.AskReason, "stdin") {
		t.Fatalf("act2: ask_reason = %q, want it to name the stdin-execution indicator", ask.AskReason)
	}
	if !toolResultContains(snap.Messages, pipeCode) {
		t.Fatalf("act2: approved pipe command never produced its output")
	}
	t.Logf("act2 ok: pipe-fed interpreter asked once, reason %q, approved run succeeded", ask.AskReason)

	// --- Act 3: sudo still asks in dev mode ---
	asksBefore = recorder.askCount()
	submitVerbatimCmd(t, ctx, c, "sudo -n true")
	waitTurnOrDump(t, recorder, 3, 5*time.Minute, c)

	ask, ok = recorder.lastAsk(asksBefore)
	if !ok {
		t.Fatalf("act3: sudo did not ask in dev mode — the privilege-escalation indicator must stand")
	}
	if !strings.Contains(ask.AskReason, "sudo") {
		t.Fatalf("act3: ask_reason = %q, want it to name the privilege escalation", ask.AskReason)
	}
	t.Logf("act3 ok: sudo asked with reason %q", ask.AskReason)

	// --- Act 4: a denied approval teaches the model why and how to reroute ---
	recorder.setPolicy(askDeny)
	asksBefore = recorder.askCount()
	submitVerbatimCmd(t, ctx, c, "echo 'print(1)' | python3")
	waitTurnOrDump(t, recorder, 4, 5*time.Minute, c)

	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(act4): %v", err)
	}
	if _, ok := recorder.lastAsk(asksBefore); !ok {
		t.Fatalf("act4: the pipe command was never asked — nothing to deny")
	}
	if !toolResultContains(snap.Messages, "denied by the user") {
		t.Fatalf("act4: transcript lacks the denial — the refused call must report back")
	}
	if !toolResultContains(snap.Messages, "approval was required because:") {
		t.Fatalf("act4: denial feedback lacks the ask reason — the model cannot learn what to avoid")
	}
	if !toolResultContains(snap.Messages, "do not retry the same command shape") {
		t.Fatalf("act4: denial feedback lacks the reroute hint")
	}
	t.Log("act4 ok: denial feedback carries the ask reason and the reroute hint")

	// --- Act 5: always-allow persists workspace-scoped across a restart ---
	// The remembered shape is `git push`: it asks in dev mode
	// (shared-state consequence), and — unlike a multi-step pipe — its
	// single-step argv earns a categorical memory binding. The command
	// FAILS (the workspace is not a git repository); that is irrelevant
	// to every assertion, which concerns the permission trail only.
	recorder.setPolicy(askAllowRemember)
	asksBefore = recorder.askCount()
	submitVerbatimCmd(t, ctx, c, "git push")
	waitTurnOrDump(t, recorder, 5, 5*time.Minute, c)

	ask, ok = recorder.lastAsk(asksBefore)
	if !ok {
		dumpTranscript(t, mustSnapshot(t, ctx, c))
		t.Fatalf("act5: git push was never asked — nothing to remember")
	}
	note := recorder.resolveNote(ask.ApprovalID)
	if note == "" {
		t.Fatalf("act5: allow-always produced no memory note — the call was not remembered")
	}
	t.Logf("act5a ok: remembered with note %q", note)

	// The stored package must carry THIS workspace's tag (schema v4) —
	// the persisted form of the card's "for this workspace" promise.
	// The policy works with the canonical (symlink-resolved) workspace
	// path — on macOS t.TempDir() returns /var/... while the resolved
	// form is /private/var/... — so the assertion compares canonical
	// forms, exactly as the runtime does.
	wantWS, err := filepath.EvalSymlinks(env.Workspace)
	if err != nil {
		t.Fatalf("act5: resolve workspace: %v", err)
	}
	stored, err := permission.LoadRememberedPackages(ctx,
		permission.RememberedDBPath(env.Resolved.Storage.RulesDir()))
	if err != nil {
		t.Fatalf("act5: load remembered store: %v", err)
	}
	var tagged bool
	for _, p := range stored {
		if p.Workspace == wantWS {
			tagged = true
		}
	}
	if !tagged {
		t.Fatalf("act5: no remembered package tagged with workspace %q: %+v", wantWS, stored)
	}
	t.Logf("act5b ok: remembered store v4 persists the workspace tag (%d package(s))", len(stored))

	// Restart the whole stack on the SAME home: the reloaded policy must
	// answer the identical command from the persisted package alone.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	_ = env.Svc.Shutdown(shutdownCtx)
	cancel()
	env.StartStack(t)

	c2 := client.NewInProc(env.Svc)
	if err := c2.ResumeSession(ctx, c.SessionID()); err != nil {
		t.Fatalf("act5: ResumeSession: %v", err)
	}
	recorder2 := newAskRecorder(c2, env.Subscribe(t, c2))
	go recorder2.run()
	recorder2.setPolicy(askAllow) // a stray ask must not hang the turn; the count is the assertion

	submitVerbatimCmd(t, ctx, c2, "git push")
	waitTurnOrDump(t, recorder2, 1, 5*time.Minute, c2)

	if got := recorder2.askCount(); got != 0 {
		t.Fatalf("act5: %d approval request(s) after restart for the remembered command, want 0 — %s",
			got, recorder2.askSummary())
	}
	t.Log("act5c ok: remembered command ran approval-free after a full stack restart")

	t.Log("ACCEPTANCE PASS: dev-mode approvals are necessary (heredoc free / pipe+sudo asked) and every ask is valuable (reason shown, denial teaches, memory persists workspace-scoped)")
}

// submitVerbatimCmd prompts the model to run ONE pinned command through
// run_cmd without rewriting it: the command text is the acceptance
// fixture, so any paraphrase (script file, -c form) would invalidate the
// act's signal.
func submitVerbatimCmd(t *testing.T, ctx context.Context, c client.Client, command string) {
	t.Helper()
	prompt := "用 run_cmd 工具执行下面这条命令，必须原样执行（不要改写、不要换成其他形式、不要先写文件）：\n\n" +
		command + "\n\n执行后把命令输出原样复述给我。如果执行失败，直接告诉我失败原因，不要换命令重试。"
	if _, err := c.SubmitPrompt(ctx, prompt, nil); err != nil {
		t.Fatalf("SubmitPrompt(%q): %v", command, err)
	}
}

// waitTurnOrDump waits like askRecorder.waitTurn but dumps the live
// transcript on timeout — a stalled turn (hung command, unresolved
// approval) is otherwise invisible in the failure output.
func waitTurnOrDump(t *testing.T, r *askRecorder, n int, timeout time.Duration, c client.Client) {
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
	snap, err := c.RequestSnapshot(context.Background())
	if err == nil {
		dumpTranscript(t, snap.Messages)
	}
	t.Fatalf("timed out waiting for turn %d to finish (asks so far:\n%s)", n, r.askSummary())
}

// mustSnapshot fetches the session snapshot or fails the test.
func mustSnapshot(t *testing.T, ctx context.Context, c client.Client) []domain.Message {
	t.Helper()
	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	return snap.Messages
}

// dumpTranscript logs a compact view of the transcript for diagnosing
// model-behavior failures: every tool call's name+arguments and every
// tool error, plus the final assistant text.
func dumpTranscript(t *testing.T, messages []domain.Message) {
	t.Helper()
	for _, m := range messages {
		for _, part := range m.Parts {
			switch part.Kind {
			case domain.PartToolCall:
				if part.ToolCall != nil {
					args := string(part.ToolCall.Arguments)
					if len(args) > 300 {
						args = args[:300] + "…"
					}
					t.Logf("tool call: %s %s", part.ToolCall.Name, args)
				}
			case domain.PartToolResult:
				if part.ToolResult == nil {
					continue
				}
				if part.ToolResult.Error != nil {
					t.Logf("tool error: %s", part.ToolResult.Error.Message)
				}
				for _, cp := range part.ToolResult.Content {
					if cp.Kind != domain.PartText {
						continue
					}
					text := cp.Text
					if len(text) > 400 {
						text = text[:400] + "…"
					}
					t.Logf("tool result: %s", text)
				}
			}
		}
	}
	if text := lastAssistantText(messages); text != "" {
		if len(text) > 400 {
			text = text[:400] + "…"
		}
		t.Logf("final assistant text: %s", text)
	}
}

// askPolicy decides how the recorder resolves an approval request.
type askPolicy func(runtimeevent.ApprovalRequestedPayload) (domain.Decision, *app.ApprovalRuleHint)

var (
	askAllow = func(runtimeevent.ApprovalRequestedPayload) (domain.Decision, *app.ApprovalRuleHint) {
		return domain.DecisionAllow, nil
	}
	askDeny = func(runtimeevent.ApprovalRequestedPayload) (domain.Decision, *app.ApprovalRuleHint) {
		return domain.DecisionDeny, nil
	}
	askAllowRemember = func(runtimeevent.ApprovalRequestedPayload) (domain.Decision, *app.ApprovalRuleHint) {
		return domain.DecisionAllow, &app.ApprovalRuleHint{}
	}
)

// askRecorder drains the event subscription, recording every approval
// request payload and resolving each through the current policy. It
// mirrors the approvalRecorder in user_intent_e2e_test.go but keeps the
// full payloads (ask_reason assertions) and the resolution notes (the
// "always allow" memory evidence).
type askRecorder struct {
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu     sync.Mutex
	asks   []runtimeevent.ApprovalRequestedPayload
	notes  map[domain.EventID]string
	turns  int
	policy askPolicy
}

func newAskRecorder(c client.Client, ch <-chan runtimeevent.RuntimeEvent) *askRecorder {
	return &askRecorder{client: c, ch: ch, notes: map[domain.EventID]string{}, policy: askAllow}
}

func (r *askRecorder) run() {
	for evt := range r.ch {
		r.mu.Lock()
		if evt.Kind == runtimeevent.KindTurnFinished {
			r.turns++
		}
		r.mu.Unlock()
		if evt.Kind != runtimeevent.KindApprovalRequested {
			continue
		}
		var p runtimeevent.ApprovalRequestedPayload
		if err := json.Unmarshal(evt.Payload, &p); err != nil {
			continue
		}
		r.mu.Lock()
		r.asks = append(r.asks, p)
		policy := r.policy
		r.mu.Unlock()
		decision, hint := policy(p)
		note, _ := r.client.ResolveApproval(context.Background(), app.ApprovalBinding{
			ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
		}, decision, hint)
		r.mu.Lock()
		r.notes[p.ApprovalID] = note
		r.mu.Unlock()
	}
}

func (r *askRecorder) setPolicy(p askPolicy) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.policy = p
}

func (r *askRecorder) askCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.asks)
}

// lastAsk returns the first ask recorded at or after index from — the
// ask belonging to the act that started with askCount()==from.
func (r *askRecorder) lastAsk(from int) (runtimeevent.ApprovalRequestedPayload, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.asks) <= from {
		return runtimeevent.ApprovalRequestedPayload{}, false
	}
	return r.asks[from], true
}

func (r *askRecorder) resolveNote(id domain.EventID) string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.notes[id]
}

func (r *askRecorder) askSummary() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var sb strings.Builder
	for _, a := range r.asks {
		sb.WriteString("[" + a.ToolName + "] " + a.Description + " (reason: " + a.AskReason + ")\n")
	}
	return sb.String()
}
