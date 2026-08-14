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
// Created: 2026/08/03

package e2e

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// TestServeRealModelE2E is the M1' acceptance suite against a REAL LLM
// provider (the user's own ~/.loom/config.yaml). It is skipped unless
// LOOM_E2E_LLM=1, so CI never pays for or depends on a live model.
//
// Acceptance coverage (docs/SERVE_DESIGN.md §12 M1' 验收):
//  1. full turn with a real tool loop (read_file), verified end-to-end by
//     a code word the model can only know by reading the file;
//  2. runtime events flow through SessionService's pump/subscription;
//  3. snapshot watermark handoff (SubscribeEvents at Snapshot.EventSeq);
//  4. busy-turn steering;
//  5. idempotent submission;
//  6. session resume via a fresh client;
//  7. approval/question requests are auto-resolved to keep turns moving
//     (and thereby exercised when the policy requires them).
func TestServeRealModelE2E(t *testing.T) {
	ctx := context.Background()
	resolved, tmp, workspace := realModelHome(t)
	const codeWord = "loom-e2e-m1-biplane-42"
	if err := os.WriteFile(filepath.Join(workspace, "marker.txt"), []byte("口令是："+codeWord+"\n"), 0o600); err != nil {
		t.Fatalf("write marker: %v", err)
	}

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(tmp, "artifacts"),
		Version:     "e2e",
		Logger:      discard,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	defer proc.Close()
	bootstrap, err := app.NewWorkspaceBootstrap(ctx, proc, app.BootstrapConfig{
		WorkspaceRoot: workspace,
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	}()

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// --- 1+2. full turn with a real tool loop ---
	prompt := fmt.Sprintf("用 read_file 工具读取 %s 这个文件，然后只回答文件里的口令本身，不要提问。", filepath.Join(workspace, "marker.txt"))
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, prompt, nil); err != nil {
		t.Fatalf("SubmitPrompt(turn1): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	finalText := lastAssistantText(snap.Messages)
	if !strings.Contains(finalText, codeWord) {
		t.Fatalf("final answer %q does not contain the code word %q (tool loop broken?)", finalText, codeWord)
	}
	if snap.TurnCount != 1 {
		t.Fatalf("TurnCount = %d, want 1", snap.TurnCount)
	}
	if snap.EventSeq == 0 {
		t.Fatalf("snapshot EventSeq = 0, want a live watermark")
	}
	collector.assertSaw(
		t,
		runtimeevent.KindTurnStarted,
		runtimeevent.KindModelResponseCompleted,
		runtimeevent.KindTurnFinished,
	)
	collector.assertSawAny(t, runtimeevent.KindToolStarted, runtimeevent.KindToolPrepared)
	t.Logf("turn1 ok: code word verified via tool loop, %d events", collector.count())

	// --- 3. snapshot watermark handoff ---
	live, err := c.SubscribeEvents(ctx, snap.EventSeq)
	if err != nil {
		t.Fatalf("SubscribeEvents(EventSeq=%d): %v (watermark handoff must succeed)", snap.EventSeq, err)
	}
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, "用一个字回答：好", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn2): %v", err)
	}
	waitForAnyEvent(t, live, sessionID, 2*time.Minute)
	collector.waitTurn(t, turns+1, 3*time.Minute)
	t.Log("watermark handoff ok: subscribe at EventSeq, live events flow")

	// --- 4. busy-turn steering ---
	if _, err := c.SubmitPrompt(ctx, "慢慢数：用中文从一数到二十，每个数字单独一行。", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn3): %v", err)
	}
	// The real model latency gives a natural busy window; retry briefly to
	// avoid racing a suspiciously fast first token.
	var steered bool
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		result, err := c.SubmitPrompt(ctx, "不用数了，直接说：停。", nil)
		if err != nil {
			t.Fatalf("SubmitPrompt(steer): %v", err)
		}
		if result.Steered {
			steered = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !steered {
		t.Fatalf("second submission during an active turn was not steered")
	}
	// The steered message either drains mid-turn or relays as the next
	// turn; both paths end with the session idle and the message in the
	// transcript.
	steerDeadline := time.Now().Add(3 * time.Minute)
	for {
		snap, err = c.RequestSnapshot(ctx)
		if err != nil {
			t.Fatalf("RequestSnapshot(after steer): %v", err)
		}
		if snap.State == app.ControllerStateIdle && transcriptContains(snap.Messages, "停") {
			break
		}
		if time.Now().After(steerDeadline) {
			t.Fatalf("steered message never reached the transcript (state=%s)", snap.State)
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Log("steer ok: busy submission queued and drained")

	// --- 4b. busy-turn followup: queued for AFTER the turn, then relayed ---
	if _, err := c.SubmitPrompt(ctx, "再慢慢数：用中文从二十一数到三十，每个数字单独一行。", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn4): %v", err)
	}
	// Same busy-window retry as the steer case above.
	var followupQueued bool
	followupDeadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(followupDeadline) {
		result, err := c.SubmitFollowup(ctx, "用一个字回答：好")
		if err != nil {
			t.Fatalf("SubmitFollowup: %v", err)
		}
		if result.Followup {
			followupQueued = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !followupQueued {
		t.Fatalf("followup submission during an active turn was not queued")
	}
	// The followup must not leak into the busy turn: it becomes its own
	// turn right after — two more turn.finished events in total.
	turns = collector.turnsDone()
	collector.waitTurn(t, turns+2, 5*time.Minute)
	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(after followup): %v", err)
	}
	if !transcriptContains(snap.Messages, "用一个字回答：好") {
		t.Fatalf("followup prompt never became its own turn")
	}
	if len(snap.PendingFollowups) != 0 {
		t.Fatalf("PendingFollowups = %v after the relay, want empty", snap.PendingFollowups)
	}
	t.Log("followup ok: queued during the busy turn, relayed as its own turn")

	// --- 5. idempotent submission ---
	turnsBefore := snap.TurnCount
	turns = collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, "用一个字回答：嗯", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn4): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	// Repeat the exact submission through the service's idempotency channel.
	turns = collector.turnsDone()
	res, dedup, err := svc.SubmitPrompt(ctx, sessionID, "用一个字回答：嗯", nil, "idem-e2e-1", false)
	if err != nil {
		t.Fatalf("SubmitPrompt(idem first): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	res2, dedup2, err := svc.SubmitPrompt(ctx, sessionID, "用一个字回答：嗯", nil, "idem-e2e-1", false)
	if err != nil {
		t.Fatalf("SubmitPrompt(idem repeat): %v", err)
	}
	if !dedup2 || res2 != res {
		t.Fatalf("idempotent repeat = (%+v, %v), want (%+v, true)", res2, dedup2, res)
	}
	if dedup {
		t.Fatalf("first idempotent submit marked deduplicated")
	}
	snap, err = c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(after idem): %v", err)
	}
	if snap.TurnCount != turnsBefore+2 {
		t.Fatalf("TurnCount = %d, want %d (repeat must not add a turn)", snap.TurnCount, turnsBefore+2)
	}
	t.Log("idempotency ok")

	// --- 6. resume via a fresh client ---
	c2 := client.NewInProc(svc)
	if err := c2.ResumeSession(ctx, sessionID); err != nil {
		t.Fatalf("ResumeSession: %v", err)
	}
	snap2, err := c2.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot(resumed): %v", err)
	}
	if snap2.TurnCount != snap.TurnCount {
		t.Fatalf("resumed TurnCount = %d, want %d", snap2.TurnCount, snap.TurnCount)
	}
	if !transcriptContains(snap2.Messages, codeWord) {
		t.Fatalf("resumed transcript lost the code-word exchange")
	}
	t.Logf("resume ok: %d turns, transcript intact", snap2.TurnCount)

	t.Log("ACCEPTANCE PASS: real-model serve-path e2e complete")
}

// TestServeRealModelPrepareFailureE2E is the real-model acceptance for the
// prepare-failure diagnosability work:
//  1. the model-facing error for a nonexistent search path NAMES the path,
//     so a parallel-call failure is attributable without call-ID forensics;
//  2. the degraded tool.call_prepared event persists a sanitized
//     args_summary (whitelisted keys only), so the failing input is
//     diagnosable from the event log alone.
//
// Skipped unless LOOM_E2E_LLM=1 (real provider via the user's own config).
func TestServeRealModelPrepareFailureE2E(t *testing.T) {
	ctx := context.Background()
	resolved, tmp, workspace := realModelHome(t)

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(tmp, "artifacts"),
		Version:     "e2e",
		Logger:      discard,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	defer proc.Close()
	bootstrap, err := app.NewWorkspaceBootstrap(ctx, proc, app.BootstrapConfig{
		WorkspaceRoot: workspace,
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	}()

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// The path is deliberately absent from the workspace; the prompt pins
	// the exact argument values so the failing call is deterministic.
	const missingPath = "internal/config/example.go"
	prompt := "调用一次 search 工具，参数原样使用：path 填 \"internal/config/example.go\"，pattern 填 \"storage\"。" +
		"不要先确认文件是否存在，不要更换路径，不要调用其他工具，原样发出这一次工具调用。" +
		"收到工具结果后，把工具返回的错误消息原文复述给我。"
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, prompt, nil); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	// 1. The model-visible tool error must name the offending path.
	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	var toolErrText string
	for _, msg := range snap.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil && part.ToolResult.Error != nil {
				toolErrText += part.ToolResult.Error.Message + "\n"
			}
		}
	}
	wantErr := `path does not exist: "` + missingPath + `"`
	if !strings.Contains(toolErrText, wantErr) {
		t.Fatalf("tool error = %q, want it to contain %q", toolErrText, wantErr)
	}
	t.Logf("model-visible error names the path: %q", strings.TrimSpace(toolErrText))

	// 2. The durable degraded prepared event must carry args_summary.
	store, err := sessionStoreReadOnly(ctx, resolved)
	if err != nil {
		t.Fatalf("open session store: %v", err)
	}
	defer store.Close()
	persisted, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	var degraded [][]byte
	for _, evt := range persisted {
		if evt.Type == domain.EventToolCallPrepared && strings.Contains(string(evt.Payload), `"prepare_failed":true`) {
			degraded = append(degraded, evt.Payload)
		}
	}
	if len(degraded) == 0 {
		t.Fatal("no degraded tool.call_prepared event persisted")
	}
	var summaryFound bool
	for _, p := range degraded {
		if strings.Contains(string(p), `"args_summary"`) && strings.Contains(string(p), missingPath) {
			summaryFound = true
		}
		if strings.Contains(string(p), `"content"`) {
			t.Fatalf("non-whitelisted argument leaked into the durable payload: %s", p)
		}
	}
	if !summaryFound {
		t.Fatalf("no degraded payload carries args_summary with the failing path: %s", degraded)
	}
	t.Logf("durable args_summary verified across %d degraded event(s)", len(degraded))

	t.Log("ACCEPTANCE PASS: prepare failure names the path and persists a sanitized args_summary")
}

