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
// Created: 2026/08/02

package session

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// MaxFileChangeSnapshotBytes bounds the inline before-content snapshot kept
// per file change. edit/write already cap their inputs at 1 MiB, so this
// ceiling only guards future writers; a larger file is recorded WITHOUT its
// content and is skipped (loudly) at rewind time.
const MaxFileChangeSnapshotBytes = 4 << 20

// FileChange is one recorded mutation of a workspace file: the state before
// a successful edit/write tool execution. Entries form an append-only
// ledger keyed by their rowid; each checkpoint snapshots the ledger
// position it covers, so rewinding to a checkpoint replays exactly the
// changes that happened after it.
type FileChange struct {
	ID            int64  `json:"id"`
	Path          string `json:"path"`
	BeforeExisted bool   `json:"before_existed"`
	BeforeHash    string `json:"before_hash"`
	// BeforeContent is the full pre-mutation content (nil when the file did
	// not exist, or when it exceeded MaxFileChangeSnapshotBytes).
	BeforeContent []byte `json:"before_content,omitempty"`
	AfterHash     string `json:"after_hash"`
	// Restorable is false when the content was not captured (oversized
	// file); rewind reports such paths instead of silently skipping them.
	Restorable bool `json:"restorable"`
	// LatestAfterHash is the content hash of the path's LAST recorded
	// change in the rewound range — the on-disk state a clean restore
	// expects to find. It is populated only on the deduplicated entries
	// of a RewindResult; a mismatch at restore time means the file was
	// modified outside the recorded history (a conflict to report, not
	// to silently clobber).
	LatestAfterHash string `json:"latest_after_hash,omitempty"`
}

// CheckpointSummary is the picker-facing projection of one persisted
// checkpoint: its event-log position plus a short human label derived
// from the most recent user message it covers.
type CheckpointSummary struct {
	ID        domain.CheckpointID `json:"id"`
	Sequence  int64               `json:"sequence"`
	CreatedAt time.Time           `json:"created_at"`
	// Label is the last user message text at the checkpoint (truncated);
	// empty when the checkpoint covers no user message.
	Label string `json:"label,omitempty"`
	// Turns is the number of user messages the checkpoint covers.
	Turns int `json:"turns"`
}

// RewindResult reports what RewindSession did: the checkpoint the session
// was rewound to and the file changes to restore, deduplicated by path
// keeping the EARLIEST post-checkpoint state per path. Callers apply the
// file restoration outside the store transaction.
type RewindResult struct {
	Checkpoint CheckpointSummary `json:"checkpoint"`
	Changes    []FileChange      `json:"changes"`
}

