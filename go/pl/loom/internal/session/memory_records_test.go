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

package session

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

func mutateTestMemory(t *testing.T, s *SQLiteStore, m memory.Mutation) memory.Record {
	t.Helper()
	r, err := s.MutateMemory(t.Context(), m)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func snapshotTestMemory(t *testing.T, s *SQLiteStore, scope string) memory.Snapshot {
	t.Helper()
	snapshot, err := s.MemorySnapshot(t.Context(), scope)
	if err != nil {
		t.Fatal(err)
	}
	return snapshot
}

func TestMemoryMutationIdempotencyAndRevision(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	m := memory.Mutation{RequestID: "tool-1", Scope: "/ws", Content: "old private content"}
	r := mutateTestMemory(t, s, m)
	again := mutateTestMemory(t, s, m)
	if r != again || !strings.HasPrefix(r.ID, "mem_") {
		t.Fatalf("unstable receipt: %+v %+v", r, again)
	}
	m.Content = "changed"
	if _, err := s.MutateMemory(ctx, m); errorCode(err) != domain.ErrConflict {
		t.Fatalf("request collision = %v", err)
	}
	for _, scope := range []string{"", "/wrong"} {
		if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "wrong" + scope, ID: r.ID, Scope: scope, Revision: r.Revision, Content: "replace"}); errorCode(err) != domain.ErrInvalidInput {
			t.Fatalf("scope isolation = %v", err)
		}
	}
	updated := mutateTestMemory(t, s, memory.Mutation{RequestID: "update", ID: r.ID, Scope: r.Scope, Revision: 1, Content: "new content"})
	if updated.Revision != 2 || updated.Sequence <= r.Sequence || updated.Kind != memory.Explicit {
		t.Fatalf("update = %+v", updated)
	}
	if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "tool-1", Scope: r.Scope, Content: "old private content"}); errorCode(err) != domain.ErrConflict {
		t.Fatalf("superseded remember reported success: %v", err)
	}
	if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "stale", ID: r.ID, Scope: r.Scope, Revision: 1, Content: "stale"}); errorCode(err) != domain.ErrConflict {
		t.Fatalf("revision conflict = %v", err)
	}
	forgotten := mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", ID: r.ID, Scope: r.Scope, Revision: 2, Forget: true})
	if forgotten.Content != "" || len(snapshotTestMemory(t, s, r.Scope).Records) != 0 {
		t.Fatal("forget retained content")
	}
	if _, err := s.MutateMemory(ctx, m); errorCode(err) != domain.ErrConflict {
		t.Fatalf("forgotten request ID reused with different payload: %v", err)
	}
	againForget := mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", ID: r.ID, Scope: r.Scope, Revision: 2, Forget: true})
	if againForget != forgotten {
		t.Fatalf("forget receipt changed: %+v %+v", againForget, forgotten)
	}
	var count int
	if err := s.db.QueryRow(`SELECT count(*) FROM memory_requests WHERE CAST(response AS TEXT) LIKE '%old private content%' OR CAST(response AS TEXT) LIKE '%new content%'`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("full-text history count=%d err=%v", count, err)
	}
	if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "missing", ID: r.ID, Scope: r.Scope, Revision: 3, Forget: true}); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("missing = %v", err)
	}
}

func TestMemoryUpdatePromotesAutomaticRecord(t *testing.T) {
	for _, scope := range []string{"", "/a"} {
		for _, content := range []string{"automatic fact", "corrected fact"} {
			t.Run(fmt.Sprintf("scope=%q/content=%q", scope, content), func(t *testing.T) {
				s := openMemoryJobTestStore(t)
				ctx := t.Context()
				keep := mutateTestMemory(t, s, memory.Mutation{RequestID: "keep", Scope: "/b", Content: "keep explicit"})
				var target memory.Record
				before := make(map[string]memory.Snapshot)
				for _, root := range []string{"", "/a", "/b"} {
					for range 2 {
						id := createMemoryJobSession(t, s)
						job := claimTestMemoryJob(t, s, id, root)
						if err := s.CompleteMemoryExtraction(ctx, job, &memory.ExtractionResult{RawMemory: "automatic fact", RolloutSummary: "old rollout"}); err != nil {
							t.Fatal(err)
						}
					}
					snapshot := snapshotTestMemory(t, s, root)
					if root == scope {
						target = snapshot.Records[0]
					}
					if ok, err := s.PublishMemoryView(ctx, snapshot, "old view", "old summary"); err != nil || !ok {
						t.Fatalf("publish=%v %v", ok, err)
					}
					before[root] = snapshotTestMemory(t, s, root)
				}
				mutation := memory.Mutation{RequestID: "promote", Scope: scope, ID: target.ID, Revision: target.Revision, Content: content}
				promoted := mutateTestMemory(t, s, mutation)
				if promoted.ID != target.ID || promoted.Kind != memory.Explicit || promoted.Content != content || promoted.Rollout != "" || promoted.Revision != target.Revision+1 || promoted.Sequence <= target.Sequence || !promoted.SessionID.IsZero() || promoted.SourceVersion != 0 {
					t.Fatalf("promotion=%+v target=%+v", promoted, target)
				}
				stored, err := s.ReadMemory(ctx, "/a", target.ID)
				if err != nil || stored != promoted {
					t.Fatalf("successful promotion not persisted: %+v %v", stored, err)
				}
				if replay := mutateTestMemory(t, s, mutation); replay != promoted {
					t.Fatalf("promotion receipt changed: %+v", replay)
				}
				for _, root := range []string{"", "/a", "/b"} {
					after := snapshotTestMemory(t, s, root)
					automatic := 0
					for _, record := range after.Records {
						if record.Kind == memory.Automatic {
							automatic++
						}
					}
					if scope == "" || root == scope {
						if automatic != 0 || after.ValidView() || after.Epoch <= before[root].Epoch {
							t.Fatalf("scope %q retained automatic evidence: %+v", root, after)
						}
						if ok, err := s.PublishMemoryView(ctx, before[root], "stale", "stale"); err != nil || ok {
							t.Fatalf("scope %q stale publication=%v %v", root, ok, err)
						}
					} else if automatic != 2 || after.View != before[root].View || after.Version != before[root].Version || after.Epoch != before[root].Epoch {
						t.Fatalf("unrelated scope %q changed: %+v", root, after)
					}
				}
				if stored, err := s.ReadMemory(ctx, "/b", keep.ID); err != nil || stored != keep {
					t.Fatalf("unrelated explicit record changed: %+v %v", stored, err)
				}
				if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "stale", Scope: scope, ID: target.ID, Revision: target.Revision, Content: "stale"}); errorCode(err) != domain.ErrConflict {
					t.Fatalf("stale promotion accepted: %v", err)
				}
				mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", Scope: scope, ID: promoted.ID, Revision: promoted.Revision, Forget: true})
				if _, err := s.ReadMemory(ctx, "/a", promoted.ID); errorCode(err) != domain.ErrInvalidInput {
					t.Fatalf("forgotten promotion survived: %v", err)
				}
				if _, err := s.MutateMemory(ctx, mutation); errorCode(err) != domain.ErrConflict {
					t.Fatalf("forgotten promotion receipt reported success: %v", err)
				}
			})
		}
	}
}

