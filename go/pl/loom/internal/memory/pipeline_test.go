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

package memory_test

import (
	"context"
	"io"
	"log/slog"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

type pipelineFixture struct {
	store     *session.SQLiteStore
	memStore  *memory.Store
	extractor *memory.Extractor
	consolid  *memory.Consolidator
	fakeModel *fakes.FakeModel
}

func newPipelineFixture(t *testing.T, script ...fakes.ScriptEntry) *pipelineFixture {
	t.Helper()
	ctx := context.Background()
	tmp := t.TempDir()

	store, err := session.OpenSQLiteStore(ctx, filepath.Join(tmp, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	memStore, err := memory.OpenStore(filepath.Join(tmp, "memories"))
	if err != nil {
		t.Fatalf("OpenStore: %v", err)
	}
	if err := memStore.InitGit(ctx); err != nil {
		t.Fatalf("InitGit: %v", err)
	}

	fakeModel := fakes.NewFakeModel(script...)
	return &pipelineFixture{
		store:     store,
		memStore:  memStore,
		extractor: memory.NewExtractor(memStore, fakeModel, "fake-extract", true),
		consolid:  memory.NewConsolidator(memStore, fakeModel, "fake-consolidate"),
		fakeModel: fakeModel,
	}
}

func (f *pipelineFixture) pipeline(cfg memory.PipelineConfig) *memory.Pipeline {
	return memory.NewPipeline(f.store, f.store, f.extractor, f.consolid, cfg,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
}

func testPipelineConfig() memory.PipelineConfig {
	cfg := memory.DefaultPipelineConfig()
	cfg.MinIdle = 0
	cfg.JobTimeout = 10 * time.Second
	cfg.ConsolidateTimeout = 10 * time.Second
	return cfg
}

func createSessionWithCheckpoint(t *testing.T, store *session.SQLiteStore, texts ...string) domain.SessionID {
	t.Helper()
	ctx := context.Background()
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	var messages []domain.Message
	for i, text := range texts {
		role := domain.RoleUser
		if i%2 == 1 {
			role = domain.RoleAssistant
		}
		messages = append(messages, domain.Message{
			ID:   domain.NewMessageID(),
			Role: role,
			Parts: []domain.ContentPart{
				{Kind: domain.PartText, Text: text},
			},
			CreatedAt: time.Now(),
		})
	}
	ckpt := domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: sessionID,
		Sequence:  0,
		Messages:  messages,
		CreatedAt: time.Now().UTC(),
	}
	if err := store.SaveCheckpoint(ctx, ckpt); err != nil {
		t.Fatalf("SaveCheckpoint: %v", err)
	}
	return sessionID
}

const extractionJSON = `{"rollout_summary": "Fixed the flaky bazel test by pinning the toolchain", "rollout_slug": "fix-flaky-bazel-test", "raw_memory": "### Task 1: fix flaky bazel test\n- User prefers table-driven tests\n- Reusable: pin the go toolchain in MODULE.bazel"}`

func TestPipelineRunOnceExtractsAndConsolidates(t *testing.T) {
	fx := newPipelineFixture(
		t,
		fakes.ScriptEntry{Text: extractionJSON},
		fakes.ScriptEntry{Text: "# Memory Handbook\n\n### User Preferences\n- prefers table-driven tests"},
		fakes.ScriptEntry{Text: "# Summary\n- table-driven tests; pin go toolchain"},
	)
	ctx := context.Background()
	sessionID := createSessionWithCheckpoint(t, fx.store,
		"the bazel test is flaky again, please look into it",
		"root cause: unpinned go toolchain; I fixed it in MODULE.bazel")

	if err := fx.store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}

	stats := fx.pipeline(testPipelineConfig()).RunOnce(ctx)
	if stats.Claimed != 1 || stats.Succeeded != 1 || stats.Failed != 0 {
		t.Fatalf("stats = %+v, want 1 claimed/1 succeeded/0 failed", stats)
	}
	if !stats.Phase2Ran || !stats.Phase2Changed {
		t.Fatalf("phase2 stats = %+v, want ran+changed", stats)
	}

	// Phase 1 artifacts.
	raw, err := fx.memStore.ReadRaw()
	if err != nil || !strings.Contains(raw, "table-driven tests") {
		t.Fatalf("raw memories = %q, err=%v", raw, err)
	}
	summary, err := fx.memStore.ReadRolloutSummary("fix-flaky-bazel-test")
	if err != nil || !strings.Contains(summary, "flaky bazel test") {
		t.Fatalf("rollout summary = %q, err=%v", summary, err)
	}

	// Phase 2 artifacts.
	main, err := fx.memStore.ReadMain()
	if err != nil || !strings.Contains(main, "Memory Handbook") {
		t.Fatalf("MEMORY.md = %q, err=%v", main, err)
	}
	hot, err := fx.memStore.ReadSummary()
	if err != nil || !strings.Contains(hot, "table-driven tests") {
		t.Fatalf("memory_summary.md = %q, err=%v", hot, err)
	}

	// The extraction request asked for structured output (P4).
	calls := fx.fakeModel.Calls()
	if len(calls) != 3 {
		t.Fatalf("model calls = %d, want 3 (extract + consolidate + summary)", len(calls))
	}
	if calls[0].ResponseFormat == nil || calls[0].ResponseFormat.Name != "memory_extraction" {
		t.Fatalf("extraction request missing response format: %+v", calls[0].ResponseFormat)
	}

	// A second pass finds no eligible job and no workspace changes.
	stats = fx.pipeline(testPipelineConfig()).RunOnce(ctx)
	if stats.Claimed != 0 {
		t.Fatalf("second pass claimed %d jobs, want 0 (already extracted)", stats.Claimed)
	}
	if stats.Phase2Changed {
		t.Fatal("second pass phase2 reported changes; want none (git baseline clean)")
	}
}

func TestPipelineNoTranscriptSettlesNoOutput(t *testing.T) {
	fx := newPipelineFixture(t) // no script: the model must never be called
	ctx := context.Background()
	sessionID := domain.NewSessionID()
	if err := fx.store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := fx.store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}

	stats := fx.pipeline(testPipelineConfig()).RunOnce(ctx)
	if stats.Claimed != 1 || stats.NoOutput != 1 || stats.Failed != 0 {
		t.Fatalf("stats = %+v, want 1 claimed/1 no_output/0 failed", stats)
	}
	if len(fx.fakeModel.Calls()) != 0 {
		t.Fatalf("model called %d times for a transcript-less session", len(fx.fakeModel.Calls()))
	}
}

func TestPipelineFailureRetriesWithBackoff(t *testing.T) {
	fx := newPipelineFixture(
		t,
		fakes.ScriptEntry{Error: "provider unavailable"},
		fakes.ScriptEntry{Text: extractionJSON},
	)
	ctx := context.Background()
	sessionID := createSessionWithCheckpoint(t, fx.store,
		"remember that I always use gofmt", "noted, always gofmt")
	if err := fx.store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}

	cfg := testPipelineConfig()
	cfg.RetryDelay = time.Millisecond // immediate retry eligibility

	stats := fx.pipeline(cfg).RunOnce(ctx)
	if stats.Claimed != 1 || stats.Failed != 1 {
		t.Fatalf("first pass stats = %+v, want 1 claimed/1 failed", stats)
	}

	// Retry succeeds; extraction lands.
	stats = fx.pipeline(cfg).RunOnce(ctx)
	if stats.Claimed != 1 || stats.Succeeded != 1 {
		t.Fatalf("retry pass stats = %+v, want 1 claimed/1 succeeded", stats)
	}
	raw, err := fx.memStore.ReadRaw()
	if err != nil || !strings.Contains(raw, "table-driven tests") {
		t.Fatalf("raw memories after retry = %q, err=%v", raw, err)
	}
}

func TestPipelineExtractionRedactsSecrets(t *testing.T) {
	fx := newPipelineFixture(
		t,
		fakes.ScriptEntry{Text: extractionJSON},
	)
	ctx := context.Background()
	sessionID := createSessionWithCheckpoint(t, fx.store,
		"debug why sk-abcdefghijklmnop1234567890 stopped working",
		"the key was rotated")
	if err := fx.store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}

	fx.pipeline(testPipelineConfig()).RunOnce(ctx)
	calls := fx.fakeModel.Calls()
	if len(calls) == 0 {
		t.Fatal("extraction model was not called")
	}
	for _, part := range calls[0].Messages[1].Parts {
		if strings.Contains(part.Text, "sk-abcdefghijklmnop1234567890") {
			t.Fatal("secret leaked into the extraction prompt")
		}
	}
}
