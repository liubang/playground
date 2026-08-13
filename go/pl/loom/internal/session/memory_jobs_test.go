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

package session

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

func openMemoryJobTestStore(t *testing.T) *SQLiteStore {
	t.Helper()
	store, err := OpenSQLiteStore(context.Background(), filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })
	return store
}

func createMemoryJobSession(t *testing.T, store *SQLiteStore) domain.SessionID {
	t.Helper()
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(context.Background(), sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	return sessionID
}

// ageSession rewrites the session's updated_at so idle/age windows treat
// it as last touched at now-d.
func ageSession(t *testing.T, store *SQLiteStore, sessionID domain.SessionID, d time.Duration) {
	t.Helper()
	at := time.Now().UTC().Add(-d).UnixNano()
	if _, err := store.db.Exec(
		"UPDATE sessions SET updated_at_unix_nano = ? WHERE session_id = ?", at, sessionID.String(),
	); err != nil {
		t.Fatalf("age session: %v", err)
	}
}

func bumpSessionVersion(t *testing.T, store *SQLiteStore, sessionID domain.SessionID, version int64) {
	t.Helper()
	if _, err := store.db.Exec(
		"UPDATE sessions SET version = ? WHERE session_id = ?", version, sessionID.String(),
	); err != nil {
		t.Fatalf("bump session version: %v", err)
	}
}

func TestEnqueueAndClaimMemoryJob(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	sessionID := createMemoryJobSession(t, store)

	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws/root"); err != nil {
		t.Fatalf("EnqueueMemoryJob: %v", err)
	}

	jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("ClaimMemoryJobs: %v", err)
	}
	if len(jobs) != 1 {
		t.Fatalf("claimed %d jobs, want 1", len(jobs))
	}
	job := jobs[0]
	if job.SessionID != sessionID {
		t.Fatalf("job session = %s, want %s", job.SessionID, sessionID)
	}
	if job.WorkspaceRoot != "/ws/root" {
		t.Fatalf("job workspace = %q, want /ws/root", job.WorkspaceRoot)
	}
	if job.ClaimToken == "" {
		t.Fatal("claim token is empty")
	}
	if job.SessionVersion != 0 {
		t.Fatalf("job session version = %d, want 0", job.SessionVersion)
	}

	// A second claim within the lease returns nothing.
	again, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("second ClaimMemoryJobs: %v", err)
	}
	if len(again) != 0 {
		t.Fatalf("second claim got %d jobs, want 0", len(again))
	}
}

func TestEnqueueSkipsAlreadyExtractedVersion(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	sessionID := createMemoryJobSession(t, store)

	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("claim: %v (n=%d)", err, len(jobs))
	}
	if err := store.CompleteMemoryJob(ctx, sessionID, jobs[0].ClaimToken, memory.JobSucceeded, jobs[0].SessionVersion); err != nil {
		t.Fatalf("complete: %v", err)
	}

	// Re-enqueue with no new activity: the job must NOT be re-queued.
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("re-enqueue: %v", err)
	}
	jobs, err = store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("re-claim: %v", err)
	}
	if len(jobs) != 0 {
		t.Fatalf("re-claim got %d jobs, want 0 (session already extracted at this version)", len(jobs))
	}

	// New activity (version bump) makes the session extractable again.
	bumpSessionVersion(t, store, sessionID, 3)
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue after activity: %v", err)
	}
	jobs, err = store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("claim after activity: %v", err)
	}
	if len(jobs) != 1 {
		t.Fatalf("claim after activity got %d jobs, want 1", len(jobs))
	}
	if jobs[0].SessionVersion != 3 {
		t.Fatalf("job session version = %d, want 3", jobs[0].SessionVersion)
	}
}

func TestClaimRespectsIdleAndAgeWindows(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	fresh := createMemoryJobSession(t, store)
	old := createMemoryJobSession(t, store)
	ancient := createMemoryJobSession(t, store)
	for _, id := range []domain.SessionID{fresh, old, ancient} {
		if err := store.EnqueueMemoryJob(ctx, id, "/ws"); err != nil {
			t.Fatalf("enqueue %s: %v", id, err)
		}
	}
	ageSession(t, store, old, 2*time.Hour)
	ageSession(t, store, ancient, 60*24*time.Hour)

	// minIdle=1h: the fresh session is skipped; maxAge=30d: ancient is skipped.
	jobs, err := store.ClaimMemoryJobs(ctx, 8, time.Hour, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("claim: %v", err)
	}
	if len(jobs) != 1 || jobs[0].SessionID != old {
		ids := make([]string, 0, len(jobs))
		for _, j := range jobs {
			ids = append(ids, j.SessionID.String())
		}
		t.Fatalf("claimed %v, want only the 2h-idle session", ids)
	}
}

