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
	"fmt"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

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
	if err := store.CompleteMemoryExtraction(ctx, jobs[0], nil); err != nil {
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
	if err := store.CompleteMemoryExtraction(ctx, jobs[0], nil); err == nil {
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
	if err := store.FailMemoryJob(ctx, jobs[0], errors.New("model exploded"), time.Hour, 2); err != nil {
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
	if err := store.FailMemoryJob(ctx, jobs[0], errors.New("boom again"), time.Hour, 2); err != nil {
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

type memoryJobRetryState struct {
	status    string
	token     string
	attempts  int
	version   int64
	nextRetry int64
}

func readMemoryJobRetryState(t *testing.T, store *SQLiteStore, id domain.SessionID) memoryJobRetryState {
	t.Helper()
	var state memoryJobRetryState
	if err := store.db.QueryRowContext(t.Context(), `SELECT status, claim_token, attempts, attempt_version, next_retry_at_unix_nano FROM memory_jobs WHERE session_id = ?`, id.String()).Scan(&state.status, &state.token, &state.attempts, &state.version, &state.nextRetry); err != nil {
		t.Fatal(err)
	}
	return state
}

func forceMemoryRetryDue(t *testing.T, store *SQLiteStore, id domain.SessionID) {
	t.Helper()
	if _, err := store.db.ExecContext(t.Context(), "UPDATE memory_jobs SET next_retry_at_unix_nano = 0 WHERE session_id = ?", id.String()); err != nil {
		t.Fatal(err)
	}
}

func TestMemoryConflictDoesNotExhaustVersionBudget(t *testing.T) {
	for _, conflictKind := range []string{"wrapped", "version", "cutoff"} {
		t.Run(conflictKind, func(t *testing.T) {
			store := openMemoryJobTestStore(t)
			id := createMemoryJobSession(t, store)
			job := claimTestMemoryJob(t, store, id, "/ws")
			for range 8 {
				jobErr := errors.New("provider error with changed evidence")
				switch conflictKind {
				case "wrapped":
					jobErr = fmt.Errorf("evidence read: %w", domain.NewError(domain.ErrConflict, "changed"))
				case "version":
					bumpSessionVersion(t, store, id, job.SessionVersion+1)
				case "cutoff":
					if _, err := store.db.ExecContext(t.Context(), `INSERT INTO memory_scope_state(scope, version, epoch, evidence_cutoff_unix_nano) VALUES ('/ws', 0, 0, 1) ON CONFLICT(scope) DO UPDATE SET evidence_cutoff_unix_nano = evidence_cutoff_unix_nano + 1`); err != nil {
						t.Fatal(err)
					}
				}
				before := time.Now()
				if err := store.FailMemoryJob(t.Context(), job, jobErr, time.Hour, 1); errorCode(err) != domain.ErrConflict {
					t.Fatalf("conflict settlement=%v", err)
				}
				state := readMemoryJobRetryState(t, store, id)
				if state.status != "pending" || state.token != "" || state.attempts != 0 || state.version != job.SessionVersion || state.nextRetry <= before.UnixNano() || state.nextRetry > time.Now().Add(time.Second).UnixNano() {
					t.Fatalf("conflict consumed budget or lost short backoff: %+v", state)
				}
				if err := store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
					t.Fatal(err)
				}
				if after := readMemoryJobRetryState(t, store, id); after != state {
					t.Fatalf("enqueue erased conflict backoff: %+v -> %+v", state, after)
				}
				if jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1); err != nil || len(jobs) != 0 {
					t.Fatalf("conflict retry ignored short backoff: jobs=%v err=%v", jobs, err)
				}
				forceMemoryRetryDue(t, store, id)
				jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1)
				if err != nil || len(jobs) != 1 {
					t.Fatalf("conflict exhausted budget: jobs=%v err=%v", jobs, err)
				}
				job = jobs[0]
			}
			if err := store.FailMemoryJob(t.Context(), job, errors.New("real provider failure"), time.Hour, 1); err != nil {
				t.Fatal(err)
			}
			state := readMemoryJobRetryState(t, store, id)
			if state.attempts != 1 || state.status != "abandoned" {
				t.Fatalf("provider failure not charged: %+v", state)
			}
		})
	}
}

func TestMemoryConflictPreservesExistingProviderAttempts(t *testing.T) {
	store := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, store)
	job := claimTestMemoryJob(t, store, id, "/ws")
	if err := store.FailMemoryJob(t.Context(), job, errors.New("provider failed"), 0, 2); err != nil {
		t.Fatal(err)
	}
	jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 2)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("claim=%v err=%v", jobs, err)
	}
	for range 5 {
		if err := store.FailMemoryJob(t.Context(), jobs[0], domain.NewError(domain.ErrConflict, "cutoff changed"), 0, 2); errorCode(err) != domain.ErrConflict {
			t.Fatal(err)
		}
		if state := readMemoryJobRetryState(t, store, id); state.attempts != 1 || state.status != "pending" {
			t.Fatalf("conflict changed provider budget: %+v", state)
		}
		jobs, err = store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 2)
		if err != nil || len(jobs) != 1 {
			t.Fatalf("claim=%v err=%v", jobs, err)
		}
	}
	if err := store.FailMemoryJob(t.Context(), jobs[0], errors.New("provider failed again"), 0, 2); err != nil {
		t.Fatal(err)
	}
	if state := readMemoryJobRetryState(t, store, id); state.attempts != 2 || state.status != "abandoned" {
		t.Fatalf("same-version budget reset: %+v", state)
	}
}

