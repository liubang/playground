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
// Created: 2026/08/06

package e2e

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/e2e/harness"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

// TestMemoryPipelineRealModelE2E verifies the P0-A memory architecture
// end to end against a REAL LLM provider (skipped unless LOOM_E2E_LLM=1):
//
//  1. a real turn runs through the serve path;
//  2. session shutdown performs NO model work — it only enqueues a job;
//  3. the background pipeline (run manually here with zero idle window)
//     claims the job, extracts a memory with the real model, and Phase 2
//     consolidates it into MEMORY.md / memory_summary.md;
//  4. ProcessRuntime.Close returns fast — consolidation no longer runs on
//     the exit path.
func TestMemoryPipelineRealModelE2E(t *testing.T) {
	ctx := context.Background()
	env := harness.NewEnv(t, harness.WithAdjust(func(resolved *config.ResolvedConfig) {
		// Neutralize the process's own background pipeline so the test
		// drives the pass manually: a huge idle window means the startup
		// pass claims nothing, and a zero interval disables periodic reruns.
		resolved.Memory.MinSessionIdle = 100000 * time.Hour
		resolved.Memory.RunInterval = 0
	}))
	if !env.Resolved.Memory.Enabled {
		t.Skip("memory system is disabled in the loaded config")
	}
	proc := env.Proc
	svc := env.Svc
	discard := env.Logger

	c := env.NewClient(t)
	collector := harness.NewCollector(t.Context(), c, env.Subscribe(t, c))
	go collector.Run()

	// --- 1. one real turn carrying a memorable code word ---
	const codeWord = "孔雀石-3141"
	turns := collector.TurnsDone()
	if _, err := c.SubmitPrompt(ctx, "请记住这个暗号："+codeWord+"。这是长期偏好测试，之后只回复两个字：收到", nil); err != nil {
		t.Fatalf("SubmitPrompt: %v", err)
	}
	collector.WaitTurn(t, turns+1, 3*time.Minute)
	t.Log("turn ok: code word exchanged")

	// --- 2. session shutdown enqueues a memory job (no model work) ---
	shutdownStart := time.Now()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	if err := svc.Shutdown(shutdownCtx); err != nil {
		t.Logf("svc.Shutdown: %v (continuing)", err)
	}
	cancel()
	if elapsed := time.Since(shutdownStart); elapsed > 20*time.Second {
		t.Fatalf("session shutdown took %v — model work leaked back onto the exit path", elapsed)
	}

	if proc.MemoryJobQueue == nil {
		t.Fatal("MemoryJobQueue is nil; memory pipeline was not wired")
	}
	if proc.MemoryExtractor == nil || proc.MemoryConsolidator == nil {
		t.Fatal("memory extractor/consolidator not wired")
	}

	// --- 3. pipeline pass: extraction + consolidation with the real model ---
	cfg := memory.DefaultPipelineConfig()
	cfg.MinIdle = 0 // the session just closed; claim it now
	pipeline := memory.NewPipeline(proc.MemoryJobQueue, proc.Store, proc.MemoryExtractor, proc.MemoryConsolidator, cfg, discard)

	passStart := time.Now()
	stats := pipeline.RunOnce(ctx)
	t.Logf("pipeline pass: %+v in %v", stats, time.Since(passStart))
	if stats.Claimed != 1 {
		t.Fatalf("pipeline claimed %d jobs, want 1 (shutdown enqueue must land)", stats.Claimed)
	}
	if stats.Succeeded+stats.NoOutput != 1 || stats.Failed != 0 {
		t.Fatalf("pipeline stats = %+v, want the job settled without failure", stats)
	}
	if !stats.Phase2Ran {
		t.Fatalf("phase2 did not run: %+v", stats)
	}

	// --- artifacts on disk ---
	raw, err := proc.MemoryStore.ReadRaw()
	if err != nil {
		t.Fatalf("ReadRaw: %v", err)
	}
	if stats.Succeeded == 1 && strings.TrimSpace(raw) == "" {
		t.Fatal("extraction succeeded but raw_memories.md is empty")
	}
	main, err := proc.MemoryStore.ReadMain()
	if err != nil {
		t.Fatalf("ReadMain: %v", err)
	}
	summary, err := proc.MemoryStore.ReadSummary()
	if err != nil {
		t.Fatalf("ReadSummary: %v", err)
	}
	combined := raw + "\n" + main + "\n" + summary
	if stats.Succeeded == 1 && !strings.Contains(combined, codeWord) && !strings.Contains(combined, "孔雀石") {
		t.Fatalf("memory artifacts lost the code word %q:\nraw=%q\nmain=%q\nsummary=%q", codeWord, raw, main, summary)
	}
	t.Logf("memory artifacts ok: MEMORY.md=%d bytes, summary=%d bytes, raw=%d bytes", len(main), len(summary), len(raw))

	// --- 4. process close is fast (no consolidation on the exit path) ---
	closeStart := time.Now()
	env.CloseProc()
	if elapsed := time.Since(closeStart); elapsed > 10*time.Second {
		t.Fatalf("ProcessRuntime.Close took %v — exit path must be free of model work", elapsed)
	}
	t.Log("ACCEPTANCE PASS: memory pipeline e2e complete (shutdown enqueue → background extraction → consolidation)")
}