func TestClaimReclaimsExpiredLease(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	sessionID := createMemoryJobSession(t, store)
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("claim: %v (n=%d)", err, len(jobs))
	}

	// Simulate a crashed worker: claimed_at 2h ago, lease 1h.
	stale := time.Now().UTC().Add(-2 * time.Hour).UnixNano()
	if _, err := store.db.Exec(
		"UPDATE memory_jobs SET claimed_at_unix_nano = ? WHERE session_id = ?", stale, sessionID.String(),
	); err != nil {
		t.Fatalf("stale claim: %v", err)
	}
	reclaimed, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("reclaim: %v", err)
	}
	if len(reclaimed) != 1 {
		t.Fatalf("reclaimed %d jobs, want 1 (expired lease)", len(reclaimed))
	}
	if reclaimed[0].ClaimToken == jobs[0].ClaimToken {
		t.Fatal("reclaim reused the stale claim token")
	}

	// The stale token can no longer complete the job.
	if err := store.CompleteMemoryJob(ctx, sessionID, jobs[0].ClaimToken, memory.JobSucceeded, 0); err == nil {
		t.Fatal("stale token completed the job; want token mismatch error")
	}
}

func TestFailMemoryJobBackoffAndAbandon(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	sessionID := createMemoryJobSession(t, store)
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("claim: %v (n=%d)", err, len(jobs))
	}

	// First failure: retryable, backoff not yet due.
	if err := store.FailMemoryJob(ctx, sessionID, jobs[0].ClaimToken, errors.New("model exploded"), time.Hour, 2); err != nil {
		t.Fatalf("fail: %v", err)
	}
	var status string
	var attempts int
	var lastError string
	if err := store.db.QueryRow(
		"SELECT status, attempts, last_error FROM memory_jobs WHERE session_id = ?", sessionID.String(),
	).
		Scan(&status, &attempts, &lastError); err != nil {
		t.Fatalf("read job: %v", err)
	}
	if status != string(memory.JobFailed) || attempts != 1 {
		t.Fatalf("job = (%s, attempts=%d), want (failed, 1)", status, attempts)
	}
	if lastError != "model exploded" {
		t.Fatalf("last_error = %q", lastError)
	}
	jobs, err = store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("claim during backoff: %v", err)
	}
	if len(jobs) != 0 {
		t.Fatalf("claimed %d jobs during backoff, want 0", len(jobs))
	}

	// Backoff due: claimable again; second failure abandons the job.
	if _, err := store.db.Exec(
		"UPDATE memory_jobs SET next_retry_at_unix_nano = 0 WHERE session_id = ?", sessionID.String(),
	); err != nil {
		t.Fatalf("force retry due: %v", err)
	}
	jobs, err = store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("re-claim: %v (n=%d)", err, len(jobs))
	}
	if err := store.FailMemoryJob(ctx, sessionID, jobs[0].ClaimToken, errors.New("boom again"), time.Hour, 2); err != nil {
		t.Fatalf("second fail: %v", err)
	}
	if err := store.db.QueryRow(
		"SELECT status, attempts FROM memory_jobs WHERE session_id = ?", sessionID.String(),
	).
		Scan(&status, &attempts); err != nil {
		t.Fatalf("read job: %v", err)
	}
	if status != string(memory.JobAbandoned) || attempts != 2 {
		t.Fatalf("job = (%s, attempts=%d), want (abandoned, 2)", status, attempts)
	}
	jobs, err = store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("claim after abandon: %v", err)
	}
	if len(jobs) != 0 {
		t.Fatalf("claimed abandoned job (%d), want 0", len(jobs))
	}
}

func TestPhase2Lease(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)

	ok, err := store.AcquirePhase2Lease(ctx, "tok-a", time.Hour)
	if err != nil || !ok {
		t.Fatalf("first acquire = (%v, %v), want (true, nil)", ok, err)
	}
	ok, err = store.AcquirePhase2Lease(ctx, "tok-b", time.Hour)
	if err != nil || ok {
		t.Fatalf("contended acquire = (%v, %v), want (false, nil)", ok, err)
	}
	if err := store.ReleasePhase2Lease(ctx, "tok-b"); err != nil {
		t.Fatalf("release by non-holder: %v", err)
	}
	// Release by a non-holder must not free the lease.
	ok, err = store.AcquirePhase2Lease(ctx, "tok-b", time.Hour)
	if err != nil || ok {
		t.Fatalf("acquire after non-holder release = (%v, %v), want (false, nil)", ok, err)
	}
	if err := store.ReleasePhase2Lease(ctx, "tok-a"); err != nil {
		t.Fatalf("release: %v", err)
	}
	ok, err = store.AcquirePhase2Lease(ctx, "tok-b", time.Hour)
	if err != nil || !ok {
		t.Fatalf("acquire after release = (%v, %v), want (true, nil)", ok, err)
	}

	// Expired lease is takeable by another token.
	stale := time.Now().UTC().Add(-2 * time.Hour).UnixNano()
	if _, err := store.db.Exec("UPDATE memory_phase2_lease SET claimed_at_unix_nano = ? WHERE id = 1", stale); err != nil {
		t.Fatalf("stale lease: %v", err)
	}
	ok, err = store.AcquirePhase2Lease(ctx, "tok-c", time.Hour)
	if err != nil || !ok {
		t.Fatalf("acquire over expired lease = (%v, %v), want (true, nil)", ok, err)
	}
}

