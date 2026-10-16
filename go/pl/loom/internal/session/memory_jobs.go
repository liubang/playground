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
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

// Compile-time guarantee that the store satisfies the memory job queue.
var _ memory.JobQueue = (*SQLiteStore)(nil)

// memory_jobs schema (applied by the idempotent schema block in
// sqlite_store.go; bump sqliteSchemaVersion when changing):
//
//	memory_jobs(session_id PK, workspace_root, status, attempts, attempt_version,
//	            claim_token, claimed_at_unix_nano, next_retry_at_unix_nano,
//	            extracted_version, last_error, created/updated timestamps)
//	memory_phase2_lease(id PK CHECK id=1, claim_token, claimed_at_unix_nano)
//
// Claiming and loading its evidence boundaries share a write transaction,
// so other processes cannot change the returned version between the two.

// EnqueueMemoryJob implements memory.JobQueue.
func (s *SQLiteStore) EnqueueMemoryJob(ctx context.Context, sessionID domain.SessionID, workspaceRoot string) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	now := time.Now().UTC()
	_, err := s.db.ExecContext(ctx, `
INSERT INTO memory_jobs(session_id, workspace_root, status, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano)
SELECT session_id, ?, 'pending', ?, ?, ?, ? FROM sessions WHERE session_id = ?
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
WHERE memory_jobs.extracted_version < (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id)
 AND (memory_jobs.status IN ('succeeded', 'succeeded_no_output')
 OR (memory_jobs.status IN ('failed', 'abandoned') AND memory_jobs.attempt_version < (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id)))`,
		workspaceRoot, formatTime(now), now.UnixNano(), formatTime(now), now.UnixNano(), sessionID.String())
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

	// Claim and pin the attempt version in one transaction. Failures retain
	// backoff/budget at the same version; newer evidence gets a fresh budget,
	// including abandoned jobs. Neither path bypasses idle/age or active leases.
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, storeError("begin memory claim", err)
	}
	defer tx.Rollback()
	res, err := tx.ExecContext(ctx, `
UPDATE memory_jobs SET
    status = 'claimed',
    attempts = CASE WHEN attempt_version <> (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id) THEN 0 ELSE attempts END,
    attempt_version = (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id),
    claim_token = ?,
    claimed_at_unix_nano = ?,
    claimed_evidence_cutoff_unix_nano = (SELECT COALESCE(MAX(evidence_cutoff_unix_nano), 0) FROM memory_scope_state WHERE scope = '' OR scope = memory_jobs.workspace_root),
    updated_at = ?,
    updated_at_unix_nano = ?
WHERE session_id IN (
    SELECT j.session_id
    FROM memory_jobs j
    JOIN sessions s ON s.session_id = j.session_id
    WHERE (
        (j.status IN ('pending', 'failed') AND j.next_retry_at_unix_nano <= ?)
        OR (j.status = 'failed' AND s.version > j.attempt_version)
        OR (j.status = 'abandoned' AND s.version > j.attempt_version)
        OR (j.status = 'claimed' AND j.claimed_at_unix_nano <= ?)
    )
      AND (j.attempts < ? OR s.version <> j.attempt_version)
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
	if n, err := res.RowsAffected(); err != nil {
		return nil, storeError("inspect memory claim", err)
	} else if n == 0 {
		return nil, nil
	}

	rows, err := tx.QueryContext(ctx, `
SELECT j.session_id, j.workspace_root, j.attempt_version, j.extracted_version
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
		var version, extractedVersion int64
		if err := rows.Scan(&rawID, &workspaceRoot, &version, &extractedVersion); err != nil {
			return nil, storeError("scan claimed memory job", err)
		}
		sessionID, err := domain.ParseSessionID(rawID)
		if err != nil {
			return nil, storeError("decode memory job session ID", err)
		}
		jobs = append(jobs, memory.Job{
			SessionID:        sessionID,
			WorkspaceRoot:    workspaceRoot,
			ClaimToken:       token,
			SessionVersion:   version,
			ExtractedVersion: extractedVersion,
		})
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate claimed memory jobs", err)
	}
	rows.Close()
	if err := tx.Commit(); err != nil {
		return nil, storeError("commit memory claim", err)
	}
	return jobs, nil
}

