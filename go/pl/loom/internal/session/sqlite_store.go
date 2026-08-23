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
// Created: 2026/07/23

package session

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	_ "modernc.org/sqlite"
)

const sqliteSchemaVersion = 9

// SQLiteStore persists session events and checkpoints in a SQLite database.
// A store serializes writes through one connection; optimistic versions still
// protect sessions from stale writers and make conflicts explicit.
type SQLiteStore struct {
	db *sql.DB
}

// SessionInspection is a consistent read-only view of persisted session data.
// Deprecated: use domain.SessionInspection instead.
type SessionInspection = domain.SessionInspection

// OpenSQLiteStore opens or creates a SQLite event store and pins the schema
// version. There are no in-code migrations: loom is still in development and
// the only databases that exist are dev-local, so a stale database fails
// loudly here — moving it forward is a one-off script, not store code.
func OpenSQLiteStore(ctx context.Context, path string) (*SQLiteStore, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "sqlite database path is required")
	}

	// The pragmas ride the DSN so EVERY pooled connection gets them —
	// database/sql opens conns on demand, and an Exec-applied pragma
	// would silently miss later connections (foreign_keys, busy_timeout).
	//
	// _txlock=immediate makes every write transaction BEGIN IMMEDIATE:
	// the write lock is acquired at BEGIN, so busy_timeout genuinely
	// applies and contending writers queue instead of failing. With the
	// default deferred mode a transaction snapshots at its first SELECT
	// and upgrades to a write lock at the first INSERT — a concurrent
	// commit in between makes SQLite return SQLITE_BUSY_SNAPSHOT at once,
	// bypassing busy_timeout entirely (observed in production: a turn's
	// terminal event lost to "database is locked (5)" while a sub-agent
	// session committed within the same millisecond). Read-only
	// transactions (InspectSession) stay deferred — the driver only
	// applies the txlock to read-write transactions.
	dsn := "file:" + filepath.ToSlash(path) +
		"?_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)" +
		"&_pragma=journal_mode(WAL)&_pragma=synchronous(FULL)" +
		"&_txlock=immediate"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, storeError("open sqlite database", err)
	}
	// WAL allows one writer plus concurrent readers: two connections let
	// a long write transaction (event batch + checkpoint) proceed without
	// starving reads, while contending writers are serialized by
	// SQLite itself and spared instant SQLITE_BUSY by busy_timeout.
	db.SetMaxOpenConns(2)
	db.SetMaxIdleConns(2)
	store := &SQLiteStore{db: db}
	if err := store.initialize(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	return store, nil
}

// OpenSQLiteStoreReadOnly opens an existing store without running migrations or writes.
func OpenSQLiteStoreReadOnly(ctx context.Context, path string) (*SQLiteStore, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "sqlite database path is required")
	}
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(path)+"?mode=ro")
	if err != nil {
		return nil, storeError("open sqlite database read-only", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	store := &SQLiteStore{db: db}
	if _, err := db.ExecContext(ctx, "PRAGMA busy_timeout = 5000"); err != nil {
		_ = db.Close()
		return nil, storeError("configure sqlite database read-only", err)
	}
	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		return nil, storeError("ping sqlite database read-only", err)
	}
	var newestVersion sql.NullInt64
	if err := db.QueryRowContext(ctx, "SELECT MAX(version) FROM schema_migrations").Scan(&newestVersion); err != nil {
		_ = db.Close()
		return nil, storeError("read sqlite schema version", err)
	}
	if !newestVersion.Valid || newestVersion.Int64 != sqliteSchemaVersion {
		_ = db.Close()
		return nil, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("sqlite schema version is incompatible; supported version is %d", sqliteSchemaVersion))
	}
	return store, nil
}

// Close releases the database connection.
func (s *SQLiteStore) Close() error {
	if s == nil || s.db == nil {
		return nil
	}
	return s.db.Close()
}