func TestMemoryRememberDeduplicatesExactContent(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	m := memory.Mutation{RequestID: "original", Scope: "/ws", Content: "  prefer Go\n"}
	r := mutateTestMemory(t, s, m)
	before := snapshotTestMemory(t, s, m.Scope)
	if ok, err := s.PublishMemoryView(ctx, before, "view", "summary"); err != nil || !ok {
		t.Fatalf("publish=%v err=%v", ok, err)
	}
	before = snapshotTestMemory(t, s, m.Scope)
	duplicate := memory.Mutation{RequestID: "duplicate", Scope: m.Scope, Content: "prefer Go"}
	if got := mutateTestMemory(t, s, duplicate); got != r {
		t.Fatalf("duplicate changed authoritative record: %+v want %+v", got, r)
	}
	after := snapshotTestMemory(t, s, m.Scope)
	if after.Version != before.Version || after.Epoch != before.Epoch || after.View != before.View || len(after.Records) != 1 {
		t.Fatalf("no-op remember changed scope/view: %+v", after)
	}
	if got := mutateTestMemory(t, s, duplicate); got != r {
		t.Fatalf("duplicate receipt changed: %+v", got)
	}
	if got := mutateTestMemory(t, s, memory.Mutation{RequestID: "same-update", ID: r.ID, Revision: r.Revision, Scope: r.Scope, Content: r.Content}); got != r {
		t.Fatalf("same-content update changed record: %+v", got)
	}
	unchanged := snapshotTestMemory(t, s, m.Scope)
	if unchanged.Version != before.Version || unchanged.Epoch != before.Epoch || unchanged.View != before.View {
		t.Fatalf("same-content update revoked evidence: %+v", unchanged)
	}
	duplicate.Content = "different payload"
	if _, err := s.MutateMemory(ctx, duplicate); errorCode(err) != domain.ErrConflict {
		t.Fatalf("duplicate request collision=%v", err)
	}
	for i, content := range []string{"prefer go", "prefer  Go", "prefer Rust"} {
		got := mutateTestMemory(t, s, memory.Mutation{RequestID: fmt.Sprintf("distinct-%d", i), Scope: m.Scope, Content: content})
		if got.ID == r.ID {
			t.Fatalf("distinct fact collapsed: %q", content)
		}
	}
	other := mutateTestMemory(t, s, memory.Mutation{RequestID: "other-scope", Scope: "/other", Content: r.Content})
	if other.ID == r.ID {
		t.Fatal("deduplication crossed scope boundary")
	}
	mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", Scope: r.Scope, ID: r.ID, Revision: r.Revision, Forget: true})
	duplicate.Content = r.Content
	if _, err := s.MutateMemory(ctx, duplicate); errorCode(err) != domain.ErrConflict {
		t.Fatalf("forgotten duplicate receipt reported success: %v", err)
	}
	var retained int
	if err := s.db.QueryRow("SELECT count(*) FROM memory_requests WHERE record_id = ? AND json_extract(response, '$.content') <> ''", r.ID).Scan(&retained); err != nil || retained != 0 {
		t.Fatalf("duplicate receipts leaked forgotten text: %d %v", retained, err)
	}
	fresh := mutateTestMemory(t, s, memory.Mutation{RequestID: "fresh", Scope: r.Scope, Content: r.Content})
	if fresh.ID == r.ID {
		t.Fatal("fresh remember reused forgotten record")
	}
}

func TestMemoryConcurrentIdenticalContentAcrossStores(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shared.db")
	a := openTestSQLiteStore(t, path)
	b := openTestSQLiteStore(t, path)
	var wg sync.WaitGroup
	results := make(chan memory.Record, 2)
	errs := make(chan error, 2)
	for i, store := range []*SQLiteStore{a, b} {
		wg.Go(func() {
			r, err := store.MutateMemory(t.Context(), memory.Mutation{RequestID: fmt.Sprintf("request-%d", i), Scope: "/ws", Content: "shared fact"})
			results <- r
			errs <- err
		})
	}
	wg.Wait()
	for range 2 {
		if err := <-errs; err != nil {
			t.Fatal(err)
		}
	}
	if first, second := <-results, <-results; first != second {
		t.Fatalf("distinct requests duplicated content: %+v %+v", first, second)
	}
	snapshot := snapshotTestMemory(t, a, "/ws")
	var receipts int
	if err := a.db.QueryRow("SELECT count(*) FROM memory_requests").Scan(&receipts); err != nil || receipts != 2 || snapshot.Version != 1 || len(snapshot.Records) != 1 {
		t.Fatalf("records=%+v receipts=%d err=%v", snapshot, receipts, err)
	}
}

func TestMemoryViewFencesAndExplicitAdditionInvalidation(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	empty := snapshotTestMemory(t, s, "missing")
	if empty.Version != 0 || empty.Epoch != 0 || len(empty.Records) != 0 || empty.ValidView() {
		t.Fatalf("empty scope=%+v", empty)
	}
	if ok, err := s.PublishMemoryView(ctx, empty, "empty", "empty"); err != nil || ok {
		t.Fatalf("empty publication=%v %v", ok, err)
	}
	r := mutateTestMemory(t, s, memory.Mutation{RequestID: "a", Content: "first"})
	first := snapshotTestMemory(t, s, "")
	if ok, err := s.PublishMemoryView(ctx, first, "first only", "first"); err != nil || !ok {
		t.Fatalf("publish initial view: %v %v", ok, err)
	}
	mutateTestMemory(t, s, memory.Mutation{RequestID: "b", Content: "second"})
	ok, err := s.PublishMemoryView(ctx, first, "first only", "first")
	if err != nil || ok {
		t.Fatalf("old snapshot published across explicit addition: %v %v", ok, err)
	}
	second := snapshotTestMemory(t, s, "")
	if second.ValidView() || second.View != (memory.View{}) || second.Version != first.Version+1 || second.Epoch != first.Epoch+1 || len(second.Records) != 2 {
		t.Fatalf("explicit addition did not invalidate view while preserving records: %+v", second)
	}
	scopes, err := s.MemoryScopes(ctx)
	if err != nil || len(scopes) != 1 || scopes[0] != "" {
		t.Fatalf("pending scopes=%v err=%v", scopes, err)
	}
	if ok, err := s.PublishMemoryView(ctx, second, "both", "both"); err != nil || !ok {
		t.Fatalf("publish latest: %v %v", ok, err)
	}
	if ok, err := s.PublishMemoryView(ctx, first, "stale", "stale"); err != nil || ok {
		t.Fatalf("older view published: %v %v", ok, err)
	}
	scopes, err = s.MemoryScopes(ctx)
	if err != nil || len(scopes) != 0 {
		t.Fatalf("up-to-date scopes=%v %v", scopes, err)
	}
	mutateTestMemory(t, s, memory.Mutation{RequestID: "replace", ID: r.ID, Revision: r.Revision, Content: "replacement"})
	after := snapshotTestMemory(t, s, "")
	if after.Epoch <= second.Epoch || after.ValidView() || after.View.Main != "" {
		t.Fatalf("stale view retained: %+v", after)
	}
	if ok, err := s.PublishMemoryView(ctx, second, "revoked", "revoked"); err != nil || ok {
		t.Fatalf("revoked view published: %v %v", ok, err)
	}
}

