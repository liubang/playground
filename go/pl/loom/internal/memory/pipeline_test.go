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
	"errors"
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
	store, err := session.OpenSQLiteStore(t.Context(), filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })
	memStore := memory.NewStore(store)
	model := fakes.NewFakeModel(script...)
	return &pipelineFixture{
		store: store, memStore: memStore.ForWorkspace("/ws"),
		extractor: memory.NewExtractor(model, "fake-extract", true),
		consolid:  memory.NewConsolidator(memStore, model, "fake-consolidate"), fakeModel: model,
	}
}

func pipelineLogger() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func (f *pipelineFixture) pipeline(cfg memory.PipelineConfig) *memory.Pipeline {
	return memory.NewPipeline(f.store, f.extractor, f.consolid, cfg, pipelineLogger())
}

func testPipelineConfig() memory.PipelineConfig {
	cfg := memory.DefaultPipelineConfig()
	cfg.MinIdle = 0
	cfg.Concurrency = 1
	cfg.JobTimeout = 10 * time.Second
	cfg.ConsolidateTimeout = 10 * time.Second
	return cfg
}

func pipelineMessage(role domain.Role, text string) domain.Message {
	return domain.Message{
		ID: domain.NewMessageID(), Role: role, Status: domain.MessageStatusFinal,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: text}}, CreatedAt: time.Now().UTC(),
	}
}

// Evidence is a real durable event log, deliberately without any checkpoint.
func createSessionEvidence(t *testing.T, store *session.SQLiteStore, messages ...domain.Message) domain.SessionID {
	t.Helper()
	id := domain.NewSessionID()
	if err := store.CreateSession(t.Context(), id, domain.WorkspaceID{}); err != nil {
		t.Fatal(err)
	}
	appendSessionEvidence(t, store, id, 0, messages...)
	return id
}

func appendSessionEvidence(t *testing.T, store *session.SQLiteStore, id domain.SessionID, version int64, messages ...domain.Message) {
	t.Helper()
	var events []domain.Event
	for i, message := range messages {
		typ := domain.EventUserMessageAdded
		if message.Role == domain.RoleAssistant {
			typ = domain.EventModelResponseCompleted
		}
		payload, err := domain.MarshalPayload(domain.MessageEventPayload{Message: message})
		if err != nil {
			t.Fatal(err)
		}
		events = append(events, domain.Event{
			ID: domain.NewEventID(), Sequence: version + int64(i) + 1,
			SessionID: id, Type: typ, Timestamp: time.Now().UTC(), Payload: payload,
		})
	}
	if err := store.AppendEvents(t.Context(), id, version, events); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
}

const extractionJSON = `{"rollout_summary":"Fixed flaky bazel test", "rollout_slug":"fix-flaky-bazel-test", "raw_memory":"User prefers table-driven tests; pin the Go toolchain"}`

func extractionScript() fakes.ScriptEntry {
	return fakes.ScriptEntry{Text: extractionJSON, StopReason: domain.StopEndTurn}
}

func requireRecords(t *testing.T, store *memory.Store, count int) []memory.Record {
	t.Helper()
	records, err := store.Records(t.Context())
	if err != nil || len(records) != count {
		t.Fatalf("records=%+v err=%v, want %d", records, err, count)
	}
	return records
}