// TestServeRealModelWritablePathsE2E is the real-model acceptance for the
// run_cmd writable_paths scoped grant: a command that drops a file OUTSIDE
// the workspace must be completed WITHOUT leaving the sandbox — the model
// is expected to declare writable_paths (the scoped in-sandbox widening)
// instead of require_escalated, exactly the behavior the dsx/talos
// approval-storm session motivated.
//
// Acceptance coverage:
//  1. the file lands on disk with the expected content (real write through
//     the seatbelt sandbox with a writable-path grant);
//  2. no approval request ever carries ESCALATED(no-sandbox) — the run
//     never leaves the sandbox;
//  3. at least one approval request carries the scoped writable=[...]
//     grant, proving the model reached for writable_paths.
//
// Skipped unless LOOM_E2E_LLM=1 (real provider via the user's own config).
func TestServeRealModelWritablePathsE2E(t *testing.T) {
	ctx := context.Background()
	resolved, tmp, workspace := realModelHome(t)
	// The write target must sit outside BOTH the workspace and the
	// sandbox's default writable temp dir — otherwise the write succeeds
	// without any grant and the test accepts nothing (an earlier version
	// used a t.TempDir()-relative target, which lives under $TMPDIR and
	// is writable by default). A throwaway directory directly in the
	// user's home mirrors the dsx ~/Library/Logs case exactly.
	home, err := os.UserHomeDir()
	if err != nil {
		t.Skip("no home dir")
	}
	outside, err := os.MkdirTemp(home, ".loom-writable-e2e-")
	if err != nil {
		t.Fatalf("create outside dir: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	target := filepath.Join(outside, "result.txt")
	const codeWord = "loom-writable-e2e-ok"

	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(tmp, "artifacts"),
		Version:     "e2e",
		Logger:      discard,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	defer proc.Close()
	bootstrap, err := app.NewWorkspaceBootstrap(ctx, proc, app.BootstrapConfig{
		WorkspaceRoot: workspace,
	})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	defer bootstrap.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	defer broker.Close()
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	}()

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// The prompt pins the OUT-OF-WORKSPACE target and the in-sandbox
	// preference, but deliberately does NOT name the writable_paths
	// parameter: the model is expected to learn it from the tool
	// description and the sandbox guidance note, which is exactly the
	// behavior being accepted.
	prompt := fmt.Sprintf("把文本 %q 写入文件 %s（该路径在工作区之外）。要求：\n"+
		"1. 必须通过 run_cmd 执行命令完成写入（不要用 write/edit 工具）；\n"+
		"2. 让命令留在沙箱内运行：如果默认沙箱拦截了这次写入，按工具描述和返回指引选择最小化的沙箱内授权方式重试，直到写入成功，不要放弃；\n"+
		"3. 写入成功后用 run_cmd 读取该文件，把文件内容原样复述给我。",
		codeWord, target)
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, prompt, nil); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	collector.waitTurn(t, turns+1, 5*time.Minute)

	// 1. The file must really have landed through the sandboxed grant.
	data, err := os.ReadFile(target)
	if err != nil {
		t.Fatalf("target file was never written: %v (turn error: %s)", err, collector.lastTurnError())
	}
	if !strings.Contains(string(data), codeWord) {
		t.Fatalf("target content = %q, want it to contain %q", data, codeWord)
	}
	t.Logf("write landed via sandboxed grant: %q", strings.TrimSpace(string(data)))

	// 2+3. Inspect the durable permission trail: no escalation, at least
	// one scoped writable-path ask.
	store, err := sessionStoreReadOnly(ctx, resolved)
	if err != nil {
		t.Fatalf("open session store: %v", err)
	}
	defer store.Close()
	persisted, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	var asks int
	var writableAsk bool
	for _, evt := range persisted {
		if evt.Type != domain.EventPermissionRequested {
			continue
		}
		asks++
		payload := string(evt.Payload)
		t.Logf("permission ask #%d: %s", asks, payload)
		if strings.Contains(payload, "ESCALATED(no-sandbox)") {
			t.Fatalf("the run escalated out of the sandbox; writable_paths should have sufficed:\n%s", payload)
		}
		if strings.Contains(payload, "writable=[") {
			writableAsk = true
		}
	}
	if !writableAsk {
		t.Fatalf("no permission request carried a scoped writable grant (%d asks); the model never used writable_paths", asks)
	}
	t.Logf("permission trail: %d ask(s), all sandboxed, scoped writable grant present", asks)

	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if !strings.Contains(lastAssistantText(snap.Messages), codeWord) {
		t.Logf("note: final answer did not echo the code word (non-fatal; the file content is authoritative)")
	}

	t.Log("ACCEPTANCE PASS: out-of-workspace write completed via scoped writable_paths grant, zero sandbox escapes")
}

