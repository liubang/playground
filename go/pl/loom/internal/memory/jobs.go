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

package memory

import (
	"context"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// JobStatus identifies the lifecycle state of a Phase 1 extraction job.
type JobStatus string

const (
	// JobPending waits to be claimed by a pipeline pass.
	JobPending JobStatus = "pending"
	// JobClaimed is being processed by a pipeline worker (leased).
	JobClaimed JobStatus = "claimed"
	// JobFailed failed with a retryable error; eligible again once its
	// backoff (next_retry_at) expires.
	JobFailed JobStatus = "failed"
	// JobAbandoned exhausted its retry budget and is never claimed again.
	JobAbandoned JobStatus = "abandoned"
	// JobSucceeded produced a memory artifact.
	JobSucceeded JobStatus = "succeeded"
	// JobSucceededNoOutput ran cleanly but produced no useful memory.
	JobSucceededNoOutput JobStatus = "succeeded_no_output"
)

// Job is one claimed Phase 1 extraction job. The claim token proves
// ownership: completion/failure updates carrying a stale token are
// rejected, so a crashed worker's expired lease can be reclaimed safely.
type Job struct {
	SessionID     domain.SessionID
	WorkspaceRoot string
	ClaimToken    string
	// SessionVersion is the sessions table event-version observed at claim
	// time; it is recorded on success so a session only re-extracts after
	// new activity.
	SessionVersion int64
}

// JobQueue persists Phase 1 extraction jobs and the Phase 2 global lease.
// *session.SQLiteStore implements it; the interface keeps the pipeline
// testable without a database.
//
// All claim/complete operations must be atomic across processes: several
// loom processes (TUI, serve) share one database file and may run pipeline
// passes concurrently.
type JobQueue interface {
	// EnqueueMemoryJob marks a session for extraction. It is idempotent
	// and cheap (a single upsert): a session already extracted at its
	// current event version is left untouched.
	EnqueueMemoryJob(ctx context.Context, sessionID domain.SessionID, workspaceRoot string) error
	// ClaimMemoryJobs atomically leases up to limit eligible jobs
	// (pending, retry-due failed, or lease-expired claimed) whose session
	// has been idle for at least minIdle, is no older than maxAge, and has
	// activity beyond the last successful extraction.
	ClaimMemoryJobs(ctx context.Context, limit int, minIdle, maxAge, lease time.Duration, maxAttempts int) ([]Job, error)
	// CompleteMemoryJob marks a claimed job with a terminal status
	// (JobSucceeded or JobSucceededNoOutput) and records the extracted
	// session version. The claim token must match.
	CompleteMemoryJob(ctx context.Context, sessionID domain.SessionID, claimToken string, status JobStatus, extractedVersion int64) error
	// FailMemoryJob records a retryable failure: attempts is incremented
	// and the job either re-enters the queue after retryDelay*attempts or
	// is abandoned once maxAttempts is reached. The claim token must match.
	FailMemoryJob(ctx context.Context, sessionID domain.SessionID, claimToken string, jobErr error, retryDelay time.Duration, maxAttempts int) error
	// AcquirePhase2Lease takes the global Phase 2 lease; false means
	// another (possibly crashed) process still holds it within its lease.
	AcquirePhase2Lease(ctx context.Context, token string, lease time.Duration) (bool, error)
	// ReleasePhase2Lease releases the global Phase 2 lease if still held
	// by token.
	ReleasePhase2Lease(ctx context.Context, token string) error
}