func TestMemoryStaleFailureCannotTouchNewClaim(t *testing.T) {
	store := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, store)
	old := claimTestMemoryJob(t, store, id, "/ws")
	if _, err := store.db.ExecContext(t.Context(), "UPDATE memory_jobs SET claimed_at_unix_nano = 0 WHERE session_id = ?", id.String()); err != nil {
		t.Fatal(err)
	}
	bumpSessionVersion(t, store, id, 1)
	jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 2)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("reclaim=%v err=%v", jobs, err)
	}
	before := readMemoryJobRetryState(t, store, id)
	for _, failure := range []error{errors.New("old provider failure"), domain.NewError(domain.ErrConflict, "old evidence")} {
		if err := store.FailMemoryJob(t.Context(), old, failure, time.Hour, 1); errorCode(err) != domain.ErrConflict {
			t.Fatalf("stale failure=%v", err)
		}
		if after := readMemoryJobRetryState(t, store, id); after != before {
			t.Fatalf("old worker changed new claim: %+v -> %+v", before, after)
		}
	}
	forged := jobs[0]
	forged.SessionVersion = old.SessionVersion
	if err := store.FailMemoryJob(t.Context(), forged, errors.New("wrong version"), 0, 1); errorCode(err) != domain.ErrConflict {
		t.Fatal(err)
	}
	if after := readMemoryJobRetryState(t, store, id); after != before {
		t.Fatal("correct token with wrong version modified claim")
	}
}

func TestMemoryNewVersionGetsFreshFailureBudget(t *testing.T) {
	for _, initialStatus := range []string{"failed", "abandoned"} {
		for _, reopen := range []string{"claim", "enqueue", "backfill"} {
			t.Run(initialStatus+"/"+reopen, func(t *testing.T) {
				store := openMemoryJobTestStore(t)
				id := createMemoryJobSession(t, store)
				job := claimTestMemoryJob(t, store, id, "/ws")
				budget := 2
				if initialStatus == "abandoned" {
					budget = 1
				}
				if err := store.FailMemoryJob(t.Context(), job, errors.New("provider failure"), time.Hour, budget); err != nil {
					t.Fatal(err)
				}
				before := readMemoryJobRetryState(t, store, id)
				// Same-version enqueue must preserve both failure and abandonment.
				if err := store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
					t.Fatal(err)
				}
				if after := readMemoryJobRetryState(t, store, id); after != before {
					t.Fatalf("same-version enqueue reset budget: %+v", after)
				}
				if jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, budget); err != nil || len(jobs) != 0 {
					t.Fatalf("same version reclaimed=%v err=%v", jobs, err)
				}
				if err := store.AppendEvents(t.Context(), id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}); err != nil {
					t.Fatal(err)
				}
				ageSession(t, store, id, 2*time.Hour)
				switch reopen {
				case "enqueue":
					if err := store.EnqueueMemoryJob(t.Context(), id, "/ws"); err != nil {
						t.Fatal(err)
					}
				case "backfill":
					if err := store.BackfillMemoryJobs(t.Context(), 24*time.Hour); err != nil {
						t.Fatal(err)
					}
				}
				jobs, err := store.ClaimMemoryJobs(t.Context(), 1, time.Hour, 24*time.Hour, time.Hour, budget)
				if err != nil || len(jobs) != 1 || jobs[0].SessionVersion != 1 {
					t.Fatalf("new version not reopened: jobs=%v err=%v", jobs, err)
				}
				state := readMemoryJobRetryState(t, store, id)
				if state.attempts != 0 || state.version != 1 {
					t.Fatalf("old budget leaked to new version: %+v", state)
				}
				if err := store.FailMemoryJob(t.Context(), jobs[0], errors.New("new version provider failure"), time.Hour, 2); err != nil {
					t.Fatal(err)
				}
				if state := readMemoryJobRetryState(t, store, id); state.attempts != 1 || state.status != "failed" || state.version != 1 {
					t.Fatalf("new version budget incorrect: %+v", state)
				}
			})
		}
	}
}