func TestMemoryConcurrentRequestAcrossStores(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shared.db")
	a := openTestSQLiteStore(t, path)
	b := openTestSQLiteStore(t, path)
	var wg sync.WaitGroup
	results := make(chan memory.Record, 2)
	errs := make(chan error, 2)
	for _, store := range []*SQLiteStore{a, b} {
		wg.Go(func() {
			r, err := store.MutateMemory(t.Context(), memory.Mutation{RequestID: "same", Content: "once"})
			results <- r
			errs <- err
		})
	}
	wg.Wait()
	for range 2 {
		if err := <-errs; err != nil {
			t.Fatal(err)
		}
	}
	if first, second := <-results, <-results; first != second {
		t.Fatalf("receipts differ: %+v %+v", first, second)
	}
	snapshot := snapshotTestMemory(t, a, "")
	if snapshot.Version != 1 || len(snapshot.Records) != 1 {
		t.Fatalf("duplicated mutation: %+v", snapshot)
	}
}

func claimTestMemoryJob(t *testing.T, s *SQLiteStore, id domain.SessionID, scope string) memory.Job {
	t.Helper()
	if err := s.EnqueueMemoryJob(t.Context(), id, scope); err != nil {
		t.Fatal(err)
	}
	jobs, err := s.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("claim %d jobs: %v", len(jobs), err)
	}
	return jobs[0]
}

func TestMemoryExtractionAtomicAndIncremental(t *testing.T) {
	s := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, s)
	explicit := mutateTestMemory(t, s, memory.Mutation{RequestID: "explicit", Scope: "/ws", Content: "user preference"})
	bumpSessionVersion(t, s, id, 3)
	job := claimTestMemoryJob(t, s, id, "/ws")
	if job.ExtractedVersion != -1 {
		t.Fatalf("initial watermark=%d", job.ExtractedVersion)
	}
	bumpSessionVersion(t, s, id, 4)
	if err := s.CompleteMemoryExtraction(t.Context(), job, &memory.ExtractionResult{RawMemory: "stale"}); errorCode(err) != domain.ErrConflict {
		t.Fatalf("stale evidence commit=%v", err)
	}
	if _, err := s.LoadMemoryEvidence(t.Context(), job); errorCode(err) != domain.ErrConflict {
		t.Fatalf("stale evidence load=%v", err)
	}
	if len(snapshotTestMemory(t, s, "/ws").Records) != 1 {
		t.Fatal("conflicted completion wrote records")
	}
	bumpSessionVersion(t, s, id, 3)
	if err := s.CompleteMemoryExtraction(t.Context(), job, &memory.ExtractionResult{RawMemory: "increment one", RolloutSummary: "summary one"}); err != nil {
		t.Fatal(err)
	}
	bumpSessionVersion(t, s, id, 6)
	next := claimTestMemoryJob(t, s, id, "/ws")
	if next.ExtractedVersion != 3 {
		t.Fatalf("next watermark=%d", next.ExtractedVersion)
	}
	if err := s.CompleteMemoryExtraction(t.Context(), next, &memory.ExtractionResult{RawMemory: "increment two"}); err != nil {
		t.Fatal(err)
	}
	snapshot := snapshotTestMemory(t, s, "/ws")
	if len(snapshot.Records) != 3 || snapshot.Records[0] != explicit {
		t.Fatalf("automatic replaced explicit: %+v", snapshot.Records)
	}
	if err := s.CompleteMemoryExtraction(t.Context(), next, nil); errorCode(err) != domain.ErrConflict {
		t.Fatalf("stale token completion=%v", err)
	}
	bumpSessionVersion(t, s, id, 7)
	last := claimTestMemoryJob(t, s, id, "/ws")
	if err := s.CompleteMemoryExtraction(t.Context(), last, nil); err != nil {
		t.Fatal(err)
	}
	var status string
	if err := s.db.QueryRow("SELECT status FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&status); err != nil || status != string(memory.JobSucceededNoOutput) {
		t.Fatalf("no-output status=%q err=%v", status, err)
	}
	if err := s.DeleteSession(t.Context(), id); err != nil {
		t.Fatal(err)
	}
	after := snapshotTestMemory(t, s, "/ws")
	if len(after.Records) != 1 || after.Records[0] != explicit || after.Epoch <= snapshot.Epoch {
		t.Fatalf("delete semantics: %+v", after)
	}
}

func TestMemoryExtractionFieldValidationIsAtomic(t *testing.T) {
	limit := memory.MaxExtractionFieldBytes
	for _, field := range []string{"raw_memory", "rollout_summary"} {
		for _, invalid := range []string{"oversized", "invalid_utf8"} {
			t.Run(field+"/"+invalid, func(t *testing.T) {
				s := openMemoryJobTestStore(t)
				id := createMemoryJobSession(t, s)
				job := claimTestMemoryJob(t, s, id, "/ws")
				before := readMemoryJobRetryState(t, s, id)
				result := &memory.ExtractionResult{RawMemory: "valid memory", RolloutSummary: "valid rollout"}
				value := strings.Repeat("x", limit+1)
				if invalid == "invalid_utf8" {
					value = "invalid\xff"
				}
				if field == "raw_memory" {
					result.RawMemory = value
				} else {
					result.RolloutSummary = value
				}
				if err := s.CompleteMemoryExtraction(t.Context(), job, result); errorCode(err) != domain.ErrInvalidInput {
					t.Fatalf("invalid extraction=%v", err)
				}
				if after := readMemoryJobRetryState(t, s, id); after != before {
					t.Fatalf("invalid extraction changed claim: %+v -> %+v", before, after)
				}
				var extracted int64
				if err := s.db.QueryRowContext(t.Context(), "SELECT extracted_version FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&extracted); err != nil || extracted != job.ExtractedVersion {
					t.Fatalf("watermark=%d err=%v", extracted, err)
				}
				for _, table := range []string{"memory_records", "memory_requests", "memory_scope_state"} {
					var count int
					if err := s.db.QueryRowContext(t.Context(), "SELECT count(*) FROM "+table).Scan(&count); err != nil || count != 0 {
						t.Fatalf("invalid extraction wrote %s: count=%d err=%v", table, count, err)
					}
				}
				if err := s.CompleteMemoryExtraction(t.Context(), job, &memory.ExtractionResult{RawMemory: "retry valid"}); err != nil {
					t.Fatalf("same claim could not retry: %v", err)
				}
			})
		}
	}
}