func (s *SQLiteStore) initialize(ctx context.Context) error {
	// Connection pragmas are bound in the DSN (see OpenSQLiteStore).
	if err := s.db.PingContext(ctx); err != nil {
		return storeError("ping sqlite database", err)
	}

	const schema = `
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    session_id TEXT PRIMARY KEY,
    version INTEGER NOT NULL CHECK (version >= 0),
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    updated_at_unix_nano INTEGER NOT NULL,
    archived_at_unix_nano INTEGER,
    workspace_id TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_sessions_workspace_updated
    ON sessions(workspace_id, updated_at_unix_nano DESC);
CREATE TABLE IF NOT EXISTS events (
event_id TEXT PRIMARY KEY,
session_id TEXT NOT NULL,
sequence INTEGER NOT NULL CHECK (sequence > 0),
type TEXT NOT NULL,
timestamp TEXT NOT NULL,
payload BLOB,
ignorable INTEGER NOT NULL DEFAULT 0,
UNIQUE (session_id, sequence),
FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_events_session_sequence
    ON events(session_id, sequence);
CREATE TABLE IF NOT EXISTS checkpoints (
    checkpoint_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    sequence INTEGER NOT NULL CHECK (sequence >= 0),
    ledger_seq INTEGER NOT NULL DEFAULT 0,
    data BLOB NOT NULL,
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_checkpoints_session_sequence
    ON checkpoints(session_id, sequence DESC, created_at_unix_nano DESC);
CREATE TABLE IF NOT EXISTS artifact_refs (
    session_id TEXT NOT NULL,
    artifact_id TEXT NOT NULL,
    size INTEGER NOT NULL CHECK (size >= 0),
    PRIMARY KEY (session_id, artifact_id),
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_artifact_refs_artifact
    ON artifact_refs(artifact_id);
CREATE TABLE IF NOT EXISTS file_changes (
    rowid INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    path TEXT NOT NULL,
    before_existed INTEGER NOT NULL,
    before_hash TEXT NOT NULL DEFAULT '',
    before_content BLOB,
    after_hash TEXT NOT NULL DEFAULT '',
    restorable INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_file_changes_session_rowid
    ON file_changes(session_id, rowid);
CREATE TABLE IF NOT EXISTS workspaces (
    workspace_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    root_path TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    updated_at_unix_nano INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS memory_jobs (
    session_id TEXT PRIMARY KEY,
    workspace_root TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'pending',
    attempts INTEGER NOT NULL DEFAULT 0,
    claim_token TEXT NOT NULL DEFAULT '',
    claimed_at_unix_nano INTEGER NOT NULL DEFAULT 0,
    next_retry_at_unix_nano INTEGER NOT NULL DEFAULT 0,
    extracted_version INTEGER NOT NULL DEFAULT -1,
    last_error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    updated_at_unix_nano INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_memory_jobs_claimable
    ON memory_jobs(status, next_retry_at_unix_nano, updated_at_unix_nano);
CREATE TABLE IF NOT EXISTS memory_phase2_lease (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    claim_token TEXT NOT NULL DEFAULT '',
    claimed_at_unix_nano INTEGER NOT NULL DEFAULT 0
);
-- app_prefs: process-level key-value preferences (schema v7). Current
-- keys: "model" ("provider/model") and "reasoning" (effort or "default"),
-- written when the user switches either one so later sessions — including
-- after a restart — inherit the manual choice.
CREATE TABLE IF NOT EXISTS app_prefs (
    pref_key TEXT PRIMARY KEY,
    pref_value TEXT NOT NULL DEFAULT ''
);
-- session_shares: one active share link per session (schema v8). The
-- 128-bit random token is the capability — anyone holding the link can
-- read the session transcript via the public /share/{token} routes until
-- the share is revoked or the session is deleted (cascade).
CREATE TABLE IF NOT EXISTS session_shares (
    session_id TEXT PRIMARY KEY,
    share_token TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return storeError("apply sqlite schema", err)
	}
	var newestVersion sql.NullInt64
	if err := s.db.QueryRowContext(ctx, "SELECT MAX(version) FROM schema_migrations").Scan(&newestVersion); err != nil {
		return storeError("read sqlite schema version", err)
	}
	if newestVersion.Valid && newestVersion.Int64 != sqliteSchemaVersion {
		return domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("sqlite schema version %d is incompatible with supported version %d (recreate the dev database or migrate it with a one-off script)", newestVersion.Int64, sqliteSchemaVersion))
	}
	_, err := s.db.ExecContext(ctx,
		"INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (?, ?)",
		sqliteSchemaVersion, formatTime(time.Now().UTC()))
	if err != nil {
		return storeError("record sqlite schema migration", err)
	}
	return nil
}

// CreateSession creates an empty session with version zero, bound to the
// given workspace (docs/WORKSPACE_DESIGN.md W1).
func (s *SQLiteStore) CreateSession(ctx context.Context, sessionID domain.SessionID, workspaceID domain.WorkspaceID) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	now := time.Now().UTC()
	_, err := s.db.ExecContext(ctx, `
INSERT INTO sessions(session_id, version, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano, workspace_id)
VALUES (?, 0, ?, ?, ?, ?, ?)`, sessionID.String(), formatTime(now), now.UnixNano(), formatTime(now), now.UnixNano(), workspaceID.String())
	if err != nil {
		if isUniqueConstraint(err) {
			return domain.NewError(domain.ErrConflict, "session already exists", domain.WithCause(err))
		}
		return storeError("create session", err)
	}
	return nil
}

// validateContiguousEvents checks that events form a contiguous batch
// starting at expectedVersion+1 for the given session.
func validateContiguousEvents(sessionID domain.SessionID, expectedVersion int64, events []domain.Event) error {
	for i, event := range events {
		if err := event.Validate(); err != nil {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("invalid event at index %d", i), domain.WithCause(err))
		}
		if event.SessionID != sessionID {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("event at index %d has a different session ID", i))
		}
		want := expectedVersion + int64(i) + 1
		if event.Sequence != want {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("event sequence %d at index %d, want %d", event.Sequence, i, want))
		}
	}
	return nil
}

// AppendEvents atomically appends a contiguous event batch at expectedVersion.
func (s *SQLiteStore) AppendEvents(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	if expectedVersion < 0 {
		return domain.NewError(domain.ErrInvalidInput, "expected version must be non-negative")
	}
	if err := validateContiguousEvents(sessionID, expectedVersion, events); err != nil {
		return err
	}
	return appendWithRetry(ctx, func() error {
		return s.appendEventsTx(ctx, sessionID, expectedVersion, events, nil)
	})
}

// appendBusyRetries bounds how often an append batch is re-executed after
// a retryable store failure (SQLITE_BUSY once every writer queue wait
// exhausts busy_timeout — possible under fsync stalls even with BEGIN
// IMMEDIATE). Only retryable errors are re-executed: conflicts (version
// mismatch, duplicate events) and validation failures can never succeed on
// retry and return immediately. The retried operation is a fresh
// transaction over the same batch, so a retry either applies the whole
// batch once or not at all — a commit that secretly succeeded before
// failing would surface as ErrConflict on retry rather than duplicate
// data.
const appendBusyRetries = 3

func appendWithRetry(ctx context.Context, op func() error) error {
	var err error
	for attempt := 0; ; attempt++ {
		if err = op(); err == nil || !domain.IsRetryable(err) || attempt >= appendBusyRetries {
			return err
		}
		delay := time.Duration(25*(attempt+1)*(attempt+1)) * time.Millisecond
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return err
		case <-timer.C:
		}
	}
}

// appendEventsTx runs the shared append skeleton in one transaction:
// optimistic version check, event inserts, session version advance. The
// optional extra runs inside the same transaction (e.g. checkpoint
// persistence) so the whole batch commits or rolls back together.
func (s *SQLiteStore) appendEventsTx(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event, extra func(ctx context.Context, tx *sql.Tx) error) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin append transaction", err)
	}
	defer func() { _ = tx.Rollback() }()

	var actualVersion int64
	var archivedAt sql.NullInt64
	if err := tx.QueryRowContext(ctx,
		"SELECT version, archived_at_unix_nano FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&actualVersion, &archivedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return storeError("load session version", err)
	}
	// Archived sessions are read-only: appending is rejected instead of
	// silently resurrecting the session. The check rides the write
	// transaction (BEGIN IMMEDIATE), so a concurrent archive cannot slip
	// in between this read and the version advance below.
	if archivedAt.Valid {
		return domain.NewError(domain.ErrSessionArchived,
			"session is archived (read-only); unarchive it to continue")
	}
	if actualVersion != expectedVersion {
		return domain.NewError(domain.ErrConflict,
			fmt.Sprintf("session version mismatch: expected %d, got %d", expectedVersion, actualVersion))
	}

	for i := range events {
		event := events[i]
		ignorable := 0
		if event.Ignorable {
			ignorable = 1
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO events(event_id, session_id, sequence, type, timestamp, payload, ignorable)
VALUES (?, ?, ?, ?, ?, ?, ?)`, event.ID.String(), sessionID.String(), event.Sequence,
			string(event.Type), formatTime(event.Timestamp), []byte(event.Payload), ignorable); err != nil {
			if isUniqueConstraint(err) {
				return domain.NewError(domain.ErrConflict, "event already exists", domain.WithCause(err))
			}
			return storeError("insert event", err)
		}
	}

	newVersion := expectedVersion + int64(len(events))
	now := time.Now().UTC()
	result, err := tx.ExecContext(ctx, `
UPDATE sessions SET version = ?, updated_at = ?, updated_at_unix_nano = ?
WHERE session_id = ? AND version = ?`, newVersion, formatTime(now), now.UnixNano(),
		sessionID.String(), expectedVersion)
	if err != nil {
		return storeError("advance session version", err)
	}
	if affected, err := result.RowsAffected(); err != nil {
		return storeError("inspect session version update", err)
	} else if affected != 1 {
		return domain.NewError(domain.ErrConflict, "session version changed while appending events")
	}
	if extra != nil {
		if err := extra(ctx, tx); err != nil {
			return err
		}
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit append transaction", err)
	}
	return nil
}