// sessionStoreReadOnly opens the isolated session store for post-turn
// inspection of the durable event log.
func sessionStoreReadOnly(ctx context.Context, cfg *config.ResolvedConfig) (*session.SQLiteStore, error) {
	return session.OpenSQLiteStoreReadOnly(ctx, cfg.Storage.SessionDBPath())
}

// TestServeRealModelPruneCompactionE2E is the real-model acceptance for
// the Level-0 tool-result pruner: a medium (8–16KB) tool output must be
// middle-pruned INLINE during a forced compaction — head and tail
// survive, the marker records the omission, nothing is externalized —
// and the session keeps working afterwards. It also verifies the
// ignorable marking on the persisted log (audit events marked,
// transcript events not).
func TestServeRealModelPruneCompactionE2E(t *testing.T) {
	resolved, home, workspace := realModelHome(t)
	ctx := context.Background()
	svc := startRealModelService(t, ctx, resolved, home, workspace)

	// ~10KB: inside the [8KB, 16KB) prune band — below the mask threshold
	// and below the ingestion truncation.
	var big strings.Builder
	big.WriteString("HEAD-SENTINEL-BEGIN\n")
	for i := 0; i < 165; i++ {
		fmt.Fprintf(&big, "line-%03d %s\n", i, strings.Repeat("z", 55))
	}
	big.WriteString("TAIL-SENTINEL-END\n")
	bigPath := filepath.Join(workspace, "band.txt")
	if err := os.WriteFile(bigPath, []byte(big.String()), 0o600); err != nil {
		t.Fatalf("write band file: %v", err)
	}
	smallPath := filepath.Join(workspace, "small.txt")
	if err := os.WriteFile(smallPath, []byte("tiny\n"), 0o600); err != nil {
		t.Fatalf("write small file: %v", err)
	}

	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c, ch: events}
	go collector.run()

	// Turn 1: the model reads the ~10KB file into the transcript.
	turns := collector.turnsDone()
	if _, err := c.SubmitPrompt(ctx, fmt.Sprintf("用 read_file 工具读取 %s 这个文件，然后只回答：好", bigPath), nil); err != nil {
		t.Fatalf("SubmitPrompt(turn1): %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)
	// Turn 2 (filler): pushes the big output out of the keep-recent window.
	if _, err := c.SubmitPrompt(ctx, fmt.Sprintf("用 read_file 工具读取 %s 这个文件，然后只回答：好", smallPath), nil); err != nil {
		t.Fatalf("SubmitPrompt(turn2): %v", err)
	}
	collector.waitTurn(t, turns+2, 3*time.Minute)

	// Forced compaction ahead of turn 3: the big output sits outside the
	// trailing window and must be middle-pruned inline.
	if _, err := c.RequestCompaction(ctx); err != nil {
		t.Fatalf("RequestCompaction: %v", err)
	}
	if _, err := c.SubmitPrompt(ctx, "用一个字回答：嗯", nil); err != nil {
		t.Fatalf("SubmitPrompt(turn3): %v", err)
	}
	collector.waitTurn(t, turns+3, 3*time.Minute)

	// The transcript must show the pruned form: marker + head + tail.
	snap, err := c.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	var prunedText string
	for _, msg := range snap.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				for _, content := range part.ToolResult.Content {
					if strings.Contains(content.Text, "[... tool result middle pruned:") {
						prunedText = content.Text
					}
				}
			}
		}
	}
	if prunedText == "" {
		t.Fatal("no middle-pruned tool output in the transcript after forced compaction")
	}
	if !strings.Contains(prunedText, "HEAD-SENTINEL-BEGIN") || !strings.Contains(prunedText, "TAIL-SENTINEL-END") {
		t.Fatalf("pruned output lost its head/tail sentinels: %q...", prunedText[:120])
	}

	// The durable audit: one context.masked directive carrying exactly one
	// prune and zero masks; audit events are ignorable, transcript events
	// are not.
	store, err := sessionStoreReadOnly(ctx, resolved)
	if err != nil {
		t.Fatalf("open session store: %v", err)
	}
	defer store.Close()
	persisted, err := store.LoadEvents(ctx, c.SessionID(), 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	var sawPrune, sawIgnorableAudit bool
	for _, evt := range persisted {
		switch evt.Type {
		case domain.EventContextMasked:
			var payload domain.ContextMaskedPayload
			if err := json.Unmarshal(evt.Payload, &payload); err != nil {
				t.Fatalf("unmarshal masked payload: %v", err)
			}
			if len(payload.Prunes) != 1 || len(payload.Masks) != 0 {
				t.Fatalf("masked payload = %d prunes / %d masks, want 1/0", len(payload.Prunes), len(payload.Masks))
			}
			sawPrune = true
		case domain.EventModelRequestStarted:
			if !evt.Ignorable {
				t.Fatal("model.request_started is pure audit and must be ignorable")
			}
			sawIgnorableAudit = true
		case domain.EventUserMessageAdded:
			if evt.Ignorable {
				t.Fatal("user.message_added carries surface state and must NOT be ignorable")
			}
		}
	}
	if !sawPrune {
		t.Fatal("no context.masked prune directive persisted")
	}
	if !sawIgnorableAudit {
		t.Fatal("no model.request_started event persisted")
	}
	t.Log("ACCEPTANCE PASS: Level-0 pruner + ignorable marks verified against a real model")
}

