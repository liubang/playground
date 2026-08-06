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
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

// Compile-time guarantee that the store satisfies the memory job queue.
var _ memory.JobQueue = (*SQLiteStore)(nil)

// memory_jobs schema (applied by the idempotent schema block in
// sqlite_store.go; bump sqliteSchemaVersion when changing):
//
//	memory_jobs(session_id PK, workspace_root, status, attempts,
//	            claim_token, claimed_at_unix_nano, next_retry_at_unix_nano,
//	            extracted_version, last_error, created/updated timestamps)
//	memory_phase2_lease(id PK CHECK id=1, claim_token, claimed_at_unix_nano)
//
// Claiming is a single UPDATE statement, which SQLite executes atomically
// even across processes sharing the database file — no BEGIN IMMEDIATE
// dance required.

// EnqueueMemoryJob implements memory.JobQueue.
func (s *SQLiteStore) EnqueueMemoryJob(ctx context.Context, sessionID domain.SessionID, workspaceRoot string) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	// Read the session's current event version so the upsert can skip
	// sessions already extracted at this version.
	var version int64
	if err := s.db.QueryRowContext(ctx,
		"SELECT version FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&version); err != nil {
		return storeError("load session version for memory enqueue", err)
	}
	now := time.Now().UTC()
	_, err := s.db.ExecContext(ctx, `
INSERT INTO memory_jobs(session_id, workspace_root, status, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano)
VALUES (?, ?, 'pending', ?, ?, ?, ?)
ON CONFLICT(session_id) DO UPDATE SET
    workspace_root = excluded.workspace_root,
    status = 'pending',
    claim_token = '',
    claimed_at_unix_nano = 0,
    next_retry_at_unix_nano = 0,
    attempts = 0,
    last_error = '',
    updated_at = excluded.updated_at,
    updated_at_unix_nano = excluded.updated_at_unix_nano
WHERE memory_jobs.extracted_version < ?`,
		sessionID.String(), workspaceRoot, formatTime(now), now.UnixNano(), formatTime(now), now.UnixNano(),
		version)
	if err != nil {
		return storeError("enqueue memory job", err)
	}
	return nil
}

// ClaimMemoryJobs implements memory.JobQueue.
func (s *SQLiteStore) ClaimMemoryJobs(ctx context.Context, limit int, minIdle, maxAge, lease time.Duration, maxAttempts int) ([]memory.Job, error) {
	if limit <= 0 {
		return nil, nil
	}
	if maxAttempts <= 0 {
		maxAttempts = 1
	}
	token := newClaimToken()
	now := time.Now().UTC()
	idleCutoff := now.Add(-minIdle).UnixNano()
	ageCutoff := now.Add(-maxAge).UnixNano()
	leaseExpiry := now.Add(-lease).UnixNano()

	// One atomic UPDATE: pending / retry-due failed / lease-expired claimed
	// jobs whose session is idle enough, young enough, and has new activity
	// beyond the last successful extraction. The attempts bound keeps
	// abandoned-but-not-yet-flipped rows out of the queue.
	res, err := s.db.ExecContext(ctx, `
UPDATE memory_jobs SET
    status = 'claimed',
    claim_token = ?,
    claimed_at_unix_nano = ?,
    updated_at = ?,
    updated_at_unix_nano = ?
WHERE session_id IN (
    SELECT j.session_id
    FROM memory_jobs j
    JOIN sessions s ON s.session_id = j.session_id
    WHERE (
        (j.status IN ('pending', 'failed') AND j.next_retry_at_unix_nano <= ?)
        OR (j.status = 'claimed' AND j.claimed_at_unix_nano <= ?)
    )
      AND j.attempts < ?
      AND j.extracted_version < s.version
      AND s.updated_at_unix_nano <= ?
      AND s.updated_at_unix_nano >= ?
    ORDER BY j.updated_at_unix_nano ASC
    LIMIT ?
)`,
		token, now.UnixNano(), formatTime(now), now.UnixNano(),
		now.UnixNano(), leaseExpiry, maxAttempts, idleCutoff, ageCutoff, limit)
	if err != nil {
		return nil, storeError("claim memory jobs", err)
	}
	if n, err := res.RowsAffected(); err != nil || n == 0 {
		return nil, nil
	}

	rows, err := s.db.QueryContext(ctx, `
SELECT j.session_id, j.workspace_root, s.version
FROM memory_jobs j
JOIN sessions s ON s.session_id = j.session_id
WHERE j.claim_token = ?`, token)
	if err != nil {
		return nil, storeError("load claimed memory jobs", err)
	}
	defer rows.Close()

	var jobs []memory.Job
	for rows.Next() {
		var rawID, workspaceRoot string
		var version int64
		if err := rows.Scan(&rawID, &workspaceRoot, &version); err != nil {
			return nil, storeError("scan claimed memory job", err)
		}
		sessionID, err := domain.ParseSessionID(rawID)
		if err != nil {
			return nil, storeError("decode memory job session ID", err)
		}
		jobs = append(jobs, memory.Job{
			SessionID:      sessionID,
			WorkspaceRoot:  workspaceRoot,
			ClaimToken:     token,
			SessionVersion: version,
		})
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate claimed memory jobs", err)
	}
	return jobs, nil
}