func TestMemoryExtractionLimitsApplyAfterRedactionAndTrim(t *testing.T) {
	s := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, s)
	job := claimTestMemoryJob(t, s, id, "/ws")
	// Both fields independently fit at the byte limit, even though their sum
	// exceeds it and their untrimmed/redaction inputs are larger.
	content := strings.Repeat("x", memory.MaxExtractionFieldBytes)
	secret := "sk-" + strings.Repeat("a", memory.MaxExtractionFieldBytes)
	rollout := strings.Repeat("界", memory.MaxExtractionFieldBytes/3)
	result := &memory.ExtractionResult{RawMemory: " \n" + content + "\t ", RolloutSummary: " " + secret + " "}
	if len(memory.RedactSecrets(secret)) >= len(secret) {
		t.Fatal("test secret was not redacted")
	}
	if err := s.CompleteMemoryExtraction(t.Context(), job, result); err != nil {
		t.Fatal(err)
	}
	first := snapshotTestMemory(t, s, "/ws")
	if len(first.Records) != 1 || first.Records[0].Content != content || first.Records[0].Rollout != strings.TrimSpace(memory.RedactSecrets(secret)) {
		t.Fatalf("normalized extraction=%+v", first.Records)
	}
	bumpSessionVersion(t, s, id, 1)
	next := claimTestMemoryJob(t, s, id, "/ws")
	if err := s.CompleteMemoryExtraction(t.Context(), next, &memory.ExtractionResult{RawMemory: content, RolloutSummary: rollout}); err != nil {
		t.Fatalf("independent field limits rejected: %v", err)
	}
	var count int
	if err := s.db.QueryRowContext(t.Context(), `SELECT count(*) FROM memory_requests r JOIN memory_records m ON m.id = r.record_id WHERE m.kind = 'automatic'`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("automatic receipts=%d err=%v", count, err)
	}
}

func TestMemoryEvidenceRawReplayAndIncrement(t *testing.T) {
	s := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, s)
	ctx := t.Context()
	now := time.Now().UTC()
	original := domain.Message{ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser, Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: now, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "original full message"}}}
	events := []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil), messageEvent(t, 2, id, domain.EventUserMessageAdded, original)}
	checkpoint := testCheckpoint(id, 2, now)
	checkpoint.Messages = []domain.Message{{ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser, Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: now, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "compressed fake evidence"}}}}
	if err := s.AppendEventsAndCheckpoint(ctx, id, 0, events, checkpoint); err != nil {
		t.Fatal(err)
	}
	job := claimTestMemoryJob(t, s, id, "")
	messages, err := s.LoadMemoryEvidence(ctx, job)
	if err != nil || len(messages) != 1 || strings.Join(messages[0].TextParts(), "") != "original full message" {
		t.Fatalf("raw evidence=%+v err=%v", messages, err)
	}
	if err := s.CompleteMemoryExtraction(ctx, job, nil); err != nil {
		t.Fatal(err)
	}
	next := original
	next.ID = domain.NewMessageID()
	next.Sequence = 2
	next.Parts = []domain.ContentPart{{Kind: domain.PartText, Text: "new increment"}}
	// A masking directive must never replace the extraction evidence.
	ref := testArtifactRef(t, 'a', 12)
	tail := []domain.Event{messageEvent(t, 3, id, domain.EventUserMessageAdded, next), maskedDirectiveEvent(t, id, 4, next.ID, ref)}
	if err := s.AppendEvents(ctx, id, 2, tail); err != nil {
		t.Fatal(err)
	}
	nextJob := claimTestMemoryJob(t, s, id, "")
	messages, err = s.LoadMemoryEvidence(ctx, nextJob)
	if err != nil || len(messages) != 1 || strings.Join(messages[0].TextParts(), "") != "new increment" {
		t.Fatalf("increment evidence=%+v err=%v", messages, err)
	}
	if nextJob.ExtractedVersion != 2 || nextJob.SessionVersion != 4 {
		t.Fatalf("evidence boundary=%+v", nextJob)
	}
	stale := nextJob
	stale.ClaimToken = "stale"
	if _, err := s.LoadMemoryEvidence(ctx, stale); errorCode(err) != domain.ErrConflict {
		t.Fatalf("stale claim load=%v", err)
	}
}

func TestMemoryRewindRevokesEvidenceAndRequests(t *testing.T) {
	s := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, s)
	ctx := t.Context()
	if err := s.AppendEventsAndCheckpoint(ctx, id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}, testCheckpoint(id, 1, time.Now().UTC())); err != nil {
		t.Fatal(err)
	}
	kept := mutateTestMemory(t, s, memory.Mutation{RequestID: "kept", Content: "kept explicit", SessionID: id, SourceVersion: 1})
	firstJob := claimTestMemoryJob(t, s, id, "")
	if err := s.CompleteMemoryExtraction(ctx, firstJob, &memory.ExtractionResult{RawMemory: "kept automatic"}); err != nil {
		t.Fatal(err)
	}
	if err := s.AppendEventsAndCheckpoint(ctx, id, 1, []domain.Event{newEvent(id, 2, domain.EventSessionCreated, nil)}, testCheckpoint(id, 2, time.Now().UTC())); err != nil {
		t.Fatal(err)
	}
	removed := memory.Mutation{RequestID: "rewound-tool", Content: "removed explicit", SessionID: id, SourceVersion: 2}
	removedRecord := mutateTestMemory(t, s, removed)
	secondJob := claimTestMemoryJob(t, s, id, "")
	if err := s.CompleteMemoryExtraction(ctx, secondJob, &memory.ExtractionResult{RawMemory: "removed automatic"}); err != nil {
		t.Fatal(err)
	}
	before := snapshotTestMemory(t, s, "")
	if ok, err := s.PublishMemoryView(ctx, before, "includes removed", "removed"); err != nil || !ok {
		t.Fatalf("publish=%v %v", ok, err)
	}
	// Also fence a currently running worker even if its prior watermark survives.
	if err := s.AppendEvents(ctx, id, 2, []domain.Event{newEvent(id, 3, domain.EventSessionCreated, nil)}); err != nil {
		t.Fatal(err)
	}
	claimed := claimTestMemoryJob(t, s, id, "")
	if _, err := s.RewindSession(ctx, id, 1); err != nil {
		t.Fatal(err)
	}
	after := snapshotTestMemory(t, s, "")
	if len(after.Records) != 2 || after.Records[0] != kept || after.Epoch <= before.Epoch || after.Version <= before.Version || after.ValidView() {
		t.Fatalf("rewound snapshot=%+v", after)
	}
	if err := s.CompleteMemoryExtraction(ctx, claimed, &memory.ExtractionResult{RawMemory: "stale worker"}); errorCode(err) != domain.ErrConflict {
		t.Fatalf("rewound worker=%v", err)
	}
	if ok, err := s.PublishMemoryView(ctx, before, "old", "old"); err != nil || ok {
		t.Fatalf("rewound publication=%v %v", ok, err)
	}
	var count int
	if err := s.db.QueryRow("SELECT count(*) FROM memory_requests WHERE record_id = ?", removedRecord.ID).Scan(&count); err != nil || count != 0 {
		t.Fatalf("rewound request count=%d %v", count, err)
	}
	var extracted int64
	if err := s.db.QueryRow("SELECT extracted_version FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&extracted); err != nil || extracted != 1 {
		t.Fatalf("surviving extraction watermark=%d %v", extracted, err)
	}
	if _, err := s.MutateMemory(ctx, removed); errorCode(err) != domain.ErrConflict {
		t.Fatalf("rewound origin accepted: %v", err)
	}
	if err := s.AppendEvents(ctx, id, 1, []domain.Event{newEvent(id, 2, domain.EventSessionCreated, nil)}); err != nil {
		t.Fatal(err)
	}
	replayed := mutateTestMemory(t, s, removed)
	if replayed.ID != removedRecord.ID || replayed.Sequence <= removedRecord.Sequence {
		t.Fatalf("tool call falsely reused old receipt: %+v", replayed)
	}
}