// AppendEventsAndCheckpoint atomically appends events and saves the projection
// checkpoint covering exactly the resulting session version.
func (s *SQLiteStore) AppendEventsAndCheckpoint(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	if sessionID.IsZero() || expectedVersion < 0 {
		return domain.NewError(domain.ErrInvalidInput, "valid session ID and expected version are required")
	}
	if checkpoint.ID.IsZero() || checkpoint.SessionID != sessionID {
		return domain.NewError(domain.ErrInvalidInput, "checkpoint identity does not match session")
	}
	newVersion := expectedVersion + int64(len(events))
	if checkpoint.Sequence != newVersion {
		return domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("checkpoint sequence %d does not match resulting version %d", checkpoint.Sequence, newVersion))
	}
	if err := validateContiguousEvents(sessionID, expectedVersion, events); err != nil {
		return err
	}
	data, err := json.Marshal(checkpoint)
	if err != nil {
		return domain.NewError(domain.ErrInvalidInput, "encode checkpoint", domain.WithCause(err))
	}
	return appendWithRetry(ctx, func() error {
		return s.appendEventsTx(ctx, sessionID, expectedVersion, events, func(ctx context.Context, tx *sql.Tx) error {
			// Snapshot the current file-change ledger position so that rewind
			// knows exactly which changes postdate this checkpoint.
			ledgerSeq, err := ledgerPositionTx(ctx, tx, sessionID)
			if err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `
INSERT INTO checkpoints(checkpoint_id, session_id, sequence, ledger_seq, data, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?, ?)`, checkpoint.ID.String(), sessionID.String(), checkpoint.Sequence,
				ledgerSeq, data, formatTime(checkpoint.CreatedAt), checkpoint.CreatedAt.UTC().UnixNano()); err != nil {
				if isUniqueConstraint(err) {
					return domain.NewError(domain.ErrConflict, "checkpoint already exists", domain.WithCause(err))
				}
				return storeError("save checkpoint", err)
			}
			return addArtifactRefs(ctx, tx, sessionID, checkpointArtifactRefs(checkpoint))
		})
	})
}