func TestPipelineRunOnceExtractsAndConsolidates(t *testing.T) {
	fx := newPipelineFixture(t, extractionScript(),
		fakes.ScriptEntry{Text: "# Memory Handbook\n- prefers table-driven tests", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "# Summary\n- table-driven tests", StopReason: domain.StopEndTurn})
	id := createSessionEvidence(t, fx.store,
		pipelineMessage(domain.RoleUser, "I prefer table-driven tests"),
		pipelineMessage(domain.RoleAssistant, "Pinned Go toolchain to fix flaky bazel test"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	stats := fx.pipeline(testPipelineConfig()).RunOnce(t.Context())
	if stats.Claimed != 1 || stats.Succeeded != 1 || stats.Failed != 0 || !stats.Phase2Ran || !stats.Phase2Changed {
		t.Fatalf("stats=%+v", stats)
	}
	records := requireRecords(t, fx.memStore, 1)
	if records[0].SessionID != id || records[0].SourceVersion != 2 || records[0].Scope != "/ws" || !strings.Contains(records[0].Content, "table-driven tests") {
		t.Fatalf("incorrect provenance/content: %+v", records[0])
	}
	calls := fx.fakeModel.Calls()
	if len(calls) != 3 || calls[0].ResponseFormat == nil || calls[0].ResponseFormat.Name != "memory_extraction" {
		t.Fatalf("unexpected model calls=%+v", calls)
	}
	stats = fx.pipeline(testPipelineConfig()).RunOnce(t.Context())
	if stats.Claimed != 0 || stats.Phase2Changed || len(fx.fakeModel.Calls()) != 3 {
		t.Fatalf("second pass repeated work: %+v, calls=%d", stats, len(fx.fakeModel.Calls()))
	}
	requireRecords(t, fx.memStore, 1)
}

func TestPipelinePhase2StatsReflectPartialScopePublication(t *testing.T) {
	for _, healthyScope := range []string{"/a-healthy", "/z-healthy"} {
		t.Run(healthyScope, func(t *testing.T) {
			fx := newPipelineFixture(t, extractionScript(), fakes.ScriptEntry{Error: "consolidation provider unavailable"})
			healthyStore := fx.memStore.ForWorkspace(healthyScope)
			if _, err := healthyStore.Mutate(t.Context(), memory.Mutation{
				RequestID: "healthy-explicit", Scope: healthyScope, Content: "prefer table-driven tests",
			}); err != nil {
				t.Fatal(err)
			}
			id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember my preference"))
			if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
				t.Fatal(err)
			}
			// MemoryScopes orders by scope in SQL; exercise publication both
			// before and after the failing automatic-memory scope.
			var logs strings.Builder
			logger := slog.New(slog.NewTextHandler(&logs, nil))
			pipeline := memory.NewPipeline(fx.store, fx.extractor, fx.consolid, testPipelineConfig(), logger)
			stats := pipeline.RunOnce(t.Context())
			if stats.Claimed != 1 || stats.Succeeded != 1 || stats.Failed != 0 || !stats.Phase2Ran || !stats.Phase2Changed || stats.Phase2Skipped {
				t.Fatalf("partial publication misreported: %+v", stats)
			}
			healthy, err := fx.store.MemorySnapshot(t.Context(), healthyScope)
			if err != nil {
				t.Fatal(err)
			}
			if !healthy.ValidView() || healthy.View.Version != healthy.Version || healthy.View.Main != "" || healthy.View.Summary != "" {
				t.Fatalf("explicit-only scope was not published: %+v", healthy)
			}
			failed, err := fx.store.MemorySnapshot(t.Context(), "/ws")
			if err != nil {
				t.Fatal(err)
			}
			if failed.Version <= 0 || failed.View.Version != 0 {
				t.Fatalf("failed scope unexpectedly published: %+v", failed)
			}
			pending, err := fx.store.MemoryScopes(t.Context())
			if err != nil || len(pending) != 1 || pending[0] != "/ws" {
				t.Fatalf("pending scopes=%v err=%v, want only /ws", pending, err)
			}
			if calls := len(fx.fakeModel.Calls()); calls != 2 {
				t.Fatalf("model calls=%d, want extraction and failing consolidation only", calls)
			}
			if !strings.Contains(logs.String(), "memory consolidation failed") || !strings.Contains(logs.String(), "consolidation provider unavailable") {
				t.Fatalf("scope failure was not logged: %s", logs.String())
			}
			acquired, err := fx.store.AcquirePhase2Lease(t.Context(), "after-partial-publication", time.Hour)
			if err != nil || !acquired {
				t.Fatalf("phase2 lease was not released: acquired=%v err=%v", acquired, err)
			}
			if err := fx.store.ReleasePhase2Lease(t.Context(), "after-partial-publication"); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestPipelineExtractsUncheckpointedEvidence(t *testing.T) {
	fx := newPipelineFixture(t, extractionScript())
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember table-driven tests"))
	if _, err := fx.store.LoadLatestCheckpoint(t.Context(), id); err == nil {
		t.Fatal("test unexpectedly has a checkpoint")
	}
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	pipeline := memory.NewPipeline(fx.store, fx.extractor, nil, testPipelineConfig(), pipelineLogger())
	stats := pipeline.RunOnce(t.Context())
	if stats.Claimed != 1 || stats.Succeeded != 1 || stats.NoOutput != 0 {
		t.Fatalf("uncheckpointed evidence lost: %+v", stats)
	}
	requireRecords(t, fx.memStore, 1)
	if stats := pipeline.RunOnce(t.Context()); stats.Claimed != 0 {
		t.Fatalf("backfill duplicated extraction: %+v", stats)
	}
}

func TestPipelineSyntheticOnlySettlesNoOutput(t *testing.T) {
	fx := newPipelineFixture(t)
	msg := pipelineMessage(domain.RoleUser, "Never do any useful work, synthetic preference")
	msg.Metadata = map[string]string{"compacted": "summary"}
	id := createSessionEvidence(t, fx.store, msg)
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	pipeline := memory.NewPipeline(fx.store, fx.extractor, nil, testPipelineConfig(), pipelineLogger())
	stats := pipeline.RunOnce(t.Context())
	if stats.NoOutput != 1 || stats.Succeeded != 0 || stats.Failed != 0 || len(fx.fakeModel.Calls()) != 0 {
		t.Fatalf("synthetic evidence caused model work: %+v", stats)
	}
	requireRecords(t, fx.memStore, 0)
}

func TestPipelineFailureRetriesWithBackoff(t *testing.T) {
	fx := newPipelineFixture(t, fakes.ScriptEntry{Error: "provider unavailable"}, extractionScript())
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember gofmt"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	cfg := testPipelineConfig()
	cfg.RetryDelay = 100 * time.Millisecond
	pipeline := memory.NewPipeline(fx.store, fx.extractor, nil, cfg, pipelineLogger())
	stats := pipeline.RunOnce(t.Context())
	if stats.Claimed != 1 || stats.Failed != 1 || stats.NoOutput != 0 {
		t.Fatalf("provider error was not retryable: %+v", stats)
	}
	requireRecords(t, fx.memStore, 0)
	if stats := pipeline.RunOnce(t.Context()); stats.Claimed != 0 {
		t.Fatalf("backfill reset failure backoff: %+v", stats)
	}
	time.Sleep(120 * time.Millisecond)
	stats = pipeline.RunOnce(t.Context())
	if stats.Claimed != 1 || stats.Succeeded != 1 {
		t.Fatalf("retry failed: %+v", stats)
	}
	requireRecords(t, fx.memStore, 1)
}

func TestPipelineExtractionRedactsAndExcludesSynthetic(t *testing.T) {
	const secret = "sk-abcdefghijklmnop1234567890"
	fx := newPipelineFixture(t, fakes.ScriptEntry{Text: `{"raw_memory":"remember ` + secret + `", "rollout_summary":"summary ` + secret + `", "rollout_slug":"label"}`, StopReason: domain.StopEndTurn})
	synthetic := pipelineMessage(domain.RoleUser, "synthetic_wrapup_should_not_appear")
	synthetic.Metadata = map[string]string{"kind": "budget_wrapup"}
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "real preference "+secret), synthetic)
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	pipeline := memory.NewPipeline(fx.store, fx.extractor, nil, testPipelineConfig(), pipelineLogger())
	if stats := pipeline.RunOnce(t.Context()); stats.Succeeded != 1 {
		t.Fatalf("stats=%+v", stats)
	}
	for _, msg := range fx.fakeModel.Calls()[0].Messages {
		text := strings.Join(msg.TextParts(), "")
		if strings.Contains(text, secret) || strings.Contains(text, "synthetic_wrapup_should_not_appear") {
			t.Fatal("secret/synthetic input leaked to extraction")
		}
	}
	for _, record := range requireRecords(t, fx.memStore, 1) {
		if strings.Contains(record.Content+record.Rollout, secret) {
			t.Fatal("secret persisted in record")
		}
	}
}

type blockingPipelineModel struct {
	entered  chan context.Context
	returned chan struct{}
}

func (m *blockingPipelineModel) Stream(ctx context.Context, _ domain.ModelRequest) (domain.ModelStream, error) {
	m.entered <- ctx
	<-ctx.Done()
	close(m.returned)
	return nil, ctx.Err()
}

func TestPipelineStartReturnsWithoutCallerModelWorkAndStopsOnExit(t *testing.T) {
	fx := newPipelineFixture(t)
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember my preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	model := &blockingPipelineModel{entered: make(chan context.Context, 1), returned: make(chan struct{})}
	queue := &settlementSignalQueue{JobQueue: fx.store, settled: make(chan error, 1)}
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	pipeline := memory.NewPipeline(queue, memory.NewExtractor(model, "model", false), nil, testPipelineConfig(), pipelineLogger())
	started := make(chan struct{})
	go func() { pipeline.Start(ctx, 0); close(started) }()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("Start blocked on a model call")
	}
	select {
	case modelCtx := <-model.entered:
		if modelCtx == ctx {
			t.Fatal("model did not get a bounded worker context")
		}
	case <-time.After(time.Second):
		t.Fatal("background extraction never started")
	}
	cancel()
	select {
	case <-model.returned:
	case <-time.After(time.Second):
		t.Fatal("model call survived worker cancellation")
	}
	// Shutdown must not settle the claim or charge a failure. The synchronous
	// cancellation regression below also checks the retained claim and budget.
	select {
	case err := <-queue.settled:
		t.Fatalf("worker settled cancellation: %v", err)
	default:
	}
	jobs, err := fx.store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1)
	if err != nil || len(jobs) != 0 {
		t.Fatalf("shutdown released the active lease: jobs=%v err=%v", jobs, err)
	}
	requireRecords(t, fx.memStore, 0)
}

type settlementSignalQueue struct {
	memory.JobQueue
	settled chan error
}

func (q *settlementSignalQueue) FailMemoryJob(ctx context.Context, job memory.Job, jobErr error, delay time.Duration, attempts int) error {
	err := q.JobQueue.FailMemoryJob(ctx, job, jobErr, delay, attempts)
	q.settled <- err
	return err
}

type backfillOrderQueue struct {
	memory.JobQueue
	backfilled           bool
	claimedAfterBackfill bool
}

func (q *backfillOrderQueue) BackfillMemoryJobs(ctx context.Context, maxAge time.Duration) error {
	q.backfilled = true
	return q.JobQueue.BackfillMemoryJobs(ctx, maxAge)
}

func (q *backfillOrderQueue) ClaimMemoryJobs(ctx context.Context, limit int, minIdle, maxAge, lease time.Duration, attempts int) ([]memory.Job, error) {
	q.claimedAfterBackfill = q.backfilled
	return q.JobQueue.ClaimMemoryJobs(ctx, limit, minIdle, maxAge, lease, attempts)
}

func TestPipelineBackfillsBeforeClaiming(t *testing.T) {
	fx := newPipelineFixture(t)
	queue := &backfillOrderQueue{JobQueue: fx.store}
	memory.NewPipeline(queue, fx.extractor, nil, testPipelineConfig(), pipelineLogger()).RunOnce(t.Context())
	if !queue.claimedAfterBackfill {
		t.Fatal("claims ran before backfill")
	}
}

type callbackPipelineModel struct {
	model      domain.Model
	beforeCall func() error
}

func (m callbackPipelineModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	if err := m.beforeCall(); err != nil {
		return nil, err
	}
	return m.model.Stream(ctx, req)
}

func TestPipelineVersionChangeDuringModelPublishesNothing(t *testing.T) {
	fx := newPipelineFixture(t, extractionScript())
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "old evidence"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	model := callbackPipelineModel{model: fx.fakeModel, beforeCall: func() error {
		// A write while the model is running must not wait on an evidence
		// read transaction. It also invalidates the fixed claimed version.
		payload, err := domain.MarshalPayload(domain.MessageEventPayload{Message: pipelineMessage(domain.RoleUser, "new evidence")})
		if err != nil {
			return err
		}
		return fx.store.AppendEvents(t.Context(), id, 1, []domain.Event{{
			ID: domain.NewEventID(), Sequence: 2, SessionID: id,
			Type: domain.EventUserMessageAdded, Timestamp: time.Now().UTC(), Payload: payload,
		}})
	}}
	pipeline := memory.NewPipeline(fx.store, memory.NewExtractor(model, "model", false), nil, testPipelineConfig(), pipelineLogger())
	stats := pipeline.RunOnce(t.Context())
	if stats.Failed != 0 || stats.Conflicted != 1 || stats.Succeeded != 0 || stats.NoOutput != 0 {
		t.Fatalf("stale evidence published or charged as model failure: %+v", stats)
	}
	requireRecords(t, fx.memStore, 0)
	text := strings.Join(fx.fakeModel.Calls()[0].Messages[1].TextParts(), "")
	if !strings.Contains(text, "old evidence") || strings.Contains(text, "new evidence") {
		t.Fatal("model evidence was not fixed at claim time")
	}
}

type conflictEvidenceQueue struct {
	memory.JobQueue
}

func (q conflictEvidenceQueue) LoadMemoryEvidence(context.Context, memory.Job) ([]domain.Message, error) {
	return nil, domain.NewError(domain.ErrConflict, "evidence cutoff changed")
}

func TestPipelineRepeatedEvidenceConflictNeverAbandons(t *testing.T) {
	fx := newPipelineFixture(t)
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	cfg := testPipelineConfig()
	cfg.MaxAttempts = 1
	cfg.RetryDelay = 0
	pipeline := memory.NewPipeline(conflictEvidenceQueue{fx.store}, fx.extractor, nil, cfg, pipelineLogger())
	for range 8 {
		stats := pipeline.RunOnce(t.Context())
		if stats.Claimed != 1 || stats.Conflicted != 1 || stats.Failed != 0 || stats.NoOutput != 0 {
			t.Fatalf("evidence conflict exhausted retries or was a model failure: %+v", stats)
		}
	}
	if len(fx.fakeModel.Calls()) != 0 {
		t.Fatal("conflicting evidence reached model")
	}
	requireRecords(t, fx.memStore, 0)
}

func TestPipelineProviderFailureWithVersionChangeIsConflict(t *testing.T) {
	fx := newPipelineFixture(t, fakes.ScriptEntry{Error: "provider unavailable"})
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "old preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	model := callbackPipelineModel{model: fx.fakeModel, beforeCall: func() error {
		payload, err := domain.MarshalPayload(domain.MessageEventPayload{Message: pipelineMessage(domain.RoleUser, "new preference")})
		if err != nil {
			return err
		}
		return fx.store.AppendEvents(t.Context(), id, 1, []domain.Event{{ID: domain.NewEventID(), Sequence: 2, SessionID: id, Type: domain.EventUserMessageAdded, Timestamp: time.Now().UTC(), Payload: payload}})
	}}
	cfg := testPipelineConfig()
	cfg.MaxAttempts = 1
	cfg.RetryDelay = 0
	stats := memory.NewPipeline(fx.store, memory.NewExtractor(model, "model", false), nil, cfg, pipelineLogger()).RunOnce(t.Context())
	if stats.Conflicted != 1 || stats.Failed != 0 {
		t.Fatalf("stale provider failure consumed budget: %+v", stats)
	}
	jobs, err := fx.store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1)
	if err != nil || len(jobs) != 1 || jobs[0].SessionVersion != 2 {
		t.Fatalf("new version was abandoned: jobs=%v err=%v", jobs, err)
	}
	requireRecords(t, fx.memStore, 0)
}