func TestMemoryBackfillSkipsDelegatedAndUsesWorkspace(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	ws, err := s.UpsertWorkspace(ctx, domain.Workspace{ID: domain.NewWorkspaceID(), Name: "workspace", RootPath: filepath.Join(t.TempDir(), "ws")})
	if err != nil {
		t.Fatal(err)
	}
	top, child, active, ancient, empty := domain.NewSessionID(), domain.NewSessionID(), domain.NewSessionID(), domain.NewSessionID(), domain.NewSessionID()
	for _, id := range []domain.SessionID{top, child, active, ancient, empty} {
		if err := s.CreateSession(ctx, id, ws.ID); err != nil {
			t.Fatal(err)
		}
		if id != empty {
			event := newEvent(id, 1, domain.EventSessionCreated, nil)
			if id == child {
				event.Type = domain.EventRunCreated
				event.Payload = []byte(`{"parent_session_id":"` + top.String() + `"}`)
			}
			if err := s.AppendEvents(ctx, id, 0, []domain.Event{event}); err != nil {
				t.Fatal(err)
			}
		}
		if id != active {
			ageSession(t, s, id, 2*time.Hour)
		}
	}
	ageSession(t, s, ancient, 48*time.Hour)
	if err := s.BackfillMemoryJobs(ctx, 24*time.Hour); err != nil {
		t.Fatal(err)
	}
	jobs, err := s.ClaimMemoryJobs(ctx, 10, 0, 24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 || jobs[0].SessionID != top || jobs[0].WorkspaceRoot != ws.RootPath {
		t.Fatalf("backfilled=%+v err=%v", jobs, err)
	}
	if err := s.CompleteMemoryExtraction(ctx, jobs[0], &memory.ExtractionResult{RawMemory: "automatic"}); err != nil {
		t.Fatal(err)
	}
	explicit := mutateTestMemory(t, s, memory.Mutation{RequestID: "long-lived", Scope: ws.RootPath, Content: "explicit survives", SessionID: top, SourceVersion: 1})
	before := snapshotTestMemory(t, s, ws.RootPath)
	if ok, err := s.PublishMemoryView(ctx, before, "both", "both"); err != nil || !ok {
		t.Fatalf("publish=%v %v", ok, err)
	}
	if err := s.DeleteWorkspace(ctx, ws.ID); err != nil {
		t.Fatal(err)
	}
	after := snapshotTestMemory(t, s, ws.RootPath)
	if len(after.Records) != 1 || after.Records[0] != explicit || after.Epoch <= before.Epoch || after.ValidView() {
		t.Fatalf("workspace deletion=%+v", after)
	}
}

// seedMemoryUser appends real raw user evidence without a checkpoint fallback.
func seedMemoryUser(t *testing.T, s *SQLiteStore, id domain.SessionID, sequence int64, text string, at time.Time) {
	t.Helper()
	message := domain.Message{ID: domain.NewMessageID(), Sequence: sequence, Role: domain.RoleUser, Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: at, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: text}}}
	event := messageEvent(t, sequence, id, domain.EventUserMessageAdded, message)
	event.Timestamp = at
	if err := s.AppendEvents(t.Context(), id, sequence-1, []domain.Event{event}); err != nil {
		t.Fatal(err)
	}
}

func TestMemoryForgetBarrierScopeAndGlobal(t *testing.T) {
	for _, global := range []bool{false, true} {
		name := "workspace"
		if global {
			name = "global"
		}
		t.Run(name, func(t *testing.T) {
			s := openMemoryJobTestStore(t)
			ctx := t.Context()
			scope := "/a"
			if global {
				scope = ""
			}
			explicit := mutateTestMemory(t, s, memory.Mutation{RequestID: "explicit", Scope: scope, Content: "forgotten fact"})
			otherExplicit := mutateTestMemory(t, s, memory.Mutation{RequestID: "other", Scope: "/b", Content: "keep explicit"})
			old := time.Now().UTC().Add(-time.Hour)
			ids := map[string]domain.SessionID{}
			snapshots := map[string]memory.Snapshot{}
			for _, root := range []string{"", "/a", "/b"} {
				id := createMemoryJobSession(t, s)
				ids[root] = id
				seedMemoryUser(t, s, id, 1, "forgotten fact", old)
				job := claimTestMemoryJob(t, s, id, root)
				if err := s.CompleteMemoryExtraction(ctx, job, &memory.ExtractionResult{RawMemory: "forgotten fact", RolloutSummary: "forgotten fact in rollout"}); err != nil {
					t.Fatal(err)
				}
				snapshot := snapshotTestMemory(t, s, root)
				snapshots[root] = snapshot
				if ok, err := s.PublishMemoryView(ctx, snapshot, "forgotten fact in main", "forgotten fact in summary"); err != nil || !ok {
					t.Fatalf("publish=%v %v", ok, err)
				}
			}
			// An in-flight model call has already read the old evidence.
			seedMemoryUser(t, s, ids["/a"], 2, "forgotten fact again", old)
			inflight := claimTestMemoryJob(t, s, ids["/a"], "/a")
			mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", Scope: scope, ID: explicit.ID, Revision: explicit.Revision, Forget: true})
			for _, root := range []string{"", "/a", "/b"} {
				after := snapshotTestMemory(t, s, root)
				affected := global || root == scope
				automatic := 0
				for _, r := range after.Records {
					if r.Kind == memory.Automatic {
						automatic++
					}
				}
				if affected {
					if automatic != 0 || after.ValidView() || after.Epoch <= snapshots[root].Epoch {
						t.Fatalf("scope %q retained forgotten memory: %+v", root, after)
					}
					if ok, err := s.PublishMemoryView(ctx, snapshots[root], "stale", "stale"); err != nil || ok {
						t.Fatalf("scope %q stale publication=%v %v", root, ok, err)
					}
				} else if automatic != 1 || !after.ValidView() {
					t.Fatalf("unrelated scope %q changed: %+v", root, after)
				}
			}
			b := snapshotTestMemory(t, s, "/b")
			found := false
			for _, r := range b.Records {
				if r.ID == otherExplicit.ID {
					found = true
				}
			}
			if !found {
				t.Fatal("global forget deleted unrelated explicit memory")
			}
			if err := s.CompleteMemoryExtraction(ctx, inflight, &memory.ExtractionResult{RawMemory: "revived"}); errorCode(err) != domain.ErrConflict {
				t.Fatalf("old worker revived facts: %v", err)
			}
			if _, err := s.LoadMemoryEvidence(ctx, inflight); errorCode(err) != domain.ErrConflict {
				t.Fatalf("old claim crossed barrier: %v", err)
			}
			// Reclaim the same old increment after the lease; it must load nothing,
			// and even a non-nil supplied result settles no-output, without records.
			if _, err := s.db.Exec("UPDATE memory_jobs SET claimed_at_unix_nano = 0 WHERE session_id = ?", inflight.SessionID.String()); err != nil {
				t.Fatal(err)
			}
			reclaimed, err := s.ClaimMemoryJobs(ctx, 1, 0, 24*time.Hour, time.Hour, 5)
			if err != nil || len(reclaimed) != 1 {
				t.Fatalf("reclaim=%+v %v", reclaimed, err)
			}
			evidence, err := s.LoadMemoryEvidence(ctx, reclaimed[0])
			if err != nil || len(evidence) != 0 {
				t.Fatalf("old evidence survived cutoff: %+v %v", evidence, err)
			}
			if err := s.CompleteMemoryExtraction(ctx, reclaimed[0], &memory.ExtractionResult{RawMemory: "must not revive"}); err != nil {
				t.Fatal(err)
			}
			if len(snapshotTestMemory(t, s, "/a").Records) != 0 {
				t.Fatal("old increment revived forgotten facts")
			}
			// A new session cannot re-extract old history. Only fresh user evidence
			// is admitted; a fresh assistant echo of old content is not evidence.
			freshID := createMemoryJobSession(t, s)
			seedMemoryUser(t, s, freshID, 1, "old historical fact", old)
			freshAt := time.Now().UTC()
			seedMemoryUser(t, s, freshID, 2, "new user preference", freshAt)
			echo := domain.Message{ID: domain.NewMessageID(), Sequence: 3, Role: domain.RoleAssistant, Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: freshAt, Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "forgotten fact echoed"}}}
			if err := s.AppendEvents(ctx, freshID, 2, []domain.Event{messageEvent(t, 3, freshID, domain.EventModelResponseCompleted, echo)}); err != nil {
				t.Fatal(err)
			}
			freshJob := claimTestMemoryJob(t, s, freshID, "/a")
			evidence, err = s.LoadMemoryEvidence(ctx, freshJob)
			if err != nil || len(evidence) != 1 || strings.Join(evidence[0].TextParts(), "") != "new user preference" {
				t.Fatalf("filtered evidence=%+v %v", evidence, err)
			}
			if err := s.CompleteMemoryExtraction(ctx, freshJob, &memory.ExtractionResult{RawMemory: "new user preference"}); err != nil {
				t.Fatal(err)
			}
			if len(snapshotTestMemory(t, s, "/a").Records) != 1 {
				t.Fatal("fresh evidence could not publish")
			}
		})
	}
}