// LoadEvents loads events after the supplied sequence in ascending order.
func (s *SQLiteStore) LoadEvents(ctx context.Context, sessionID domain.SessionID, after int64) ([]domain.Event, error) {
	if sessionID.IsZero() {
		return nil, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	if after < 0 {
		return nil, domain.NewError(domain.ErrInvalidInput, "after sequence must be non-negative")
	}
	var exists int
	if err := s.db.QueryRowContext(ctx,
		"SELECT 1 FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&exists); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return nil, storeError("find session", err)
	}

	rows, err := s.db.QueryContext(ctx, `
SELECT event_id, sequence, type, timestamp, payload, ignorable
FROM events WHERE session_id = ? AND sequence > ? ORDER BY sequence ASC`, sessionID.String(), after)
	if err != nil {
		return nil, storeError("load events", err)
	}
	defer rows.Close()

	result := make([]domain.Event, 0)
	for rows.Next() {
		var id, eventType, timestamp string
		var sequence int64
		var payload []byte
		var ignorable int
		if err := rows.Scan(&id, &sequence, &eventType, &timestamp, &payload, &ignorable); err != nil {
			return nil, storeError("scan event", err)
		}
		eventID, err := domain.ParseEventID(id)
		if err != nil {
			return nil, storeError("decode event ID", err)
		}
		parsedTime, err := time.Parse(time.RFC3339Nano, timestamp)
		if err != nil {
			return nil, storeError("decode event timestamp", err)
		}
		event := domain.Event{
			ID: eventID, Sequence: sequence, SessionID: sessionID,
			Type: domain.EventType(eventType), Timestamp: parsedTime,
			Payload:   append(json.RawMessage(nil), payload...),
			Ignorable: ignorable != 0,
		}
		if err := event.Validate(); err != nil {
			return nil, storeError("validate persisted event", err)
		}
		result = append(result, event)
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate events", err)
	}
	return result, nil
}

// ListSessions returns one page of persisted sessions ordered by most
// recent creation, each row carrying its delegation parent when the session
// is a sub-agent child (read off the child's run.created event payload,
// no schema migration). archived selects the archived view instead of the
// default active listing. cursor is the previous page's nextCursor (""
// selects the first page); the returned nextCursor is "" once the last
// page has been served.
// ListSessions returns a page of session summaries in reverse-creation order.
// workspaceID, when non-zero, restricts the listing to that workspace
// (docs/WORKSPACE_DESIGN.md §8.1); zero lists across all workspaces.
func (s *SQLiteStore) ListSessions(ctx context.Context, cursor string, limit int, archived bool, workspaceID domain.WorkspaceID) ([]domain.SessionSummary, string, error) {
	if limit <= 0 || limit > 1000 {
		return nil, "", domain.NewError(domain.ErrInvalidInput, "session list limit must be between 1 and 1000")
	}
	// Keyset pagination over (created_at_unix_nano DESC, session_id DESC);
	// cursorNano = -1 marks the first page.
	cursorNano := int64(-1)
	cursorID := ""
	if cursor != "" {
		nano, id, ok := strings.Cut(cursor, ":")
		n, err := strconv.ParseInt(nano, 10, 64)
		if !ok || err != nil || id == "" {
			return nil, "", domain.NewError(domain.ErrInvalidInput, "invalid session list cursor")
		}
		cursorNano, cursorID = n, id
	}
	archivedFlag := 0
	if archived {
		archivedFlag = 1
	}
	rows, err := s.db.QueryContext(ctx, `
SELECT s.session_id, s.version, s.created_at, s.updated_at, s.created_at_unix_nano, s.workspace_id,
       (SELECT json_extract(CAST(e.payload AS TEXT), '$.parent_session_id')
        FROM events e
        WHERE e.session_id = s.session_id AND e.type = 'run.created'
        LIMIT 1) AS parent_session_id
FROM sessions s
WHERE ((s.archived_at_unix_nano IS NOT NULL) = ?4)
  AND (?5 = '' OR s.workspace_id = ?5)
  AND ((?1 < 0)
   OR (s.created_at_unix_nano < ?1)
   OR (s.created_at_unix_nano = ?1 AND s.session_id < ?2))
ORDER BY s.created_at_unix_nano DESC, s.session_id DESC
LIMIT ?3`, cursorNano, cursorID, limit, archivedFlag, workspaceID.String())
	if err != nil {
		return nil, "", storeError("list sessions", err)
	}
	defer rows.Close()

	result := make([]domain.SessionSummary, 0)
	var lastNano int64
	var lastID string
	for rows.Next() {
		var id, createdAt, updatedAt, rawWorkspace string
		var version int64
		var parent sql.NullString
		if err := rows.Scan(&id, &version, &createdAt, &updatedAt, &lastNano, &rawWorkspace, &parent); err != nil {
			return nil, "", storeError("scan session", err)
		}
		lastID = id
		sessionID, err := domain.ParseSessionID(id)
		if err != nil {
			return nil, "", storeError("decode session ID", err)
		}
		created, err := time.Parse(time.RFC3339Nano, createdAt)
		if err != nil {
			return nil, "", storeError("decode session creation time", err)
		}
		updated, err := time.Parse(time.RFC3339Nano, updatedAt)
		if err != nil {
			return nil, "", storeError("decode session update time", err)
		}
		summary := domain.SessionSummary{
			ID: sessionID, Version: version, CreatedAt: created, UpdatedAt: updated,
		}
		if wsID, err := domain.ParseWorkspaceID(rawWorkspace); err == nil {
			summary.WorkspaceID = wsID
		}
		if parent.Valid && parent.String != "" {
			parentID, err := domain.ParseSessionID(parent.String)
			if err != nil {
				return nil, "", storeError("decode parent session ID", err)
			}
			summary.ParentSessionID = parentID
		}
		result = append(result, summary)
	}
	if err := rows.Err(); err != nil {
		return nil, "", storeError("iterate sessions", err)
	}
	nextCursor := ""
	if len(result) == limit {
		nextCursor = strconv.FormatInt(lastNano, 10) + ":" + lastID
	}
	return result, nextCursor, nil
}

// FirstUserMessageTexts returns the text of each session's first user
// message (the user.message_added event with the lowest sequence), keyed
// by session ID. Sessions without a user message are absent from the map.
// One indexed lookup per session — cheap enough for list-endpoint
// enrichment (docs/WEB_DESIGN.md §7.7).
func (s *SQLiteStore) FirstUserMessageTexts(ctx context.Context, ids []domain.SessionID) (map[domain.SessionID]string, error) {
	result := make(map[domain.SessionID]string, len(ids))
	for _, id := range ids {
		var payload []byte
		err := s.db.QueryRowContext(ctx, `
SELECT payload FROM events WHERE session_id = ? AND type = ? ORDER BY sequence LIMIT 1`,
			id.String(), string(domain.EventUserMessageAdded)).Scan(&payload)
		if errors.Is(err, sql.ErrNoRows) {
			continue
		}
		if err != nil {
			return nil, storeError("query first user message", err)
		}
		var env domain.MessageEventPayload
		if err := json.Unmarshal(payload, &env); err != nil {
			continue // malformed payloads must not break listing
		}
		parts := env.Message.TextParts()
		if len(parts) == 0 || parts[0] == "" {
			continue
		}
		result[id] = parts[0]
	}
	return result, nil
}

// InspectSession returns session metadata, its latest checkpoint, the recovered
// transcript, and the complete event timeline from one consistent read snapshot.
func (s *SQLiteStore) InspectSession(ctx context.Context, sessionID domain.SessionID) (domain.SessionInspection, error) {
	if sessionID.IsZero() {
		return domain.SessionInspection{}, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return domain.SessionInspection{}, storeError("begin session inspection", err)
	}
	defer func() { _ = tx.Rollback() }()

	var summary domain.SessionSummary
	var id, createdAt, updatedAt string
	if err := tx.QueryRowContext(ctx, `
SELECT session_id, version, created_at, updated_at
FROM sessions WHERE session_id = ?`, sessionID.String()).Scan(
		&id, &summary.Version, &createdAt, &updatedAt,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.SessionInspection{}, domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return domain.SessionInspection{}, storeError("load session metadata", err)
	}
	summary.ID, err = domain.ParseSessionID(id)
	if err != nil {
		return domain.SessionInspection{}, storeError("decode session ID", err)
	}
	summary.CreatedAt, err = time.Parse(time.RFC3339Nano, createdAt)
	if err != nil {
		return domain.SessionInspection{}, storeError("decode session creation time", err)
	}
	summary.UpdatedAt, err = time.Parse(time.RFC3339Nano, updatedAt)
	if err != nil {
		return domain.SessionInspection{}, storeError("decode session update time", err)
	}

	var checkpoint *domain.Checkpoint
	var checkpointData []byte
	err = tx.QueryRowContext(ctx, `
SELECT data FROM checkpoints WHERE session_id = ?
ORDER BY sequence DESC, created_at_unix_nano DESC, checkpoint_id DESC LIMIT 1`, sessionID.String()).Scan(&checkpointData)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return domain.SessionInspection{}, storeError("load latest checkpoint", err)
	}
	if err == nil {
		var decoded domain.Checkpoint
		if err := json.Unmarshal(checkpointData, &decoded); err != nil {
			return domain.SessionInspection{}, storeError("decode checkpoint", err)
		}
		if decoded.ID.IsZero() || decoded.SessionID != sessionID || decoded.Sequence > summary.Version {
			return domain.SessionInspection{}, storeError("validate persisted checkpoint", errors.New("checkpoint identity or sequence mismatch"))
		}
		checkpoint = &decoded
	}

	rows, err := tx.QueryContext(ctx, `
SELECT event_id, sequence, type, timestamp, payload, ignorable
FROM events WHERE session_id = ? ORDER BY sequence ASC`, sessionID.String())
	if err != nil {
		return domain.SessionInspection{}, storeError("load session events", err)
	}
	events, err := scanEvents(rows, sessionID)
	if err != nil {
		return domain.SessionInspection{}, err
	}
	if int64(len(events)) != summary.Version {
		return domain.SessionInspection{}, storeError("validate session event log",
			fmt.Errorf("session version is %d but event count is %d", summary.Version, len(events)))
	}

	var transcript Transcript
	if checkpoint != nil {
		later := events[checkpoint.Sequence:]
		transcript, err = ReplayFromCheckpoint(*checkpoint, later)
	} else if len(events) > 0 {
		transcript, err = Replay(events)
	} else {
		transcript = Transcript{SessionID: sessionID}
	}
	if err != nil {
		return domain.SessionInspection{}, storeError("recover session transcript", err)
	}
	if transcript.LastEventSequence != summary.Version {
		return domain.SessionInspection{}, storeError("validate recovered transcript",
			fmt.Errorf("recovered sequence is %d but session version is %d", transcript.LastEventSequence, summary.Version))
	}
	if err := tx.Commit(); err != nil {
		return domain.SessionInspection{}, storeError("commit session inspection", err)
	}
	return domain.SessionInspection{
		Session:    summary,
		Checkpoint: checkpoint,
		Transcript: domain.SessionTranscript{
			SessionID:         transcript.SessionID,
			Messages:          transcript.Messages,
			LastEventSequence: transcript.LastEventSequence,
		},
		Events: events,
	}, nil
}