func TestPipelineIncrementalEvidenceDoesNotReextractOldMessages(t *testing.T) {
	fx := newPipelineFixture(t, extractionScript(), extractionScript())
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "first preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	pipeline := memory.NewPipeline(fx.store, fx.extractor, nil, testPipelineConfig(), pipelineLogger())
	if stats := pipeline.RunOnce(t.Context()); stats.Succeeded != 1 {
		t.Fatalf("first stats=%+v", stats)
	}
	appendSessionEvidence(t, fx.store, id, 1, pipelineMessage(domain.RoleUser, "second preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	if stats := pipeline.RunOnce(t.Context()); stats.Succeeded != 1 {
		t.Fatalf("second stats=%+v", stats)
	}
	calls := fx.fakeModel.Calls()
	text := strings.Join(calls[1].Messages[1].TextParts(), "")
	if strings.Contains(text, "first preference") || !strings.Contains(text, "second preference") {
		t.Fatalf("increment reused old evidence: %s", text)
	}
	requireRecords(t, fx.memStore, 2)
}

type cancellingEvidenceQueue struct {
	memory.JobQueue
	cancel context.CancelFunc
}

func (q cancellingEvidenceQueue) LoadMemoryEvidence(ctx context.Context, job memory.Job) ([]domain.Message, error) {
	messages, err := q.JobQueue.LoadMemoryEvidence(ctx, job)
	q.cancel()
	return messages, err
}

func TestPipelineCancellationCannotBecomeNoOutput(t *testing.T) {
	fx := newPipelineFixture(t)
	msg := pipelineMessage(domain.RoleUser, "synthetic")
	msg.Metadata = map[string]string{"kind": "budget_wrapup"}
	id := createSessionEvidence(t, fx.store, msg)
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	queue := cancellingEvidenceQueue{JobQueue: fx.store, cancel: cancel}
	stats := memory.NewPipeline(queue, fx.extractor, nil, testPipelineConfig(), pipelineLogger()).RunOnce(ctx)
	if stats.Failed != 0 || stats.Conflicted != 1 || stats.NoOutput != 0 || len(fx.fakeModel.Calls()) != 0 {
		t.Fatalf("cancelled evidence read misreported failure/no-output: %+v", stats)
	}
	requireRecords(t, fx.memStore, 0)
}

func TestPipelineRepeatedCancellationPreservesClaimAndRetryBudget(t *testing.T) {
	for _, phase := range []string{"evidence", "model error", "model completion"} {
		t.Run(phase, func(t *testing.T) {
			fx := newPipelineFixture(t)
			id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember preference"))
			if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
				t.Fatal(err)
			}
			cfg := testPipelineConfig()
			cfg.MaxAttempts = 1
			// Reclaim an expired lease immediately on the next pass; separately
			// probe with a full lease to verify cancellation did not release it.
			cfg.Lease = 0
			for range 8 {
				ctx, cancel := context.WithCancel(t.Context())
				queue := &settlementSignalQueue{JobQueue: fx.store, settled: make(chan error, 1)}
				var jobs memory.JobQueue = queue
				model := callbackPipelineModel{model: fakes.NewFakeModel(extractionScript()), beforeCall: func() error {
					cancel()
					if phase == "model error" {
						return context.Canceled
					}
					return nil
				}}
				if phase == "evidence" {
					jobs = cancellingEvidenceQueue{JobQueue: queue, cancel: cancel}
				}
				stats := memory.NewPipeline(jobs, memory.NewExtractor(model, "model", false), nil, cfg, pipelineLogger()).RunOnce(ctx)
				cancel()
				if stats.Claimed != 1 || stats.Conflicted != 1 || stats.Failed != 0 || stats.Succeeded != 0 || stats.NoOutput != 0 {
					t.Fatalf("interruption consumed retry budget or published: %+v", stats)
				}
				select {
				case err := <-queue.settled:
					t.Fatalf("cancellation charged a failure: %v", err)
				default:
				}
				claimed, err := fx.store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1)
				if err != nil || len(claimed) != 0 {
					t.Fatalf("cancellation released claim: jobs=%v err=%v", claimed, err)
				}
			}
			// With MaxAttempts=1, this can succeed only if all cancellations left
			// attempts at zero for the same unchanged evidence version.
			pipeline := memory.NewPipeline(fx.store, memory.NewExtractor(fakes.NewFakeModel(extractionScript()), "model", false), nil, cfg, pipelineLogger())
			if stats := pipeline.RunOnce(t.Context()); stats.Succeeded != 1 || stats.Failed != 0 {
				t.Fatalf("cancelled attempts exhausted retry budget: %+v", stats)
			}
			requireRecords(t, fx.memStore, 1)
		})
	}
}