// CompleteMemoryJob implements memory.JobQueue.
func (s *SQLiteStore) CompleteMemoryJob(ctx context.Context, sessionID domain.SessionID, claimToken string, status memory.JobStatus, extractedVersion int64) error {
	if status != memory.JobSucceeded && status != memory.JobSucceededNoOutput {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("invalid terminal memory job status %q", status))
	}
	now := time.Now().UTC()
	res, err := s.db.ExecContext(ctx, `
UPDATE memory_jobs SET
    status = ?,
    claim_token = '',
    claimed_at_unix_nano = 0,
    extracted_version = ?,
    last_error = '',
    updated_at = ?,
    updated_at_unix_nano = ?
WHERE session_id = ? AND claim_token = ?`,
		string(status), extractedVersion, formatTime(now), now.UnixNano(),
		sessionID.String(), claimToken)
	if err != nil {
		return storeError("complete memory job", err)
	}
	if n, err := res.RowsAffected(); err != nil || n != 1 {
		return domain.NewError(domain.ErrConflict, "memory job claim token mismatch")
	}
	return nil
}

// FailMemoryJob implements memory.JobQueue.
func (s *SQLiteStore) FailMemoryJob(ctx context.Context, sessionID domain.SessionID, claimToken string, jobErr error, retryDelay time.Duration, maxAttempts int) error {
	if maxAttempts <= 0 {
		maxAttempts = 1
	}
	now := time.Now().UTC()
	errMsg := ""
	if jobErr != nil {
		errMsg = jobErr.Error()
		if len(errMsg) > 512 {
			errMsg = errMsg[:512]
		}
	}
	// Single statement: increment attempts, flip to abandoned once the
	// budget is exhausted, otherwise schedule a linear-backoff retry
	// (retryDelay * attempts).
	res, err := s.db.ExecContext(ctx, `
UPDATE memory_jobs SET
    attempts = attempts + 1,
    status = CASE WHEN attempts + 1 >= ? THEN 'abandoned' ELSE 'failed' END,
    claim_token = '',
    claimed_at_unix_nano = 0,
    next_retry_at_unix_nano = CASE WHEN attempts + 1 >= ? THEN 0 ELSE ? + ? * (attempts + 1) END,
    last_error = ?,
    updated_at = ?,
    updated_at_unix_nano = ?
WHERE session_id = ? AND claim_token = ?`,
		maxAttempts, maxAttempts, now.UnixNano(), retryDelay.Nanoseconds(), errMsg,
		formatTime(now), now.UnixNano(), sessionID.String(), claimToken)
	if err != nil {
		return storeError("fail memory job", err)
	}
	if n, err := res.RowsAffected(); err != nil || n != 1 {
		return domain.NewError(domain.ErrConflict, "memory job claim token mismatch")
	}
	return nil
}

// AcquirePhase2Lease implements memory.JobQueue.
func (s *SQLiteStore) AcquirePhase2Lease(ctx context.Context, token string, lease time.Duration) (bool, error) {
	if token == "" {
		return false, domain.NewError(domain.ErrInvalidInput, "phase2 lease token is required")
	}
	now := time.Now().UTC()
	expiry := now.Add(-lease).UnixNano()
	res, err := s.db.ExecContext(ctx, `
INSERT INTO memory_phase2_lease(id, claim_token, claimed_at_unix_nano)
VALUES (1, ?, ?)
ON CONFLICT(id) DO UPDATE SET
    claim_token = excluded.claim_token,
    claimed_at_unix_nano = excluded.claimed_at_unix_nano
WHERE memory_phase2_lease.claimed_at_unix_nano <= ?`,
		token, now.UnixNano(), expiry)
	if err != nil {
		return false, storeError("acquire phase2 lease", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return false, storeError("inspect phase2 lease acquisition", err)
	}
	return n == 1, nil
}

// ReleasePhase2Lease implements memory.JobQueue.
func (s *SQLiteStore) ReleasePhase2Lease(ctx context.Context, token string) error {
	_, err := s.db.ExecContext(ctx,
		"UPDATE memory_phase2_lease SET claim_token = '', claimed_at_unix_nano = 0 WHERE id = 1 AND claim_token = ?",
		token)
	if err != nil {
		return storeError("release phase2 lease", err)
	}
	return nil
}

func newClaimToken() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand never fails on supported platforms; fall back to a
		// time-based token rather than panicking in a background worker.
		return fmt.Sprintf("t%d", time.Now().UTC().UnixNano())
	}
	return hex.EncodeToString(b[:])
}