// FailMemoryJob settles only the matched versioned claim. Changed evidence is
// rescheduled without charging the provider-failure budget for that version.
func (s *SQLiteStore) FailMemoryJob(ctx context.Context, job memory.Job, jobErr error, retryDelay time.Duration, maxAttempts int) error {
	maxAttempts = max(1, maxAttempts)
	retryDelay = max(0, retryDelay)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin memory failure settlement", err)
	}
	defer tx.Rollback()
	var version, extracted, claimedCutoff, cutoff int64
	var attempts int
	err = tx.QueryRowContext(ctx, `SELECT s.version, j.extracted_version, j.attempts,
 j.claimed_evidence_cutoff_unix_nano,
 (SELECT COALESCE(MAX(evidence_cutoff_unix_nano), 0) FROM memory_scope_state WHERE scope = '' OR scope = j.workspace_root)
 FROM memory_jobs j JOIN sessions s ON s.session_id = j.session_id
 WHERE j.session_id = ? AND j.status = 'claimed' AND j.claim_token = ? AND j.claim_token <> ''
 AND j.attempt_version = ? AND j.workspace_root = ?`,
		job.SessionID.String(), job.ClaimToken, job.SessionVersion, job.WorkspaceRoot).
		Scan(&version, &extracted, &attempts, &claimedCutoff, &cutoff)
	if errors.Is(err, sql.ErrNoRows) {
		return domain.NewError(domain.ErrConflict, "memory job claim token or version mismatch")
	}
	if err != nil {
		return storeError("read memory failure claim", err)
	}
	agentErr, isAgentErr := errors.AsType[*domain.AgentError](jobErr)
	conflict := isAgentErr && agentErr.Code == domain.ErrConflict
	conflict = conflict || version != job.SessionVersion || extracted != job.ExtractedVersion || cutoff != claimedCutoff
	now := time.Now().UTC()
	status := memory.JobPending
	nextRetry := now.Add(min(retryDelay, time.Second)).UnixNano()
	if !conflict {
		attempts++
		status = memory.JobFailed
		nextRetry = now.Add(retryDelay * time.Duration(attempts)).UnixNano()
		if attempts >= maxAttempts {
			status = memory.JobAbandoned
			nextRetry = 0
		}
	}
	errMsg := ""
	if jobErr != nil {
		errMsg = memory.RedactSecrets(jobErr.Error())
		if len(errMsg) > 512 {
			end := 512
			for end > 0 && !utf8.RuneStart(errMsg[end]) {
				end--
			}
			errMsg = errMsg[:end]
		}
	}
	res, err := tx.ExecContext(ctx, `UPDATE memory_jobs SET attempts = ?, status = ?,
 claim_token = '', claimed_at_unix_nano = 0, next_retry_at_unix_nano = ?,
 last_error = ?, updated_at = ?, updated_at_unix_nano = ?
 WHERE session_id = ? AND claim_token = ? AND status = 'claimed' AND attempt_version = ?`,
		attempts, status, nextRetry, errMsg, formatTime(now), now.UnixNano(),
		job.SessionID.String(), job.ClaimToken, job.SessionVersion)
	if err != nil {
		return storeError("settle memory failure", err)
	}
	if n, err := res.RowsAffected(); err != nil {
		return storeError("inspect memory failure settlement", err)
	} else if n != 1 {
		return domain.NewError(domain.ErrConflict, "memory job claim changed during settlement")
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit memory failure settlement", err)
	}
	if conflict {
		return domain.NewError(domain.ErrConflict, "memory evidence changed; claim rescheduled")
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

// validateMemoryClaim fences both the worker lease and the evidence boundary.
func validateMemoryClaim(ctx context.Context, tx *sql.Tx, job memory.Job) error {
	if job.ClaimToken == "" || job.SessionVersion < 0 || job.ExtractedVersion >= job.SessionVersion {
		return domain.NewError(domain.ErrConflict, "invalid memory claim or evidence boundary")
	}
	var exists int
	err := tx.QueryRowContext(ctx, `SELECT 1 FROM memory_jobs j JOIN sessions s ON s.session_id = j.session_id
 WHERE j.session_id = ? AND j.status = 'claimed' AND j.claim_token = ? AND s.version = ? AND j.extracted_version = ? AND j.workspace_root = ?
 AND j.attempt_version = s.version
 AND j.claimed_evidence_cutoff_unix_nano = (SELECT COALESCE(MAX(evidence_cutoff_unix_nano), 0) FROM memory_scope_state WHERE scope = '' OR scope = j.workspace_root)`,
		job.SessionID.String(), job.ClaimToken, job.SessionVersion, job.ExtractedVersion, job.WorkspaceRoot).Scan(&exists)
	if errors.Is(err, sql.ErrNoRows) {
		return domain.NewError(domain.ErrConflict, "memory claim or session version changed")
	}
	if err != nil {
		return storeError("validate memory claim", err)
	}
	return nil
}

func memoryEvidenceCutoff(ctx context.Context, tx *sql.Tx, scope string) (int64, error) {
	var cutoff int64
	err := tx.QueryRowContext(ctx, `SELECT COALESCE(MAX(evidence_cutoff_unix_nano), 0) FROM memory_scope_state WHERE scope = '' OR scope = ?`, scope).Scan(&cutoff)
	if err != nil {
		return 0, storeError("read memory evidence cutoff", err)
	}
	return cutoff, nil
}

// After an explicit replacement/revocation only fresh user-authored messages
// are evidence. Assistant/tool messages can echo forgotten history even after
// the barrier; admitting them would defeat forgetting without lexical matching.
func filterMemoryEvidence(events []domain.Event, cutoff int64) []domain.Event {
	if cutoff == 0 {
		return events
	}
	var fresh []domain.Event
	for _, event := range events {
		if event.Type == domain.EventUserMessageAdded && event.Timestamp.UnixNano() > cutoff {
			fresh = append(fresh, event)
		}
	}
	return fresh
}

func hasPostCutoffUserEvidence(ctx context.Context, tx *sql.Tx, job memory.Job, cutoff int64) (bool, error) {
	rows, err := tx.QueryContext(ctx, `SELECT timestamp FROM events WHERE session_id = ? AND sequence > ? AND sequence <= ? AND type = ?`, job.SessionID.String(), max(job.ExtractedVersion, 0), job.SessionVersion, domain.EventUserMessageAdded)
	if err != nil {
		return false, storeError("read post-forget evidence", err)
	}
	defer rows.Close()
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			return false, storeError("scan post-forget evidence", err)
		}
		timestamp, err := time.Parse(time.RFC3339Nano, raw)
		if err != nil {
			return false, storeError("decode post-forget evidence time", err)
		}
		if timestamp.UnixNano() > cutoff {
			return true, nil
		}
	}
	return false, rows.Err()
}