func TestPipelineJobTimeoutChargesRetryBudget(t *testing.T) {
	fx := newPipelineFixture(t)
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember preference"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	cfg := testPipelineConfig()
	cfg.JobTimeout = 100 * time.Millisecond
	cfg.MaxAttempts = 1
	cfg.RetryDelay = 0
	model := &blockingPipelineModel{entered: make(chan context.Context, 1), returned: make(chan struct{})}
	queue := &settlementSignalQueue{JobQueue: fx.store, settled: make(chan error, 1)}
	pipeline := memory.NewPipeline(queue, memory.NewExtractor(model, "model", false), nil, cfg, pipelineLogger())
	ctx := t.Context()
	stats := pipeline.RunOnce(ctx)
	if ctx.Err() != nil || stats.Claimed != 1 || stats.Failed != 1 || stats.Conflicted != 0 || stats.NoOutput != 0 {
		t.Fatalf("job timeout was not a budgeted failure: stats=%+v parent=%v", stats, ctx.Err())
	}
	select {
	case modelCtx := <-model.entered:
		if !errors.Is(modelCtx.Err(), context.DeadlineExceeded) {
			t.Fatalf("model did not reach its job deadline: %v", modelCtx.Err())
		}
	default:
		t.Fatal("timeout occurred before the model call")
	}
	select {
	case err := <-queue.settled:
		if err != nil {
			t.Fatalf("timeout settlement: %v", err)
		}
	default:
		t.Fatal("job timeout did not settle failure")
	}
	if stats := pipeline.RunOnce(ctx); stats.Claimed != 0 {
		t.Fatalf("job timeout did not exhaust MaxAttempts=1: %+v", stats)
	}
	requireRecords(t, fx.memStore, 0)
}