// TestServeRealModelOrphanRecoveryE2E is the real-model acceptance for
// crash-orphaned run closure: a session whose log tail is an unfinished
// run (the process died mid-turn) recovers with a run.interrupted audit
// marker and a visible interruption projection, then continues with a
// real turn.
func TestServeRealModelOrphanRecoveryE2E(t *testing.T) {
	resolved, home, workspace := realModelHome(t)
	ctx := context.Background()

	svc := startRealModelService(t, ctx, resolved, home, workspace)
	c := client.NewInProc(svc)
	if err := c.NewSession(ctx); err != nil {
		t.Fatalf("NewSession: %v", err)
	}
	sessionID := c.SessionID()

	// Craft the crash tail directly in the store: a run that started and
	// showed activity but never reached a terminal event.
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	existing, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	base := int64(len(existing))
	now := time.Now().UTC()
	deadRunID := domain.NewRunID()
	userMsg := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "crashed turn"}},
		CreatedAt: now,
	}
	runCreated, err := domain.MarshalPayload(struct {
		RunID domain.RunID `json:"run_id"`
	}{RunID: deadRunID})
	if err != nil {
		t.Fatalf("marshal run.created: %v", err)
	}
	userAdded, err := domain.MarshalPayload(domain.MessageEventPayload{Message: userMsg})
	if err != nil {
		t.Fatalf("marshal user message: %v", err)
	}
	tail := []domain.Event{
		{ID: domain.NewEventID(), Sequence: base + 1, SessionID: sessionID, Type: domain.EventRunCreated, Timestamp: now, Payload: runCreated},
		{ID: domain.NewEventID(), Sequence: base + 2, SessionID: sessionID, Type: domain.EventUserMessageAdded, Timestamp: now, Payload: userAdded},
		{ID: domain.NewEventID(), Sequence: base + 3, SessionID: sessionID, Type: domain.EventModelRequestStarted, Timestamp: now},
	}
	ckpt := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: base + 3,
		State:    domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		Messages: []domain.Message{userMsg}, CreatedAt: now,
	}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, base, tail, ckpt); err != nil {
		t.Fatalf("append crash tail: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	// Recovery only runs when no live controller owns the session: shut
	// the first service down and resume through a fresh one.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	_ = svc.Shutdown(shutdownCtx)
	cancel()
	svc2 := startRealModelService(t, ctx, resolved, home, workspace)
	c2 := client.NewInProc(svc2)
	if err := c2.ResumeSession(ctx, sessionID); err != nil {
		t.Fatalf("ResumeSession: %v", err)
	}

	// The interruption projection is visible from the snapshot alone.
	snap, err := c2.RequestSnapshot(ctx)
	if err != nil {
		t.Fatalf("RequestSnapshot: %v", err)
	}
	if snap.LastError == nil || !strings.Contains(snap.LastError.Message, "ended before completing") {
		t.Fatalf("snapshot.last_error = %+v, want the interruption notice", snap.LastError)
	}

	// The recovered session keeps working against the real model.
	eventsCtx, stopEvents := context.WithCancel(ctx)
	defer stopEvents()
	events, err := c2.SubscribeEvents(eventsCtx, 0)
	if err != nil {
		t.Fatalf("SubscribeEvents: %v", err)
	}
	collector := &eventCollector{client: c2, ch: events}
	go collector.run()
	turns := collector.turnsDone()
	if _, err := c2.SubmitPrompt(ctx, "用一个字回答：好", nil); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	collector.waitTurn(t, turns+1, 3*time.Minute)

	// The orphan marker persisted with the dead run's identity.
	store2, err := sessionStoreReadOnly(ctx, resolved)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer store2.Close()
	persisted, err := store2.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	var marker *domain.Event
	for i := range persisted {
		if persisted[i].Type == domain.EventRunInterrupted {
			marker = &persisted[i]
		}
	}
	if marker == nil {
		t.Fatal("no run.interrupted marker persisted for the crash-orphaned run")
	}
	var payload struct {
		RunID domain.RunID `json:"run_id"`
	}
	if err := json.Unmarshal(marker.Payload, &payload); err != nil {
		t.Fatalf("unmarshal marker: %v", err)
	}
	if payload.RunID != deadRunID {
		t.Fatalf("marker run_id = %s, want the dead run %s", payload.RunID, deadRunID)
	}
	if !marker.Ignorable {
		t.Fatal("run.interrupted is pure audit and must be ignorable")
	}
	t.Log("ACCEPTANCE PASS: crash-orphaned run marked interrupted, session recovered with a real turn")
}

// readRealUserConfig reads the user's own loom config (<loom
// home>/config.yaml; LOOM_HOME or ~/.loom) for the real-model suites.
func readRealUserConfig(t *testing.T) []byte {
	t.Helper()
	home, err := config.HomeDir(os.LookupEnv)
	if err != nil {
		t.Skipf("no home dir: %v", err)
	}
	raw, err := os.ReadFile(config.ConfigPathForHome(home))
	if err != nil {
		t.Skipf("loom config not found at %s", config.ConfigPathForHome(home))
	}
	return raw
}

// loadIsolatedConfig copies raw into a fresh temp loom home and loads it
// from there, so every writable location derives from the temp home and
// the user's stores stay untouched. Returns the temp home and the
// resolved config.
func loadIsolatedConfig(t *testing.T, raw []byte) (string, *config.ResolvedConfig) {
	t.Helper()
	tmp := t.TempDir()
	if err := os.WriteFile(config.ConfigPathForHome(tmp), raw, 0o600); err != nil {
		t.Fatalf("write isolated config: %v", err)
	}
	resolved, err := config.Load(tmp, config.LoadOptions{RequireProviders: true, Logger: slog.Default()}, os.LookupEnv)
	if err != nil {
		t.Skipf("load loom config: %v", err)
	}
	return tmp, resolved
}

// realModelHome prepares an isolated loom home for the real-model suites:
// the user's config copied into a temp home (so the user's stores stay
// untouched) plus a throwaway workspace. Returns the resolved config, the
// loom home, and the workspace root.
func realModelHome(t *testing.T) (*config.ResolvedConfig, string, string) {
	t.Helper()
	if os.Getenv("LOOM_E2E_LLM") != "1" {
		t.Skip("set LOOM_E2E_LLM=1 to run the real-model acceptance suite")
	}
	tmp, resolved := loadIsolatedConfig(t, readRealUserConfig(t))
	if err := os.MkdirAll(resolved.Storage.SessionsDir(), 0o700); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	workspace := filepath.Join(tmp, "ws")
	if err := os.MkdirAll(workspace, 0o755); err != nil {
		t.Fatalf("mkdir ws: %v", err)
	}
	return resolved, tmp, workspace
}

// startRealModelService brings up the serve-path stack (process runtime,
// workspace bootstrap, broker, singleton service) on an isolated home.
func startRealModelService(t *testing.T, ctx context.Context, resolved *config.ResolvedConfig, home, workspace string) *app.SessionService {
	t.Helper()
	discard := slog.New(slog.NewTextHandler(io.Discard, nil))
	proc, err := app.NewProcessRuntime(ctx, resolved, app.ProcessRuntimeConfig{
		ArtifactDir: filepath.Join(home, "artifacts"),
		Version:     "e2e",
		Logger:      discard,
	})
	if err != nil {
		t.Fatalf("NewProcessRuntime: %v", err)
	}
	t.Cleanup(func() { proc.Close() })
	bootstrap, err := app.NewWorkspaceBootstrap(ctx, proc, app.BootstrapConfig{WorkspaceRoot: workspace})
	if err != nil {
		t.Fatalf("NewWorkspaceBootstrap: %v", err)
	}
	t.Cleanup(func() { bootstrap.Close() })
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSingletonWorkspaceService(bootstrap, broker, app.SessionServiceConfig{Logger: discard})
	t.Cleanup(func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = svc.Shutdown(shutdownCtx)
	})
	return svc
}