// LoadMemoryEvidence reads raw events, never the model's compressed checkpoint.
func (s *SQLiteStore) LoadMemoryEvidence(ctx context.Context, job memory.Job) ([]domain.Message, error) {
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return nil, storeError("begin memory evidence", err)
	}
	defer tx.Rollback()
	if err := validateMemoryClaim(ctx, tx, job); err != nil {
		return nil, err
	}
	lower := max(job.ExtractedVersion, 0)
	rows, err := tx.QueryContext(ctx, `SELECT event_id, sequence, type, timestamp, payload, ignorable FROM events WHERE session_id = ? AND sequence > ? AND sequence <= ? ORDER BY sequence`, job.SessionID.String(), lower, job.SessionVersion)
	if err != nil {
		return nil, storeError("load memory evidence events", err)
	}
	events, err := scanEventsAfter(rows, job.SessionID, lower)
	if err != nil {
		return nil, err
	}
	if int64(len(events)) != job.SessionVersion-lower {
		return nil, domain.NewError(domain.ErrConflict, "memory evidence event range is incomplete")
	}
	cutoff, err := memoryEvidenceCutoff(ctx, tx, job.WorkspaceRoot)
	if err != nil {
		return nil, err
	}
	events = filterMemoryEvidence(events, cutoff)
	var messages []domain.Message
	if len(events) > 0 {
		transcript, err := ReplayFull(events)
		if err != nil {
			return nil, storeError("replay memory evidence", err)
		}
		messages = transcript.Messages
	}
	if err := tx.Commit(); err != nil {
		return nil, storeError("finish memory evidence", err)
	}
	return messages, nil
}