func scanEvents(rows *sql.Rows, sessionID domain.SessionID) ([]domain.Event, error) {
	defer rows.Close()
	result := make([]domain.Event, 0)
	for rows.Next() {
		var id, eventType, timestamp string
		var sequence int64
		var payload []byte
		var ignorable int
		if err := rows.Scan(&id, &sequence, &eventType, &timestamp, &payload, &ignorable); err != nil {
			return nil, storeError("scan event", err)
		}
		eventID, err := domain.ParseEventID(id)
		if err != nil {
			return nil, storeError("decode event ID", err)
		}
		parsedTime, err := time.Parse(time.RFC3339Nano, timestamp)
		if err != nil {
			return nil, storeError("decode event timestamp", err)
		}
		event := domain.Event{
			ID: eventID, Sequence: sequence, SessionID: sessionID,
			Type: domain.EventType(eventType), Timestamp: parsedTime,
			Payload:   append(json.RawMessage(nil), payload...),
			Ignorable: ignorable != 0,
		}
		if err := event.Validate(); err != nil {
			return nil, storeError("validate persisted event", err)
		}
		if event.Sequence != int64(len(result))+1 {
			return nil, storeError("validate session event log",
				fmt.Errorf("event sequence is %d, want %d", event.Sequence, len(result)+1))
		}
		result = append(result, event)
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate events", err)
	}
	return result, nil
}

// SaveCheckpoint persists a checkpoint snapshot for a session.
func (s *SQLiteStore) SaveCheckpoint(ctx context.Context, checkpoint domain.Checkpoint) error {
	if checkpoint.ID.IsZero() || checkpoint.SessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "checkpoint and session IDs are required")
	}
	if checkpoint.Sequence < 0 {
		return domain.NewError(domain.ErrInvalidInput, "checkpoint sequence must be non-negative")
	}
	data, err := json.Marshal(checkpoint)
	if err != nil {
		return domain.NewError(domain.ErrInvalidInput, "encode checkpoint", domain.WithCause(err))
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin checkpoint transaction", err)
	}
	defer func() { _ = tx.Rollback() }()
	var sessionVersion int64
	if err := tx.QueryRowContext(ctx,
		"SELECT version FROM sessions WHERE session_id = ?", checkpoint.SessionID.String()).Scan(&sessionVersion); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return storeError("load checkpoint session version", err)
	}
	if checkpoint.Sequence > sessionVersion {
		return domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("checkpoint sequence %d exceeds session version %d", checkpoint.Sequence, sessionVersion))
	}
	// Snapshot the current file-change ledger position for rewind.
	ledgerSeq, err := ledgerPositionTx(ctx, tx, checkpoint.SessionID)
	if err != nil {
		return err
	}
	_, err = tx.ExecContext(ctx, `
INSERT INTO checkpoints(checkpoint_id, session_id, sequence, ledger_seq, data, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?, ?)`, checkpoint.ID.String(), checkpoint.SessionID.String(), checkpoint.Sequence,
		ledgerSeq, data, formatTime(checkpoint.CreatedAt), checkpoint.CreatedAt.UTC().UnixNano())
	if err != nil {
		if isUniqueConstraint(err) {
			return domain.NewError(domain.ErrConflict, "checkpoint already exists", domain.WithCause(err))
		}
		return storeError("save checkpoint", err)
	}
	if err := addArtifactRefs(ctx, tx, checkpoint.SessionID, checkpointArtifactRefs(checkpoint)); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit checkpoint transaction", err)
	}
	return nil
}