func TestMemoryRetryStillRespectsAgeAndIdle(t *testing.T) {
	store := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, store)
	job := claimTestMemoryJob(t, store, id, "/ws")
	if err := store.FailMemoryJob(t.Context(), job, errors.New("provider failed"), 0, 1); err != nil {
		t.Fatal(err)
	}
	bumpSessionVersion(t, store, id, 1)
	for _, age := range []time.Duration{0, 48 * time.Hour} {
		ageSession(t, store, id, age)
		jobs, err := store.ClaimMemoryJobs(t.Context(), 1, time.Hour, 24*time.Hour, time.Hour, 1)
		if err != nil || len(jobs) != 0 {
			t.Fatalf("new-version retry bypassed age/idle: age=%v jobs=%v err=%v", age, jobs, err)
		}
	}
	ageSession(t, store, id, 2*time.Hour)
	jobs, err := store.ClaimMemoryJobs(t.Context(), 1, time.Hour, 24*time.Hour, time.Hour, 1)
	if err != nil || len(jobs) != 1 {
		t.Fatalf("eligible retry=%v err=%v", jobs, err)
	}
}

func TestMemoryActiveClaimSurvivesNewVersionEnqueueAndBackfill(t *testing.T) {
	store := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, store)
	job := claimTestMemoryJob(t, store, id, "/ws")
	if err := store.AppendEvents(t.Context(), id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}); err != nil {
		t.Fatal(err)
	}
	ageSession(t, store, id, 2*time.Hour)
	before := readMemoryJobRetryState(t, store, id)
	if err := store.EnqueueMemoryJob(t.Context(), id, "/other"); err != nil {
		t.Fatal(err)
	}
	if err := store.BackfillMemoryJobs(t.Context(), 24*time.Hour); err != nil {
		t.Fatal(err)
	}
	if after := readMemoryJobRetryState(t, store, id); after != before {
		t.Fatalf("new activity replaced active lease: %+v -> %+v", before, after)
	}
	if jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1); err != nil || len(jobs) != 0 {
		t.Fatalf("new version bypassed active lease: jobs=%v err=%v", jobs, err)
	}
	if err := store.FailMemoryJob(t.Context(), job, errors.New("old provider error"), 0, 1); errorCode(err) != domain.ErrConflict {
		t.Fatalf("old provider settlement=%v", err)
	}
	jobs, err := store.ClaimMemoryJobs(t.Context(), 1, 0, 24*time.Hour, time.Hour, 1)
	if err != nil || len(jobs) != 1 || jobs[0].SessionVersion != 1 {
		t.Fatalf("released new version=%v err=%v", jobs, err)
	}
	if state := readMemoryJobRetryState(t, store, id); state.attempts != 0 {
		t.Fatalf("old provider exhausted new version: %+v", state)
	}
}

func TestBackfillPreservesSameVersionFailureAndClaim(t *testing.T) {
	store := openMemoryJobTestStore(t)
	id := createMemoryJobSession(t, store)
	if err := store.AppendEvents(t.Context(), id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}); err != nil {
		t.Fatal(err)
	}
	ageSession(t, store, id, 2*time.Hour)
	job := claimTestMemoryJob(t, store, id, "/ws")
	before := readMemoryJobRetryState(t, store, id)
	if err := store.BackfillMemoryJobs(t.Context(), 24*time.Hour); err != nil {
		t.Fatal(err)
	}
	if after := readMemoryJobRetryState(t, store, id); after != before {
		t.Fatal("backfill replaced active claim")
	}
	if err := store.FailMemoryJob(t.Context(), job, errors.New("provider failure"), time.Hour, 2); err != nil {
		t.Fatal(err)
	}
	before = readMemoryJobRetryState(t, store, id)
	for range 3 {
		if err := store.BackfillMemoryJobs(t.Context(), 24*time.Hour); err != nil {
			t.Fatal(err)
		}
		if after := readMemoryJobRetryState(t, store, id); after != before {
			t.Fatalf("backfill erased same-version failure: %+v", after)
		}
	}
}