// RecordFileChange appends one file mutation to the session's change
// ledger. beforeContent may be nil (file did not exist, or was not
// captured); beforeExisted distinguishes the two cases.
func (s *SQLiteStore) RecordFileChange(ctx context.Context, sessionID domain.SessionID, path string, beforeExisted bool, beforeHash string, beforeContent []byte, afterHash string) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	path = strings.TrimSpace(path)
	if path == "" {
		return domain.NewError(domain.ErrInvalidInput, "file change path is required")
	}
	if len(beforeContent) > MaxFileChangeSnapshotBytes {
		beforeContent = nil
	}
	// Content missing for a file that existed is only valid when it
	// exceeded the snapshot cap; such entries are marked unrestorable.
	restorable := !beforeExisted || beforeContent != nil
	if restorable && beforeExisted && len(beforeContent) == 0 {
		// Normalize an empty (but captured) file to a non-nil empty blob:
		// a SQL NULL on read-back would be indistinguishable from
		// "content not captured", wrongly reporting the file unrestorable.
		beforeContent = []byte{}
	}
	_, err := s.db.ExecContext(ctx, `
INSERT INTO file_changes(session_id, path, before_existed, before_hash, before_content, after_hash, restorable, created_at)
VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		sessionID.String(), path, beforeExisted, beforeHash, beforeContent, afterHash, restorable,
		formatTime(time.Now().UTC()))
	if err != nil {
		return storeError("record file change", err)
	}
	return nil
}

// ledgerPositionTx returns the current change-ledger position (max rowid)
// for a session inside a transaction; checkpoints snapshot it so rewind
// knows exactly which changes postdate them.
func ledgerPositionTx(ctx context.Context, tx *sql.Tx, sessionID domain.SessionID) (int64, error) {
	var position int64
	if err := tx.QueryRowContext(ctx,
		"SELECT COALESCE(MAX(rowid), 0) FROM file_changes WHERE session_id = ?",
		sessionID.String()).Scan(&position); err != nil {
		return 0, storeError("read file change ledger position", err)
	}
	return position, nil
}

// ListCheckpoints returns the session's persisted checkpoints ordered by
// most recent first, each labelled with the last user message it covers.
func (s *SQLiteStore) ListCheckpoints(ctx context.Context, sessionID domain.SessionID, limit int) ([]CheckpointSummary, error) {
	if sessionID.IsZero() {
		return nil, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	if limit <= 0 || limit > 200 {
		return nil, domain.NewError(domain.ErrInvalidInput, "checkpoint list limit must be between 1 and 200")
	}
	rows, err := s.db.QueryContext(ctx, `
SELECT checkpoint_id, sequence, data, created_at
FROM checkpoints WHERE session_id = ?
ORDER BY sequence DESC, created_at_unix_nano DESC, checkpoint_id DESC LIMIT ?`,
		sessionID.String(), limit)
	if err != nil {
		return nil, storeError("list checkpoints", err)
	}
	defer rows.Close()

	out := make([]CheckpointSummary, 0)
	for rows.Next() {
		var id string
		var data []byte
		var createdAt string
		var summary CheckpointSummary
		if err := rows.Scan(&id, &summary.Sequence, &data, &createdAt); err != nil {
			return nil, storeError("scan checkpoint", err)
		}
		checkpointID, err := domain.ParseCheckpointID(id)
		if err != nil {
			return nil, storeError("decode checkpoint ID", err)
		}
		summary.ID = checkpointID
		summary.CreatedAt, err = time.Parse(time.RFC3339Nano, createdAt)
		if err != nil {
			return nil, storeError("decode checkpoint creation time", err)
		}
		summary.Label, summary.Turns = checkpointLabel(data)
		out = append(out, summary)
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate checkpoints", err)
	}
	return out, nil
}

// checkpointLabel derives a display label from a serialized checkpoint:
// the text of the last user message (truncated) and the user-turn count.
func checkpointLabel(data []byte) (string, int) {
	var checkpoint domain.Checkpoint
	if err := json.Unmarshal(data, &checkpoint); err != nil {
		return "", 0
	}
	turns := 0
	label := ""
	for _, msg := range checkpoint.Messages {
		if msg.Role != domain.RoleUser {
			continue
		}
		// Steer/budget system notes ride the user role too; only consider
		// messages with real text.
		text := strings.TrimSpace(strings.Join(msg.TextParts(), " "))
		if text == "" {
			continue
		}
		turns++
		label = text
	}
	runes := []rune(label)
	if len(runes) > 80 {
		label = string(runes[:80]) + "…"
	}
	return label, turns
}

// RewindSession truncates the session back to the checkpoint covering
// event sequence checkpointSequence: events and checkpoints after it are
// deleted, the session version is reset, and every file change recorded
// after that checkpoint is returned (earliest-per-path) for the caller to
// restore. Artifact references are recomputed from the surviving
// checkpoints (deleted ones must not keep pinning their artifacts), and
// memory extraction jobs that observed deleted events — or are mid-flight
// reading them — are reset to pending so the pipeline re-extracts from
// the rewound transcript. The whole mutation runs in one transaction;
// file restoration happens afterwards and is idempotent
// (content-addressed snapshots).
func (s *SQLiteStore) RewindSession(ctx context.Context, sessionID domain.SessionID, checkpointSequence int64) (RewindResult, error) {
	if sessionID.IsZero() {
		return RewindResult{}, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	if checkpointSequence <= 0 {
		return RewindResult{}, domain.NewError(domain.ErrInvalidInput, "checkpoint sequence must be positive")
	}

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return RewindResult{}, storeError("begin rewind transaction", err)
	}
	defer func() { _ = tx.Rollback() }()

	var summary domain.SessionSummary
	var version int64
	var id, createdAt, updatedAt string
	var archivedAt sql.NullInt64
	if err := tx.QueryRowContext(ctx,
		"SELECT session_id, version, created_at, updated_at, archived_at_unix_nano FROM sessions WHERE session_id = ?",
		sessionID.String()).Scan(&id, &version, &createdAt, &updatedAt, &archivedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return RewindResult{}, domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return RewindResult{}, storeError("load session for rewind", err)
	}
	// Archived sessions are read-only: rewinding rewrites history, so it
	// is rejected until the session is explicitly unarchived.
	if archivedAt.Valid {
		return RewindResult{}, domain.NewError(domain.ErrSessionArchived,
			"session is archived (read-only); unarchive it to rewind")
	}
	summary.ID = sessionID
	summary.Version = version

	// The target checkpoint must exist and must not be newer than the
	// current version (a future sequence would resurrect deleted history).
	var checkpointID, checkpointCreated string
	var checkpointData []byte
	err = tx.QueryRowContext(ctx, `
SELECT checkpoint_id, data, created_at FROM checkpoints
WHERE session_id = ? AND sequence = ?
ORDER BY created_at_unix_nano DESC, checkpoint_id DESC LIMIT 1`,
		sessionID.String(), checkpointSequence).Scan(&checkpointID, &checkpointData, &checkpointCreated)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return RewindResult{}, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("no checkpoint at sequence %d", checkpointSequence))
		}
		return RewindResult{}, storeError("load rewind checkpoint", err)
	}
	if checkpointSequence > version {
		return RewindResult{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("checkpoint sequence %d exceeds session version %d", checkpointSequence, version))
	}

	// The ledger position the checkpoint covers: prefer the snapshot taken
	// at checkpoint-write time; legacy checkpoints (column default 0)
	// rewind every recorded change, which is the safe direction.
	var ledgerSeq int64
	if err := tx.QueryRowContext(ctx, `
SELECT COALESCE(MAX(ledger_seq), 0) FROM checkpoints
WHERE session_id = ? AND sequence = ?`, sessionID.String(), checkpointSequence).Scan(&ledgerSeq); err != nil {
		return RewindResult{}, storeError("read checkpoint ledger position", err)
	}

	// Collect the changes to revert: everything recorded after the
	// checkpoint's ledger position, ordered ascending so the caller can
	// keep the earliest state per path.
	rows, err := tx.QueryContext(ctx, `
SELECT rowid, path, before_existed, before_hash, before_content, after_hash, restorable
FROM file_changes WHERE session_id = ? AND rowid > ? ORDER BY rowid ASC`,
		sessionID.String(), ledgerSeq)
	if err != nil {
		return RewindResult{}, storeError("load file changes for rewind", err)
	}
	var all []FileChange
	for rows.Next() {
		var change FileChange
		if err := rows.Scan(&change.ID, &change.Path, &change.BeforeExisted, &change.BeforeHash,
			&change.BeforeContent, &change.AfterHash, &change.Restorable); err != nil {
			_ = rows.Close()
			return RewindResult{}, storeError("scan file change", err)
		}
		all = append(all, change)
	}
	if err := rows.Err(); err != nil {
		_ = rows.Close()
		return RewindResult{}, storeError("iterate file changes", err)
	}
	_ = rows.Close()

	if _, err := tx.ExecContext(ctx,
		"DELETE FROM file_changes WHERE session_id = ? AND rowid > ?", sessionID.String(), ledgerSeq); err != nil {
		return RewindResult{}, storeError("delete rewound file changes", err)
	}
	if _, err := tx.ExecContext(ctx,
		"DELETE FROM events WHERE session_id = ? AND sequence > ?", sessionID.String(), checkpointSequence); err != nil {
		return RewindResult{}, storeError("delete rewound events", err)
	}
	if _, err := tx.ExecContext(ctx,
		"DELETE FROM checkpoints WHERE session_id = ? AND sequence > ?", sessionID.String(), checkpointSequence); err != nil {
		return RewindResult{}, storeError("delete rewound checkpoints", err)
	}
	now := time.Now().UTC()
	// Review M29: artifact_refs rows are registered per checkpoint and the
	// deleted checkpoints may be an artifact's only referrer — leaving
	// their rows behind pins the artifact forever (the GC never collects
	// it). Rebuild the session's references from the SURVIVING checkpoints
	// inside the same transaction.
	if err := recomputeArtifactRefs(ctx, tx, sessionID); err != nil {
		return RewindResult{}, err
	}
	// Review M29: a memory extraction whose extracted_version covers events
	// the rewind just deleted is stale (its memories may describe turns
	// that no longer exist), and an in-flight claim is reading the
	// pre-rewind transcript. Reset those jobs to pending with no extracted
	// version so the pipeline re-extracts from the rewound transcript.
	// Jobs that only observed retained events (extracted_version <= the
	// rewind point) stay untouched, as do fresh pending jobs.
	if _, err := tx.ExecContext(ctx, `
UPDATE memory_jobs SET
    status = 'pending',
    claim_token = '',
    claimed_at_unix_nano = 0,
    next_retry_at_unix_nano = 0,
    attempts = 0,
    extracted_version = -1,
    last_error = '',
    updated_at = ?, updated_at_unix_nano = ?
WHERE session_id = ? AND (extracted_version > ? OR status = 'claimed')`,
		formatTime(now), now.UnixNano(), sessionID.String(), checkpointSequence); err != nil {
		return RewindResult{}, storeError("reset stale memory jobs", err)
	}
	result, err := tx.ExecContext(ctx, `
UPDATE sessions SET version = ?, updated_at = ?, updated_at_unix_nano = ?
WHERE session_id = ? AND version = ?`,
		checkpointSequence, formatTime(now), now.UnixNano(), sessionID.String(), version)
	if err != nil {
		return RewindResult{}, storeError("reset session version", err)
	}
	if affected, err := result.RowsAffected(); err != nil {
		return RewindResult{}, storeError("inspect session version reset", err)
	} else if affected != 1 {
		return RewindResult{}, domain.NewError(domain.ErrConflict, "session version changed while rewinding")
	}
	if err := tx.Commit(); err != nil {
		return RewindResult{}, storeError("commit rewind transaction", err)
	}

	// Deduplicate by path, keeping the EARLIEST post-checkpoint change per
	// path — its before-state is the content the checkpoint observed —
	// annotated with the LATEST after-hash so the restore can detect
	// modifications that happened outside the recorded history.
	latestAfter := make(map[string]string, len(all))
	for _, change := range all {
		latestAfter[change.Path] = change.AfterHash
	}
	seen := make(map[string]bool, len(all))
	changes := make([]FileChange, 0, len(all))
	for _, change := range all {
		if seen[change.Path] {
			continue
		}
		seen[change.Path] = true
		change.LatestAfterHash = latestAfter[change.Path]
		changes = append(changes, change)
	}

	parsedID, err := domain.ParseCheckpointID(checkpointID)
	if err != nil {
		return RewindResult{}, storeError("decode rewind checkpoint ID", err)
	}
	created, err := time.Parse(time.RFC3339Nano, checkpointCreated)
	if err != nil {
		return RewindResult{}, storeError("decode rewind checkpoint time", err)
	}
	label, turns := checkpointLabel([]byte(checkpointData))
	return RewindResult{
		Checkpoint: CheckpointSummary{
			ID: parsedID, Sequence: checkpointSequence, CreatedAt: created,
			Label: label, Turns: turns,
		},
		Changes: changes,
	}, nil
}

// recomputeArtifactRefs rebuilds a session's artifact_refs rows from its
// surviving checkpoints. Rewind calls it after truncating the log:
// references registered by deleted checkpoints must not outlive them, or
// the artifacts they pin leak past the GC forever (review M29).
func recomputeArtifactRefs(ctx context.Context, tx *sql.Tx, sessionID domain.SessionID) error {
	if _, err := tx.ExecContext(ctx,
		"DELETE FROM artifact_refs WHERE session_id = ?", sessionID.String()); err != nil {
		return storeError("reset rewound artifact references", err)
	}
	rows, err := tx.QueryContext(ctx,
		"SELECT data FROM checkpoints WHERE session_id = ?", sessionID.String())
	if err != nil {
		return storeError("load surviving checkpoints for artifact references", err)
	}
	refs := make(map[domain.ArtifactID]int64)
	for rows.Next() {
		var data []byte
		if err := rows.Scan(&data); err != nil {
			_ = rows.Close()
			return storeError("scan checkpoint for artifact references", err)
		}
		var checkpoint domain.Checkpoint
		if err := json.Unmarshal(data, &checkpoint); err != nil {
			_ = rows.Close()
			return storeError("decode checkpoint for artifact references", err)
		}
		for id, size := range checkpointArtifactRefs(checkpoint) {
			if old, ok := refs[id]; !ok || size > old {
				refs[id] = size
			}
		}
	}
	if err := rows.Err(); err != nil {
		_ = rows.Close()
		return storeError("iterate checkpoints for artifact references", err)
	}
	if err := rows.Close(); err != nil {
		return storeError("close checkpoints for artifact references", err)
	}
	// Surviving directive events pin their artifacts too: the reference
	// graph must stay complete even if the checkpoints that carried the
	// same references were truncated (docs/SURFACE_DESIGN.md §4.6).
	directiveRows, err := tx.QueryContext(ctx,
		`SELECT type, payload FROM events WHERE session_id = ? AND type IN ('context.masked', 'context.archived')`,
		sessionID.String())
	if err != nil {
		return storeError("load surviving directive events for artifact references", err)
	}
	for directiveRows.Next() {
		var evtType string
		var payload []byte
		if err := directiveRows.Scan(&evtType, &payload); err != nil {
			_ = directiveRows.Close()
			return storeError("scan directive event for artifact references", err)
		}
		directiveRefs, err := surfaceDirectiveArtifactRefs(domain.Event{Type: domain.EventType(evtType), Payload: payload})
		if err != nil {
			_ = directiveRows.Close()
			return storeError("scan directive artifact references", err)
		}
		for _, ref := range directiveRefs {
			addArtifactRef(refs, ref)
		}
	}
	if err := directiveRows.Err(); err != nil {
		_ = directiveRows.Close()
		return storeError("iterate directive events for artifact references", err)
	}
	if err := directiveRows.Close(); err != nil {
		return storeError("close directive events for artifact references", err)
	}
	return addArtifactRefs(ctx, tx, sessionID, refs)
}