func TestMemoryUpdateBarrierAndMaximumCutoff(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	global := mutateTestMemory(t, s, memory.Mutation{RequestID: "global", Content: "global old"})
	local := mutateTestMemory(t, s, memory.Mutation{RequestID: "local", Scope: "/ws", Content: "local old"})
	seedID := createMemoryJobSession(t, s)
	seedMemoryUser(t, s, seedID, 1, "local old", time.Now().UTC().Add(-time.Hour))
	seedJob := claimTestMemoryJob(t, s, seedID, "/ws")
	if err := s.CompleteMemoryExtraction(ctx, seedJob, &memory.ExtractionResult{RawMemory: "local old", RolloutSummary: "local old duplicate"}); err != nil {
		t.Fatal(err)
	}
	before := snapshotTestMemory(t, s, "/ws")
	if ok, err := s.PublishMemoryView(ctx, before, "local old duplicate", "local old duplicate"); err != nil || !ok {
		t.Fatalf("publish before update=%v %v", ok, err)
	}
	local = mutateTestMemory(t, s, memory.Mutation{RequestID: "update-local", Scope: "/ws", ID: local.ID, Revision: local.Revision, Content: "local corrected"})
	after := snapshotTestMemory(t, s, "/ws")
	if len(after.Records) != 1 || after.Records[0] != local || after.ValidView() || after.Epoch <= before.Epoch {
		t.Fatalf("update retained duplicate evidence: %+v", after)
	}
	var localCutoff int64
	if err := s.db.QueryRow("SELECT evidence_cutoff_unix_nano FROM memory_scope_state WHERE scope = ?", "/ws").Scan(&localCutoff); err != nil || localCutoff <= 0 {
		t.Fatalf("local cutoff=%d %v", localCutoff, err)
	}
	id := createMemoryJobSession(t, s)
	seedMemoryUser(t, s, id, 1, "between local and global barrier", time.Now().UTC())
	mutateTestMemory(t, s, memory.Mutation{RequestID: "update-global", ID: global.ID, Revision: global.Revision, Content: "global corrected"})
	job := claimTestMemoryJob(t, s, id, "/ws")
	messages, err := s.LoadMemoryEvidence(ctx, job)
	if err != nil || len(messages) != 0 {
		t.Fatalf("did not take maximum cutoff: %+v %v", messages, err)
	}
	snapshot := snapshotTestMemory(t, s, "/ws")
	if len(snapshot.Records) != 1 || snapshot.Records[0] != local {
		t.Fatal("global correction changed local explicit")
	}
}

func TestMemoryForgetBarrierPersistsAcrossReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "memory.db")
	ctx := t.Context()
	s, err := OpenSQLiteStore(ctx, path)
	if err != nil {
		t.Fatal(err)
	}
	explicit := mutateTestMemory(t, s, memory.Mutation{RequestID: "explicit", Content: "forgotten"})
	id := createMemoryJobSession(t, s)
	seedMemoryUser(t, s, id, 1, "forgotten old evidence", time.Now().UTC().Add(-time.Hour))
	mutateTestMemory(t, s, memory.Mutation{RequestID: "forget", ID: explicit.ID, Revision: explicit.Revision, Forget: true})
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reopened := openTestSQLiteStore(t, path)
	job := claimTestMemoryJob(t, reopened, id, "/new-workspace")
	messages, err := reopened.LoadMemoryEvidence(ctx, job)
	if err != nil || len(messages) != 0 {
		t.Fatalf("global barrier lost on reopen/new scope: %+v %v", messages, err)
	}
	if err := reopened.CompleteMemoryExtraction(ctx, job, &memory.ExtractionResult{RawMemory: "revived after restart"}); err != nil {
		t.Fatal(err)
	}
	if len(snapshotTestMemory(t, reopened, "/new-workspace").Records) != 0 {
		t.Fatal("restart revived forgotten history")
	}
}