// LoadLatestCheckpoint loads the checkpoint with the greatest covered sequence.
func (s *SQLiteStore) LoadLatestCheckpoint(ctx context.Context, sessionID domain.SessionID) (domain.Checkpoint, error) {
	if sessionID.IsZero() {
		return domain.Checkpoint{}, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	var data []byte
	err := s.db.QueryRowContext(ctx, `
SELECT data FROM checkpoints WHERE session_id = ?
ORDER BY sequence DESC, created_at_unix_nano DESC, checkpoint_id DESC LIMIT 1`, sessionID.String()).Scan(&data)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.Checkpoint{}, domain.NewError(domain.ErrInvalidInput, "no checkpoint found")
		}
		return domain.Checkpoint{}, storeError("load latest checkpoint", err)
	}
	var checkpoint domain.Checkpoint
	if err := json.Unmarshal(data, &checkpoint); err != nil {
		return domain.Checkpoint{}, storeError("decode checkpoint", err)
	}
	if checkpoint.ID.IsZero() || checkpoint.SessionID != sessionID {
		return domain.Checkpoint{}, storeError("validate persisted checkpoint", errors.New("checkpoint identity mismatch"))
	}
	return checkpoint, nil
}

// ListArtifactRefs returns all artifacts referenced by durable session projections.
func (s *SQLiteStore) ListArtifactRefs(ctx context.Context) (map[domain.ArtifactID]int64, error) {
	rows, err := s.db.QueryContext(ctx, `
SELECT artifact_id, MAX(size) FROM artifact_refs GROUP BY artifact_id ORDER BY artifact_id`)
	if err != nil {
		return nil, storeError("list artifact references", err)
	}
	defer rows.Close()
	refs := make(map[domain.ArtifactID]int64)
	for rows.Next() {
		var rawID string
		var size int64
		if err := rows.Scan(&rawID, &size); err != nil {
			return nil, storeError("scan artifact reference", err)
		}
		id, err := domain.ParseArtifactID(rawID)
		if err != nil {
			return nil, storeError("decode artifact reference", err)
		}
		refs[id] = size
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate artifact references", err)
	}
	return refs, nil
}

// DeleteSession removes a session and all of its persisted data. Events,
// checkpoints and artifact_refs cascade from the sessions row; file_changes
// and memory_jobs carry no foreign key and are deleted explicitly. All
// three deletes run in one transaction (review M29): a failure must never
// leave a half-deleted session behind.
func (s *SQLiteStore) DeleteSession(ctx context.Context, sessionID domain.SessionID) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin delete session transaction", err)
	}
	defer func() { _ = tx.Rollback() }()
	if _, err := tx.ExecContext(ctx,
		"DELETE FROM file_changes WHERE session_id = ?", sessionID.String()); err != nil {
		return storeError("delete session file changes", err)
	}
	if _, err := tx.ExecContext(ctx,
		"DELETE FROM memory_jobs WHERE session_id = ?", sessionID.String()); err != nil {
		return storeError("delete session memory job", err)
	}
	res, err := tx.ExecContext(ctx,
		"DELETE FROM sessions WHERE session_id = ?", sessionID.String())
	if err != nil {
		return storeError("delete session", err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return storeError("delete session result", err)
	}
	if affected == 0 {
		return fmt.Errorf("session not found: %s", sessionID)
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit delete session transaction", err)
	}
	return nil
}

// SetSessionArchived marks a session archived (hidden from default
// listings, read-only) or restores it to active. It reports whether the
// state changed; repeating the current state is a no-op.
//
// Archiving takes effect immediately even for a session with an
// in-flight turn: the turn's next event append fails with
// ErrSessionArchived and the turn aborts. That is the intended trade-off
// — "put it away" wins over letting a background turn keep writing.
// The auto-archiver does not hit this in practice (an actively-writing
// turn keeps updated_at fresh, so the sweep does not select it), though
// a turn stalled on an approval or a slow model for longer than the
// configured threshold can still be swept — it then fails on its next
// append, exactly as with a manual archive.
//
// The not-found SELECT after a no-op UPDATE can race a concurrent
// delete/state change on another connection; that only affects the
// changed report and error attribution, never correctness (the UPDATE
// itself is atomic and idempotent).
func (s *SQLiteStore) SetSessionArchived(ctx context.Context, sessionID domain.SessionID, archived bool) (bool, error) {
	var at any
	archivedFlag := 0
	if archived {
		at = time.Now().UTC().UnixNano()
		archivedFlag = 1
	}
	res, err := s.db.ExecContext(ctx, `
UPDATE sessions SET archived_at_unix_nano = ?
WHERE session_id = ? AND (archived_at_unix_nano IS NOT NULL) != ?`,
		at, sessionID.String(), archivedFlag)
	if err != nil {
		return false, storeError("archive session", err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return false, storeError("archive session result", err)
	}
	if affected == 1 {
		return true, nil
	}
	// No row updated: either already in the requested state or no such
	// session — distinguish so a typo'd ID still fails loudly.
	var exists int
	if err := s.db.QueryRowContext(ctx,
		"SELECT 1 FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&exists); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return false, fmt.Errorf("session not found: %s", sessionID)
		}
		return false, storeError("check session for archive", err)
	}
	return false, nil
}

// IsSessionArchived reports whether the session is currently archived.
// A missing session gets the same plain "session not found" error as
// SetSessionArchived, so HTTP mapping lands on 404 for both.
func (s *SQLiteStore) IsSessionArchived(ctx context.Context, sessionID domain.SessionID) (bool, error) {
	var archivedAt sql.NullInt64
	if err := s.db.QueryRowContext(ctx,
		"SELECT archived_at_unix_nano FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&archivedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return false, fmt.Errorf("session not found: %s", sessionID)
		}
		return false, storeError("check session archive state", err)
	}
	return archivedAt.Valid, nil
}