func TestDeleteSessionRemovesMemoryJob(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)
	sessionID := createMemoryJobSession(t, store)
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	if err := store.DeleteSession(ctx, sessionID); err != nil {
		t.Fatalf("DeleteSession: %v", err)
	}
	var count int
	if err := store.db.QueryRow(
		"SELECT COUNT(*) FROM memory_jobs WHERE session_id = ?", sessionID.String(),
	).Scan(&count); err != nil {
		t.Fatalf("count jobs: %v", err)
	}
	if count != 0 {
		t.Fatalf("memory_jobs row survived DeleteSession (count=%d)", count)
	}
}

// TestRewindSessionResetsStaleMemoryJob is the M29 regression lock: a
// completed extraction whose extracted_version covers events the rewind
// deleted must re-enter the queue (its memories may describe turns that
// no longer exist), while an extraction that only observed retained
// events stays untouched.
func TestRewindSessionResetsStaleMemoryJob(t *testing.T) {
	ctx := context.Background()
	store := openMemoryJobTestStore(t)

	// Two checkpoints (seq 1 and 2) so the session can rewind from
	// version 2 to 1.
	setup := func(t *testing.T) domain.SessionID {
		t.Helper()
		sessionID := createMemoryJobSession(t, store)
		events1 := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
		if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, testCheckpoint(sessionID, 1, time.Now().UTC())); err != nil {
			t.Fatalf("first checkpoint: %v", err)
		}
		events2 := []domain.Event{newEvent(sessionID, 2, domain.EventUserMessageAdded, nil)}
		if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, testCheckpoint(sessionID, 2, time.Now().UTC())); err != nil {
			t.Fatalf("second checkpoint: %v", err)
		}
		return sessionID
	}
	extract := func(t *testing.T, sessionID domain.SessionID, extractedVersion int64) {
		t.Helper()
		if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
			t.Fatalf("enqueue: %v", err)
		}
		jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
		if err != nil || len(jobs) != 1 {
			t.Fatalf("claim: %v (n=%d)", err, len(jobs))
		}
		if err := store.CompleteMemoryJob(ctx, sessionID, jobs[0].ClaimToken, memory.JobSucceeded, extractedVersion); err != nil {
			t.Fatalf("complete: %v", err)
		}
	}
	jobState := func(t *testing.T, sessionID domain.SessionID) (string, int64) {
		t.Helper()
		var status string
		var extracted int64
		if err := store.db.QueryRowContext(ctx,
			"SELECT status, extracted_version FROM memory_jobs WHERE session_id = ?", sessionID.String()).
			Scan(&status, &extracted); err != nil {
			t.Fatalf("load job: %v", err)
		}
		return status, extracted
	}

	stale := setup(t) // extracted at version 2 — covers soon-to-be-deleted events
	extract(t, stale, 2)
	fresh := setup(t) // extracted at version 1 — retained events only
	extract(t, fresh, 1)

	if _, err := store.RewindSession(ctx, stale, 1); err != nil {
		t.Fatalf("rewind stale session: %v", err)
	}
	if _, err := store.RewindSession(ctx, fresh, 1); err != nil {
		t.Fatalf("rewind fresh session: %v", err)
	}

	if status, extracted := jobState(t, stale); status != string(memory.JobPending) || extracted != -1 {
		t.Fatalf("stale job = (%q, %d), want (pending, -1)", status, extracted)
	}
	if status, extracted := jobState(t, fresh); status != string(memory.JobSucceeded) || extracted != 1 {
		t.Fatalf("fresh job = (%q, %d), want (succeeded, 1)", status, extracted)
	}

	// The reset job is claimable again; the untouched one is not.
	jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
	if err != nil {
		t.Fatalf("claim after rewind: %v", err)
	}
	if len(jobs) != 1 || jobs[0].SessionID != stale {
		t.Fatalf("claimed after rewind = %+v, want only the rewound session's job", jobs)
	}
}