func TestMemoryMutationSourceValidation(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	id := createMemoryJobSession(t, s)
	if err := s.AppendEventsAndCheckpoint(ctx, id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}, testCheckpoint(id, 1, time.Now().UTC())); err != nil {
		t.Fatal(err)
	}
	for _, m := range []memory.Mutation{
		{RequestID: "missing", SessionID: domain.NewSessionID(), SourceVersion: 1, Content: "no"},
		{RequestID: "future", SessionID: id, SourceVersion: 2, Content: "no"},
	} {
		if _, err := s.MutateMemory(ctx, m); errorCode(err) != domain.ErrConflict {
			t.Fatalf("invalid source accepted: %+v %v", m, err)
		}
	}
	if _, err := s.MutateMemory(ctx, memory.Mutation{RequestID: "partial", SourceVersion: 1, Content: "no"}); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("partial source=%v", err)
	}
	// A version number alone is insufficient when called by the real tool loop.
	start := newEvent(id, 2, domain.EventToolExecutionStarted, []byte(`{"call_id":"tool-origin"}`))
	if err := s.AppendEvents(ctx, id, 1, []domain.Event{start}); err != nil {
		t.Fatal(err)
	}
	toolCtx := domain.WithToolContext(ctx, domain.ToolContext{SessionID: id, SourceVersion: 2})
	mutation := memory.Mutation{RequestID: "tool-origin", SessionID: id, SourceVersion: 2, Content: "durable origin"}
	r, err := s.MutateMemory(toolCtx, mutation)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.MutateMemory(toolCtx, memory.Mutation{RequestID: "no-start", SessionID: id, SourceVersion: 2, Content: "no"}); errorCode(err) != domain.ErrConflict {
		t.Fatalf("missing execution identity accepted: %v", err)
	}
	if _, err := s.RewindSession(ctx, id, 1); err != nil {
		t.Fatal(err)
	}
	if _, err := s.MutateMemory(toolCtx, mutation); errorCode(err) != domain.ErrConflict {
		t.Fatalf("rewound origin accepted: %v", err)
	}
	// Reusing version 2 for a different execution must not resurrect the old tool.
	replacement := newEvent(id, 2, domain.EventToolExecutionStarted, []byte(`{"call_id":"different-tool"}`))
	if err := s.AppendEvents(ctx, id, 1, []domain.Event{replacement}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.MutateMemory(toolCtx, mutation); errorCode(err) != domain.ErrConflict {
		t.Fatalf("reused version accepted stale execution: %v", err)
	}
	if len(snapshotTestMemory(t, s, "").Records) != 0 {
		t.Fatalf("rewound record %s survived", r.ID)
	}
	// Reject origins from deleted sessions, including otherwise valid receipts.
	fresh := memory.Mutation{RequestID: "different-tool", SessionID: id, SourceVersion: 2, Content: "fresh origin"}
	if _, err := s.MutateMemory(toolCtx, fresh); err != nil {
		t.Fatal(err)
	}
	if err := s.DeleteSession(ctx, id); err != nil {
		t.Fatal(err)
	}
	if _, err := s.MutateMemory(toolCtx, fresh); errorCode(err) != domain.ErrConflict {
		t.Fatalf("deleted session receipt accepted: %v", err)
	}
}