func TestRewindMemoryFailedAttemptRetriesSuccessfully(t *testing.T) {
	for _, status := range []memory.JobStatus{memory.JobFailed, memory.JobAbandoned} {
		t.Run(string(status), func(t *testing.T) {
			store := openMemoryJobTestStore(t)
			ctx := t.Context()
			id := createMemoryJobSession(t, store)
			if err := store.AppendEventsAndCheckpoint(ctx, id, 0, []domain.Event{newEvent(id, 1, domain.EventSessionCreated, nil)}, testCheckpoint(id, 1, time.Now().UTC())); err != nil {
				t.Fatal(err)
			}
			seedMemoryUser(t, store, id, 2, "retained evidence", time.Now().UTC())
			if err := store.SaveCheckpoint(ctx, testCheckpoint(id, 2, time.Now().UTC())); err != nil {
				t.Fatal(err)
			}
			seedMemoryUser(t, store, id, 3, "discarded evidence", time.Now().UTC())
			job := claimTestMemoryJob(t, store, id, "/ws")
			budget := 2
			if status == memory.JobAbandoned {
				budget = 1
			}
			if err := store.FailMemoryJob(ctx, job, errors.New("provider failed"), time.Hour, budget); err != nil {
				t.Fatal(err)
			}
			before := readMemoryJobRetryState(t, store, id)
			if before.status != string(status) || before.version != 3 || before.attempts != 1 {
				t.Fatalf("failed attempt=%+v", before)
			}
			if _, err := store.RewindSession(ctx, id, 2); err != nil {
				t.Fatal(err)
			}
			state := readMemoryJobRetryState(t, store, id)
			if state.status != "pending" || state.attempts != 0 || state.token != "" || state.nextRetry != 0 {
				t.Fatalf("rewind did not reset failed attempt: %+v", state)
			}
			jobs, err := store.ClaimMemoryJobs(ctx, 1, 0, 24*time.Hour, time.Hour, 1)
			if err != nil || len(jobs) != 1 || jobs[0].SessionVersion != 2 || jobs[0].ExtractedVersion != -1 {
				t.Fatalf("retry=%+v err=%v", jobs, err)
			}
			evidence, err := store.LoadMemoryEvidence(ctx, jobs[0])
			if err != nil || len(evidence) != 1 || strings.Join(evidence[0].TextParts(), "") != "retained evidence" {
				t.Fatalf("rewound evidence=%+v err=%v", evidence, err)
			}
			if err := store.CompleteMemoryExtraction(ctx, jobs[0], &memory.ExtractionResult{RawMemory: "retry succeeded"}); err != nil {
				t.Fatal(err)
			}
			snapshot := snapshotTestMemory(t, store, "/ws")
			if len(snapshot.Records) != 1 || snapshot.Records[0].SourceVersion != 2 || snapshot.Records[0].Content != "retry succeeded" {
				t.Fatalf("retry records=%+v", snapshot.Records)
			}
			var extracted int64
			var lastError string
			if err := store.db.QueryRowContext(ctx, "SELECT extracted_version, last_error FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&extracted, &lastError); err != nil {
				t.Fatal(err)
			}
			if state := readMemoryJobRetryState(t, store, id); state.status != "succeeded" || state.attempts != 0 || extracted != 2 || lastError != "" {
				t.Fatalf("retry settlement=%+v extracted=%d last_error=%q", state, extracted, lastError)
			}
		})
	}
}

func TestFailMemoryJobTruncatesLastErrorAtRuneBoundary(t *testing.T) {
	for _, prefixBytes := range []int{509, 510, 511, 512} {
		t.Run(fmt.Sprint(prefixBytes), func(t *testing.T) {
			store := openMemoryJobTestStore(t)
			id := createMemoryJobSession(t, store)
			job := claimTestMemoryJob(t, store, id, "/ws")
			prefix := strings.Repeat("x", prefixBytes)
			if err := store.FailMemoryJob(t.Context(), job, errors.New(prefix+"界tail"), 0, 2); err != nil {
				t.Fatal(err)
			}
			var lastError string
			if err := store.db.QueryRowContext(t.Context(), "SELECT last_error FROM memory_jobs WHERE session_id = ?", id.String()).Scan(&lastError); err != nil {
				t.Fatal(err)
			}
			want := prefix
			if prefixBytes == 509 {
				want += "界"
			}
			if !utf8.ValidString(lastError) || len(lastError) > 512 || lastError != want {
				t.Fatalf("last_error bytes=%d valid=%v want=%q got=%q", len(lastError), utf8.ValidString(lastError), want, lastError)
			}
		})
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
		bumpSessionVersion(t, store, sessionID, extractedVersion)
		if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
			t.Fatalf("enqueue: %v", err)
		}
		jobs, err := store.ClaimMemoryJobs(ctx, 8, 0, 30*24*time.Hour, time.Hour, 5)
		if err != nil || len(jobs) != 1 {
			t.Fatalf("claim: %v (n=%d)", err, len(jobs))
		}
		if err := store.CompleteMemoryExtraction(ctx, jobs[0], &memory.ExtractionResult{RawMemory: "extracted evidence"}); err != nil {
			t.Fatalf("complete: %v", err)
		}
		bumpSessionVersion(t, store, sessionID, 2)
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