// DeleteExpiredArchivedSessions permanently removes every session archived
// at or before cutoff, together with all of its persisted data, and returns
// the deleted session IDs. It is the retention sweep behind
// sessions.gc_archived_after: archiving is reversible, this is not.
//
// The candidate scan and the deletes ride one transaction, which BEGIN
// IMMEDIATE (see OpenSQLiteStore) takes the write lock up front — a
// concurrent unarchive (SetSessionArchived) serializes against the whole
// pass, so a session restored between scan and delete can never be
// removed. Like ArchiveStaleSessions the sweep is idempotent and safe for
// concurrent loom processes sharing the database.
func (s *SQLiteStore) DeleteExpiredArchivedSessions(ctx context.Context, cutoff time.Time) ([]domain.SessionID, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, storeError("begin purge transaction", err)
	}
	defer func() { _ = tx.Rollback() }()
	rows, err := tx.QueryContext(ctx, `
SELECT session_id FROM sessions
WHERE archived_at_unix_nano IS NOT NULL AND archived_at_unix_nano <= ?`, cutoff.UnixNano())
	if err != nil {
		return nil, storeError("scan expired archived sessions", err)
	}
	var ids []domain.SessionID
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			_ = rows.Close()
			return nil, storeError("scan expired archived session", err)
		}
		id, err := domain.ParseSessionID(raw)
		if err != nil {
			_ = rows.Close()
			return nil, storeError("decode expired session ID", err)
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		_ = rows.Close()
		return nil, storeError("iterate expired archived sessions", err)
	}
	_ = rows.Close()
	for _, id := range ids {
		if _, err := tx.ExecContext(ctx,
			"DELETE FROM file_changes WHERE session_id = ?", id.String()); err != nil {
			return nil, storeError("delete purged session file changes", err)
		}
		if _, err := tx.ExecContext(ctx,
			"DELETE FROM memory_jobs WHERE session_id = ?", id.String()); err != nil {
			return nil, storeError("delete purged session memory job", err)
		}
		// Events, checkpoints, artifact_refs and session_shares cascade
		// from the sessions row.
		if _, err := tx.ExecContext(ctx,
			"DELETE FROM sessions WHERE session_id = ?", id.String()); err != nil {
			return nil, storeError("delete purged session", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, storeError("commit purge transaction", err)
	}
	return ids, nil
}

// ArchiveStaleSessions marks every unarchived session whose last activity
// (updated_at) is at or before cutoff as archived, and reports how many
// sessions this call archived. It is a single atomic UPDATE — idempotent
// and safe for concurrent loom processes sharing the database. Read paths
// (listing, inspect, resume-for-viewing) are unaffected: archiving only
// hides a session from the default view and blocks further writes until
// it is explicitly unarchived.
func (s *SQLiteStore) ArchiveStaleSessions(ctx context.Context, cutoff time.Time) (int64, error) {
	res, err := s.db.ExecContext(ctx, `
UPDATE sessions SET archived_at_unix_nano = ?
WHERE archived_at_unix_nano IS NULL AND updated_at_unix_nano <= ?`,
		time.Now().UTC().UnixNano(), cutoff.UnixNano())
	if err != nil {
		return 0, storeError("archive stale sessions", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return 0, storeError("archive stale sessions result", err)
	}
	return n, nil
}

func addArtifactRefs(ctx context.Context, tx *sql.Tx, sessionID domain.SessionID, refs map[domain.ArtifactID]int64) error {
	for id, size := range refs {
		if _, err := tx.ExecContext(ctx, `
INSERT INTO artifact_refs(session_id, artifact_id, size) VALUES (?, ?, ?)
ON CONFLICT(session_id, artifact_id) DO UPDATE SET size = MAX(size, excluded.size)`,
			sessionID.String(), id.String(), size); err != nil {
			return storeError("insert session artifact reference", err)
		}
	}
	return nil
}

func checkpointArtifactRefs(checkpoint domain.Checkpoint) map[domain.ArtifactID]int64 {
	refs := make(map[domain.ArtifactID]int64)
	for _, message := range checkpoint.Messages {
		for _, ref := range message.ArtifactRefs() {
			addArtifactRef(refs, ref)
		}
		// Compaction replacement messages can only carry text parts, so the
		// artifact references their payload depends on travel in metadata.
		if encoded := message.Metadata[domain.MetadataCompactedArtifacts]; encoded != "" {
			var metaRefs []domain.ArtifactRef
			if err := json.Unmarshal([]byte(encoded), &metaRefs); err == nil {
				for _, ref := range metaRefs {
					addArtifactRef(refs, ref)
				}
			}
		}
	}
	return refs
}

// addArtifactRef merges one reference into the map, keeping the max size.
func addArtifactRef(refs map[domain.ArtifactID]int64, ref domain.ArtifactRef) {
	if ref.ID.IsZero() || ref.Size < 0 {
		return
	}
	if old, ok := refs[ref.ID]; !ok || ref.Size > old {
		refs[ref.ID] = ref.Size
	}
}

// surfaceDirectiveArtifactRefs extracts the artifact references carried by
// surface directive events (docs/SURFACE_DESIGN.md §4.6): masked outputs
// and archived spans, including the archive marker's inherited reference
// list. Scanning them keeps directive-referenced artifacts alive even when
// every checkpoint of the session is lost — the log alone must suffice to
// rebuild the reference graph (checkpoint as pure cache, §2 principle 4).
//
// context.summarized is deliberately absent: buildSummaryReplacement
// produces text-only messages (verbatim user text + summary bridge), so a
// replacement currently carries no references. If that ever changes, add
// the case here.
func surfaceDirectiveArtifactRefs(evt domain.Event) ([]domain.ArtifactRef, error) {
	switch evt.Type {
	case domain.EventContextMasked:
		var payload domain.ContextMaskedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			return nil, fmt.Errorf("decode context.masked payload: %w", err)
		}
		refs := make([]domain.ArtifactRef, 0, len(payload.Masks))
		for _, mask := range payload.Masks {
			refs = append(refs, mask.Artifact)
		}
		return refs, nil
	case domain.EventContextArchived:
		var payload domain.ContextArchivedPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			return nil, fmt.Errorf("decode context.archived payload: %w", err)
		}
		refs := []domain.ArtifactRef{payload.Artifact}
		if encoded := payload.Marker.Metadata[domain.MetadataCompactedArtifacts]; encoded != "" {
			var metaRefs []domain.ArtifactRef
			if err := json.Unmarshal([]byte(encoded), &metaRefs); err == nil {
				refs = append(refs, metaRefs...)
			}
		}
		return refs, nil
	default:
		return nil, nil
	}
}