// CompleteMemoryExtraction commits append-only automatic evidence and the
// extraction watermark in one transaction. Automatic records never replace
// explicit evidence (or earlier extraction increments).
func (s *SQLiteStore) CompleteMemoryExtraction(ctx context.Context, job memory.Job, result *memory.ExtractionResult) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin memory extraction completion", err)
	}
	defer tx.Rollback()
	if err := validateMemoryClaim(ctx, tx, job); err != nil {
		return err
	}
	cutoff, err := memoryEvidenceCutoff(ctx, tx, job.WorkspaceRoot)
	if err != nil {
		return err
	}
	if result != nil && cutoff > 0 {
		// A claim over old events can settle its watermark, but cannot publish
		// arbitrary output when its entire authoritative evidence was forgotten.
		eligible, err := hasPostCutoffUserEvidence(ctx, tx, job, cutoff)
		if err != nil {
			return err
		}
		if !eligible {
			result = nil
		}
	}
	status := memory.JobSucceededNoOutput
	now := time.Now().UTC()
	if result != nil {
		content := strings.TrimSpace(memory.RedactSecrets(result.RawMemory))
		rollout := strings.TrimSpace(memory.RedactSecrets(result.RolloutSummary))
		if !utf8.ValidString(content) || !utf8.ValidString(rollout) {
			return domain.NewError(domain.ErrInvalidInput, "invalid UTF-8 memory extraction")
		}
		if len(content) > memory.MaxExtractionFieldBytes || len(rollout) > memory.MaxExtractionFieldBytes {
			return domain.NewError(domain.ErrInvalidInput, "memory extraction field exceeds size limit")
		}
		if content != "" || rollout != "" {
			// The record boundary includes both the previous watermark and this
			// claimed version, so retry/rewind cannot overwrite a different increment.
			id := memoryID("automatic:" + job.SessionID.String() + ":" + fmt.Sprint(job.ExtractedVersion) + ":" + fmt.Sprint(job.SessionVersion))
			sequence, err := advanceMemoryScope(ctx, tx, job.WorkspaceRoot, false)
			if err != nil {
				return err
			}
			_, err = tx.ExecContext(ctx, `INSERT INTO memory_records (`+memoryRecordColumns+`) VALUES (?, ?, 'automatic', ?, ?, 1, ?, ?, ?, ?)`,
				id, job.WorkspaceRoot, content, rollout, sequence, job.SessionID.String(), job.SessionVersion, formatTime(now))
			if err != nil {
				return storeError("save automatic memory increment", err)
			}
			status = memory.JobSucceeded
		}
	}
	_, err = tx.ExecContext(ctx, `UPDATE memory_jobs SET status = ?, extracted_version = ?, claim_token = '', claimed_at_unix_nano = 0, last_error = '', updated_at = ?, updated_at_unix_nano = ? WHERE session_id = ?`, status, job.SessionVersion, formatTime(now), now.UnixNano(), job.SessionID.String())
	if err != nil {
		return storeError("complete memory extraction job", err)
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit memory extraction completion", err)
	}
	return nil
}

// BackfillMemoryJobs only queues recent idle top-level sessions with raw events;
// model execution remains the responsibility of claimed background workers.
func (s *SQLiteStore) BackfillMemoryJobs(ctx context.Context, maxAge time.Duration) error {
	now := time.Now().UTC()
	_, err := s.db.ExecContext(ctx, `INSERT INTO memory_jobs(session_id, workspace_root, status, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano)
 SELECT s.session_id, COALESCE(w.root_path, ''), 'pending', ?, ?, ?, ? FROM sessions s
 LEFT JOIN workspaces w ON w.workspace_id = s.workspace_id
 WHERE s.updated_at_unix_nano >= ? AND s.updated_at_unix_nano <= ?
 AND EXISTS (SELECT 1 FROM events e WHERE e.session_id = s.session_id AND e.sequence = s.version)
 AND NOT EXISTS (SELECT 1 FROM events e WHERE e.session_id = s.session_id AND e.type = ? AND COALESCE(json_extract(e.payload, '$.parent_session_id'), '') <> '')
 ON CONFLICT(session_id) DO UPDATE SET workspace_root = excluded.workspace_root, status = 'pending', attempts = 0, next_retry_at_unix_nano = 0, last_error = '', updated_at = excluded.updated_at, updated_at_unix_nano = excluded.updated_at_unix_nano
 WHERE memory_jobs.extracted_version < (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id)
 AND (memory_jobs.status IN ('succeeded', 'succeeded_no_output')
 OR (memory_jobs.status IN ('failed', 'abandoned') AND memory_jobs.attempt_version < (SELECT version FROM sessions WHERE session_id = memory_jobs.session_id)))`,
		formatTime(now), now.UnixNano(), formatTime(now), now.UnixNano(), now.Add(-maxAge).UnixNano(), now.Add(-time.Minute).UnixNano(), domain.EventRunCreated)
	if err != nil {
		return storeError("backfill memory jobs", err)
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