// eventCollector drains a subscription and auto-resolves any
// approval/question requests so real-model turns never wedge.
type eventCollector struct {
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu    sync.Mutex
	seen  map[runtimeevent.RuntimeEventKind]int
	turns int
	// lastTurnErr carries the Error field of the most recent
	// KindTurnFinished event ("" for a clean finish) — the first thing to
	// inspect when a real-model turn comes back empty.
	lastTurnErr string
}

func (c *eventCollector) run() {
	c.seen = make(map[runtimeevent.RuntimeEventKind]int)
	for evt := range c.ch {
		c.mu.Lock()
		c.seen[evt.Kind]++
		if evt.Kind == runtimeevent.KindTurnFinished {
			c.turns++
			var p runtimeevent.TurnFinishedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				c.lastTurnErr = p.Error
			}
		}
		c.mu.Unlock()
		switch evt.Kind {
		case runtimeevent.KindApprovalRequested:
			var p runtimeevent.ApprovalRequestedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.ResolveApproval(context.Background(), app.ApprovalBinding{
					ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
				}, domain.DecisionAllow, nil)
			}
		case runtimeevent.KindQuestionAsked:
			var p runtimeevent.QuestionAskedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.AnswerQuestion(context.Background(), p.QuestionID, domain.QuestionAnswer{Skipped: true})
			}
		}
	}
}