// seedContextRecords batches a large fixture to exercise SQL-side limits.
func seedContextRecords(t *testing.T, s *SQLiteStore, scope string, explicit, automatic int) {
	t.Helper()
	tx, err := s.db.BeginTx(t.Context(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	for i := range explicit + automatic {
		kind := memory.Explicit
		if i >= explicit {
			kind = memory.Automatic
		}
		at := time.Unix(int64(i+1), 0).UTC()
		_, err := tx.ExecContext(t.Context(), `INSERT INTO memory_records (`+memoryRecordColumns+`) VALUES (?, ?, ?, ?, ?, 1, ?, '', 0, ?)`, fmt.Sprintf("%s-%s-%04d", scope, kind, i), scope, kind, fmt.Sprintf("Go 中文 record %04d", i), "BAZEL rollout", i+1, formatTime(at))
		if err != nil {
			t.Fatal(err)
		}
	}
	if _, err := tx.ExecContext(t.Context(), `INSERT INTO memory_scope_state(scope, version, epoch) VALUES (?, ?, 1)`, scope, explicit+automatic); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
}

func TestMemoryContextBoundedAndCoverage(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	seedContextRecords(t, s, "", 105, 65)
	seedContextRecords(t, s, "/ws", 105, 65)
	seedContextRecords(t, s, "/hidden", 1, 1)
	contexts, err := s.MemoryContext(ctx, "/ws")
	if err != nil || len(contexts) != 2 {
		t.Fatalf("contexts=%+v err=%v", contexts, err)
	}
	for i, snapshot := range contexts {
		scope := ""
		if i == 1 {
			scope = "/ws"
		}
		if snapshot.Scope != scope || !snapshot.Omitted || len(snapshot.Records) != 150 || snapshot.ValidView() {
			t.Fatalf("bounded snapshot=%+v", snapshot)
		}
		if snapshot.Records[0].ID != fmt.Sprintf("%s-explicit-0104", scope) || snapshot.Records[100].ID != fmt.Sprintf("%s-automatic-0169", scope) {
			t.Fatalf("latest records order=%+v", snapshot.Records)
		}
		full := snapshotTestMemory(t, s, scope)
		if len(full.Records) != 170 || full.Omitted {
			t.Fatalf("background snapshot bounded unexpectedly: %+v", full)
		}
		if ok, err := s.PublishMemoryView(ctx, full, "covered automatic", "covered summary"); err != nil || !ok {
			t.Fatalf("publish=%v %v", ok, err)
		}
	}
	contexts, err = s.MemoryContext(ctx, "/ws")
	if err != nil {
		t.Fatal(err)
	}
	for _, snapshot := range contexts {
		if len(snapshot.Records) != 100 || !snapshot.Omitted || !snapshot.ValidView() || snapshot.View.Version != 170 {
			t.Fatalf("coverage was fabricated: %+v", snapshot)
		}
		for _, r := range snapshot.Records {
			if r.Kind != memory.Explicit {
				t.Fatal("covered automatic loaded")
			}
		}
	}
	// With exactly the explicit limit and all automatic covered, omission is false.
	if _, err := s.db.Exec(`DELETE FROM memory_records WHERE kind = 'explicit' AND sequence <= 5 AND scope IN ('', '/ws')`); err != nil {
		t.Fatal(err)
	}
	contexts, err = s.MemoryContext(ctx, "/ws")
	if err != nil {
		t.Fatal(err)
	}
	for _, snapshot := range contexts {
		if snapshot.Omitted || len(snapshot.Records) != 100 {
			t.Fatalf("exact-limit omission=%+v", snapshot)
		}
	}
	// A stale-epoch view must not hide automatic evidence.
	if _, err := s.db.Exec(`UPDATE memory_views SET epoch = 0 WHERE scope = '/ws'`); err != nil {
		t.Fatal(err)
	}
	contexts, err = s.MemoryContext(ctx, "/ws")
	if err != nil || contexts[1].ValidView() || !contexts[1].Omitted || len(contexts[1].Records) != 150 {
		t.Fatalf("invalid view masked evidence: %+v %v", contexts, err)
	}
	// Valid partial coverage loads only the remaining automatic increment.
	if _, err := s.db.Exec(`UPDATE memory_views SET epoch = 1, version = 150 WHERE scope = '/ws'`); err != nil {
		t.Fatal(err)
	}
	contexts, err = s.MemoryContext(ctx, "/ws")
	if err != nil || contexts[1].Omitted || len(contexts[1].Records) != 120 || contexts[1].View.Version != 150 {
		t.Fatalf("partial coverage=%+v %v", contexts, err)
	}
	for _, r := range contexts[1].Records {
		if r.Kind == memory.Automatic && r.Sequence <= 150 {
			t.Fatal("already-covered automatic loaded")
		}
	}
	global, err := s.MemoryContext(ctx, "")
	if err != nil || len(global) != 1 || global[0].Scope != "" {
		t.Fatalf("global duplicated/leaked workspace: %+v %v", global, err)
	}
}

func TestMemorySQLQueryAndPointRead(t *testing.T) {
	s := openMemoryJobTestStore(t)
	ctx := t.Context()
	seedContextRecords(t, s, "", 105, 65)
	seedContextRecords(t, s, "/ws", 105, 65)
	seedContextRecords(t, s, "/hidden", 1, 1)
	store := memory.NewStore(s).ForWorkspace("/ws")
	listed, err := store.Records(ctx)
	if err != nil || len(listed) != 200 {
		t.Fatalf("bounded list length=%d err=%v", len(listed), err)
	}
	for _, r := range listed {
		if r.Scope != "" && r.Scope != "/ws" {
			t.Fatal("list scope leak")
		}
	}
	result, err := store.Search(ctx, "GO bazel 中文", 7)
	if err != nil || !result.Truncated || len(result.Records) != 7 {
		t.Fatalf("search=%+v err=%v", result, err)
	}
	for _, r := range result.Records {
		if r.Kind != memory.Explicit {
			t.Fatal("automatic sorted before explicit")
		}
	}
	result, err = s.QueryMemory(ctx, "/ws", "record 0169", 2)
	if err != nil || result.Truncated || len(result.Records) != 2 {
		t.Fatalf("exact limit=%+v %v", result, err)
	}
	// Old records omitted from prompt/list remain addressable directly.
	oldID := "/ws-explicit-0000"
	r, err := store.Read(ctx, oldID)
	if err != nil || r.ID != oldID || r.Scope != "/ws" {
		t.Fatalf("point read old=%+v %v", r, err)
	}
	for _, id := range []string{"/hidden-explicit-0000", "missing", "' OR 1=1 --"} {
		if _, err := store.Read(ctx, id); errorCode(err) != domain.ErrInvalidInput {
			t.Fatalf("invisible/injected ID %q accepted: %v", id, err)
		}
	}
	if _, err := memory.NewStore(s).Read(ctx, oldID); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("global read leaked workspace: %v", err)
	}
	for _, query := range []string{"absent", "' OR 1=1 --", "%", "_"} {
		result, err := s.QueryMemory(ctx, "/ws", query, 10)
		if err != nil || len(result.Records) != 0 || result.Truncated {
			t.Fatalf("literal query=%q %+v %v", query, result, err)
		}
	}
	result, err = s.QueryMemory(ctx, "' OR 1=1 --", "", 200)
	if err != nil || len(result.Records) != 170 || result.Truncated {
		t.Fatalf("scope injection=%+v %v", result, err)
	}
	for _, r := range result.Records {
		if r.Scope != "" {
			t.Fatal("injected scope exposed other records")
		}
	}
	for _, limit := range []int{-1, 0, 201} {
		if _, err := s.QueryMemory(ctx, "/ws", "", limit); errorCode(err) != domain.ErrInvalidInput {
			t.Fatalf("invalid limit=%d %v", limit, err)
		}
	}
	// A corrupt unrelated record proves point reads and selective searches do
	// not decode full visible sets before applying their SQL filters.
	if _, err := s.db.Exec(`UPDATE memory_records SET source_session_id = 'invalid' WHERE id = '/ws-explicit-0001'`); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Read(ctx, oldID); err != nil {
		t.Fatalf("point read scanned unrelated record: %v", err)
	}
	result, err = store.Search(ctx, "record 0000", 2)
	if err != nil || len(result.Records) != 2 {
		t.Fatalf("query scanned nonmatching corrupt record: %+v %v", result, err)
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if _, err := s.MemoryContext(cancelled, "/ws"); err == nil {
		t.Fatal("context cancellation ignored")
	}
	if _, err := s.ReadMemory(cancelled, "/ws", oldID); err == nil {
		t.Fatal("read cancellation ignored")
	}
	if _, err := s.QueryMemory(cancelled, "/ws", "go", 10); err == nil {
		t.Fatal("query cancellation ignored")
	}
}

func TestMemoryContextCrossScopeConsistentTransaction(t *testing.T) {
	path := filepath.Join(t.TempDir(), "shared-context.db")
	reader := openTestSQLiteStore(t, path)
	writer := openTestSQLiteStore(t, path)
	seedContextRecords(t, reader, "", 1, 0)
	seedContextRecords(t, reader, "/ws", 1, 0)
	ctx := t.Context()
	done := make(chan error, 1)
	var wg sync.WaitGroup
	wg.Go(func() {
		for version := int64(2); version <= 150; version++ {
			tx, err := writer.db.BeginTx(ctx, nil)
			if err != nil {
				done <- err
				return
			}
			if _, err := tx.ExecContext(ctx, `UPDATE memory_scope_state SET version = ? WHERE scope IN ('', '/ws')`, version); err != nil {
				tx.Rollback()
				done <- err
				return
			}
			if _, err := tx.ExecContext(ctx, `UPDATE memory_records SET sequence = ?, content = ? WHERE scope IN ('', '/ws')`, version, fmt.Sprint(version)); err != nil {
				tx.Rollback()
				done <- err
				return
			}
			if _, err := tx.ExecContext(ctx, `INSERT INTO memory_views(scope, version, epoch, main, summary) SELECT scope, version, epoch, '', '' FROM memory_scope_state WHERE scope IN ('', '/ws') ON CONFLICT(scope) DO UPDATE SET version = excluded.version`); err != nil {
				tx.Rollback()
				done <- err
				return
			}
			if err := tx.Commit(); err != nil {
				done <- err
				return
			}
		}
		done <- nil
	})
	for range 150 {
		snapshots, err := reader.MemoryContext(ctx, "/ws")
		if err != nil {
			t.Error(err)
			break
		}
		if len(snapshots) != 2 || snapshots[0].Version != snapshots[1].Version || snapshots[0].View.Version != snapshots[1].View.Version {
			t.Errorf("cross-scope torn snapshot: %+v", snapshots)
			break
		}
		for _, snapshot := range snapshots {
			if len(snapshot.Records) != 1 || snapshot.Records[0].Sequence != snapshot.Version || snapshot.View.Version > snapshot.Version {
				t.Errorf("state/record/view torn snapshot: %+v", snapshot)
				break
			}
		}
	}
	wg.Wait()
	if err := <-done; err != nil && !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
}

func TestEnqueuePreservesClaimAndBackoff(t *testing.T) {
	s := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, s)
	job := claimTestMemoryJob(t, s, id, "/ws")
	bumpSessionVersion(t, s, id, 1)
	if err := s.EnqueueMemoryJob(t.Context(), id, "/other"); err != nil {
		t.Fatal(err)
	}
	var token, root string
	if err := s.db.QueryRow("SELECT claim_token, workspace_root FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&token, &root); err != nil || token != job.ClaimToken || root != "/ws" {
		t.Fatalf("enqueue cleared claim: %q %q %v", token, root, err)
	}
	// A newer event keeps the old token until settlement, but settlement must
	// reschedule that outdated claim rather than charge a provider failure.
	if err := s.FailMemoryJob(t.Context(), job, nil, time.Hour, 5); errorCode(err) != domain.ErrConflict {
		t.Fatalf("outdated claim settlement=%v", err)
	}
	if _, err := s.db.Exec("UPDATE memory_jobs SET next_retry_at_unix_nano = 0 WHERE session_id = ?", id.String()); err != nil {
		t.Fatal(err)
	}
	reclaimed, err := s.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 5)
	if err != nil || len(reclaimed) != 1 {
		t.Fatalf("reclaim=%+v %v", reclaimed, err)
	}
	if err := s.FailMemoryJob(t.Context(), reclaimed[0], nil, time.Hour, 5); err != nil {
		t.Fatal(err)
	}
	if err := s.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
		t.Fatal(err)
	}
	jobs, err := s.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 0 {
		t.Fatalf("enqueue erased backoff: %v %v", jobs, err)
	}
}