func formatTime(value time.Time) string {
	return value.UTC().Format(time.RFC3339Nano)
}

func storeError(operation string, err error) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, context.Canceled) {
		return domain.NewError(domain.ErrCancelled, operation, domain.WithCause(err))
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return domain.NewError(domain.ErrTimeout, operation, domain.WithCause(err))
	}
	message := strings.ToLower(err.Error())
	if strings.Contains(message, "database is locked") || strings.Contains(message, "database is busy") {
		return domain.NewError(domain.ErrUnavailable, operation, domain.WithRetryable(true), domain.WithCause(err))
	}
	return domain.NewError(domain.ErrInternal, operation, domain.WithCause(err))
}

func isUniqueConstraint(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(message, "unique constraint failed") ||
		strings.Contains(message, "primary key constraint failed")
}

// --- app_prefs: process-level preferences (schema v7) ---

// GetPref returns the value stored for key, or "" when the key was never
// set. Unknown keys are not an error — preferences are optional by design.
func (s *SQLiteStore) GetPref(ctx context.Context, key string) (string, error) {
	var value string
	err := s.db.QueryRowContext(ctx,
		"SELECT pref_value FROM app_prefs WHERE pref_key = ?", key).Scan(&value)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	if err != nil {
		return "", storeError("get app pref", err)
	}
	return value, nil
}

// SetPref upserts a process-level preference value.
func (s *SQLiteStore) SetPref(ctx context.Context, key, value string) error {
	_, err := s.db.ExecContext(ctx, `
INSERT INTO app_prefs(pref_key, pref_value) VALUES(?, ?)
ON CONFLICT(pref_key) DO UPDATE SET pref_value = excluded.pref_value`, key, value)
	if err != nil {
		return storeError("set app pref", err)
	}
	return nil
}

// --- session_shares: public read-only share links (schema v8) ---

// GetOrCreateShare returns the session's active share token, creating one
// on first use (idempotent: repeated calls return the same token until the
// share is revoked). The token is 128 bits of randomness, hex-encoded.
func (s *SQLiteStore) GetOrCreateShare(ctx context.Context, sessionID domain.SessionID) (string, error) {
	if sessionID.IsZero() {
		return "", domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	var token string
	err := s.db.QueryRowContext(ctx,
		"SELECT share_token FROM session_shares WHERE session_id = ?", sessionID.String()).Scan(&token)
	if err == nil {
		return token, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", storeError("load session share", err)
	}
	var exists int
	if err := s.db.QueryRowContext(ctx,
		"SELECT 1 FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&exists); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return "", domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return "", storeError("find session", err)
	}
	var tokenBytes [16]byte
	if _, err := rand.Read(tokenBytes[:]); err != nil {
		return "", storeError("generate share token", err)
	}
	token = hex.EncodeToString(tokenBytes[:])
	now := time.Now().UTC()
	// Insert-or-ignore then read back the surviving row: under concurrent
	// creation the loser must return the winner's persisted token. The
	// previous DO UPDATE form let the last writer overwrite the row, leaving
	// the earlier caller with a token the store no longer honours
	// (REVIEW H14).
	if _, err := s.db.ExecContext(ctx, `
INSERT INTO session_shares(session_id, share_token, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?)
ON CONFLICT(session_id) DO NOTHING`,
		sessionID.String(), token, formatTime(now), now.UnixNano()); err != nil {
		return "", storeError("create session share", err)
	}
	if err := s.db.QueryRowContext(ctx,
		"SELECT share_token FROM session_shares WHERE session_id = ?", sessionID.String()).Scan(&token); err != nil {
		return "", storeError("load session share", err)
	}
	return token, nil
}

// ResolveShare maps a share token back to its session. A revoked or never
// created share yields ErrInvalidInput("share not found").
func (s *SQLiteStore) ResolveShare(ctx context.Context, token string) (domain.SessionID, error) {
	token = strings.TrimSpace(token)
	if token == "" {
		return domain.SessionID{}, domain.NewError(domain.ErrInvalidInput, "share not found")
	}
	var rawSession string
	err := s.db.QueryRowContext(ctx,
		"SELECT session_id FROM session_shares WHERE share_token = ?", token).Scan(&rawSession)
	if errors.Is(err, sql.ErrNoRows) {
		return domain.SessionID{}, domain.NewError(domain.ErrInvalidInput, "share not found")
	}
	if err != nil {
		return domain.SessionID{}, storeError("resolve share token", err)
	}
	sessionID, err := domain.ParseSessionID(rawSession)
	if err != nil {
		return domain.SessionID{}, storeError("decode shared session ID", err)
	}
	return sessionID, nil
}

// DeleteShare revokes the session's share link (idempotent).
func (s *SQLiteStore) DeleteShare(ctx context.Context, sessionID domain.SessionID) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	if _, err := s.db.ExecContext(ctx,
		"DELETE FROM session_shares WHERE session_id = ?", sessionID.String()); err != nil {
		return storeError("delete session share", err)
	}
	return nil
}

// HasArtifactRef reports whether the session's durable projections reference
// the artifact — the authorization check for serving artifact bytes through
// a share link (only artifacts the shared session actually rendered).
func (s *SQLiteStore) HasArtifactRef(ctx context.Context, sessionID domain.SessionID, artifactID domain.ArtifactID) (bool, error) {
	var exists int
	err := s.db.QueryRowContext(ctx,
		"SELECT 1 FROM artifact_refs WHERE session_id = ? AND artifact_id = ?",
		sessionID.String(), artifactID.String()).Scan(&exists)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, storeError("check shared artifact reference", err)
	}
	return true, nil
}