func (c *eventCollector) turnsDone() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.turns
}

func (c *eventCollector) lastTurnError() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.lastTurnErr
}

func (c *eventCollector) waitTurn(t *testing.T, n int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if c.turnsDone() >= n {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for turn %d to finish (done=%d)", n, c.turnsDone())
}

func (c *eventCollector) assertSaw(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] == 0 {
			t.Fatalf("never saw event %q", k)
		}
	}
}

func (c *eventCollector) assertSawAny(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] > 0 {
			return
		}
	}
	t.Fatalf("never saw any of %v", kinds)
}

func (c *eventCollector) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, v := range c.seen {
		n += v
	}
	return n
}

func waitForAnyEvent(t *testing.T, ch <-chan runtimeevent.RuntimeEvent, want domain.SessionID, timeout time.Duration) {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case evt, ok := <-ch:
			if !ok {
				t.Fatalf("live channel closed before any event")
			}
			if evt.SessionID == want {
				return
			}
		case <-deadline:
			t.Fatalf("timed out waiting for a live event after watermark handoff")
		}
	}
}

func lastAssistantText(messages []domain.Message) string {
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == domain.RoleAssistant {
			return strings.Join(messages[i].TextParts(), "")
		}
	}
	return ""
}

func transcriptContains(messages []domain.Message, needle string) bool {
	for _, m := range messages {
		for _, part := range m.Parts {
			if part.Kind == domain.PartText && strings.Contains(part.Text, needle) {
				return true
			}
		}
	}
	return false
}