type publicationFailureQueue struct {
	memory.JobQueue
	failures int
}

func (q *publicationFailureQueue) CompleteMemoryExtraction(context.Context, memory.Job, *memory.ExtractionResult) error {
	return errors.New("publication transaction failed")
}

func (q *publicationFailureQueue) FailMemoryJob(ctx context.Context, job memory.Job, err error, delay time.Duration, attempts int) error {
	q.failures++
	return q.JobQueue.FailMemoryJob(ctx, job, err, delay, attempts)
}

func TestPipelinePublicationFailureBacksOff(t *testing.T) {
	fx := newPipelineFixture(t, extractionScript())
	id := createSessionEvidence(t, fx.store, pipelineMessage(domain.RoleUser, "remember table-driven tests"))
	if err := fx.store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	queue := &publicationFailureQueue{JobQueue: fx.store}
	pipeline := memory.NewPipeline(queue, fx.extractor, nil, testPipelineConfig(), pipelineLogger())
	if stats := pipeline.RunOnce(t.Context()); stats.Failed != 1 || stats.Succeeded != 0 || queue.failures != 1 {
		t.Fatalf("publication failure misreported: %+v, queue=%+v", stats, queue)
	}
	requireRecords(t, fx.memStore, 0)
	if stats := pipeline.RunOnce(t.Context()); stats.Claimed != 0 {
		t.Fatalf("publication failure had no backoff: %+v", stats)
	}
}
