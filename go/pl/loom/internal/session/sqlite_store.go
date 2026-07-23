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
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	_ "modernc.org/sqlite"
)

const sqliteSchemaVersion = 1

// SQLiteStore persists session events and checkpoints in a SQLite database.
// A store serializes writes through one connection; optimistic versions still
// protect sessions from stale writers and make conflicts explicit.
type SQLiteStore struct {
	db *sql.DB
}

// SessionSummary is a lightweight persisted session view for listing sessions.
type SessionSummary struct {
	ID        domain.SessionID `json:"id"`
	Version   int64            `json:"version"`
	CreatedAt time.Time        `json:"created_at"`
	UpdatedAt time.Time        `json:"updated_at"`
}

// SessionInspection is a consistent read-only view of persisted session data.
type SessionInspection struct {
	Session    SessionSummary     `json:"session"`
	Checkpoint *domain.Checkpoint `json:"checkpoint,omitempty"`
	Transcript Transcript         `json:"transcript"`
	Events     []domain.Event     `json:"events"`
}

// OpenSQLiteStore opens or creates a SQLite event store and applies migrations.
func OpenSQLiteStore(ctx context.Context, path string) (*SQLiteStore, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "sqlite database path is required")
	}

	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, storeError("open sqlite database", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
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
	for _, statement := range []string{
		"PRAGMA foreign_keys = ON",
		"PRAGMA journal_mode = WAL",
		"PRAGMA synchronous = FULL",
		"PRAGMA busy_timeout = 5000",
	} {
		if _, err := s.db.ExecContext(ctx, statement); err != nil {
			return storeError("configure sqlite database", err)
		}
	}
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
    updated_at_unix_nano INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    event_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    sequence INTEGER NOT NULL CHECK (sequence > 0),
    type TEXT NOT NULL,
    timestamp TEXT NOT NULL,
    payload BLOB,
    UNIQUE (session_id, sequence),
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_events_session_sequence
    ON events(session_id, sequence);
CREATE TABLE IF NOT EXISTS checkpoints (
    checkpoint_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    sequence INTEGER NOT NULL CHECK (sequence >= 0),
    data BLOB NOT NULL,
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_checkpoints_session_sequence
    ON checkpoints(session_id, sequence DESC, created_at_unix_nano DESC);
`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return storeError("apply sqlite schema", err)
	}
	var newestVersion sql.NullInt64
	if err := s.db.QueryRowContext(ctx, "SELECT MAX(version) FROM schema_migrations").Scan(&newestVersion); err != nil {
		return storeError("read sqlite schema version", err)
	}
	if newestVersion.Valid && newestVersion.Int64 > sqliteSchemaVersion {
		return domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("sqlite schema version %d is newer than supported version %d", newestVersion.Int64, sqliteSchemaVersion))
	}
	_, err := s.db.ExecContext(ctx,
		"INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (?, ?)",
		sqliteSchemaVersion, formatTime(time.Now().UTC()))
	if err != nil {
		return storeError("record sqlite schema migration", err)
	}
	return nil
}

// CreateSession creates an empty session with version zero.
func (s *SQLiteStore) CreateSession(ctx context.Context, sessionID domain.SessionID) error {
	if sessionID.IsZero() {
		return domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	now := time.Now().UTC()
	_, err := s.db.ExecContext(ctx, `
INSERT INTO sessions(session_id, version, created_at, created_at_unix_nano, updated_at, updated_at_unix_nano)
VALUES (?, 0, ?, ?, ?, ?)`, sessionID.String(), formatTime(now), now.UnixNano(), formatTime(now), now.UnixNano())
	if err != nil {
		if isUniqueConstraint(err) {
			return domain.NewError(domain.ErrConflict, "session already exists", domain.WithCause(err))
		}
		return storeError("create session", err)
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

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin append transaction", err)
	}
	defer func() { _ = tx.Rollback() }()

	var actualVersion int64
	if err := tx.QueryRowContext(ctx,
		"SELECT version FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&actualVersion); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return storeError("load session version", err)
	}
	if actualVersion != expectedVersion {
		return domain.NewError(domain.ErrConflict,
			fmt.Sprintf("session version mismatch: expected %d, got %d", expectedVersion, actualVersion))
	}

	for i := range events {
		event := events[i]
		if _, err := tx.ExecContext(ctx, `
INSERT INTO events(event_id, session_id, sequence, type, timestamp, payload)
VALUES (?, ?, ?, ?, ?, ?)`, event.ID.String(), sessionID.String(), event.Sequence,
			string(event.Type), formatTime(event.Timestamp), []byte(event.Payload)); err != nil {
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
	for i, event := range events {
		if err := event.Validate(); err != nil {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("invalid event at index %d", i), domain.WithCause(err))
		}
		if event.SessionID != sessionID || event.Sequence != expectedVersion+int64(i)+1 {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("event at index %d is not contiguous for the session", i))
		}
	}
	data, err := json.Marshal(checkpoint)
	if err != nil {
		return domain.NewError(domain.ErrInvalidInput, "encode checkpoint", domain.WithCause(err))
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return storeError("begin append and checkpoint transaction", err)
	}
	defer func() { _ = tx.Rollback() }()
	var actualVersion int64
	if err := tx.QueryRowContext(ctx, "SELECT version FROM sessions WHERE session_id = ?", sessionID.String()).Scan(&actualVersion); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return storeError("load session version", err)
	}
	if actualVersion != expectedVersion {
		return domain.NewError(domain.ErrConflict,
			fmt.Sprintf("session version mismatch: expected %d, got %d", expectedVersion, actualVersion))
	}
	for i := range events {
		event := events[i]
		if _, err := tx.ExecContext(ctx, `
INSERT INTO events(event_id, session_id, sequence, type, timestamp, payload)
VALUES (?, ?, ?, ?, ?, ?)`, event.ID.String(), sessionID.String(), event.Sequence,
			string(event.Type), formatTime(event.Timestamp), []byte(event.Payload)); err != nil {
			if isUniqueConstraint(err) {
				return domain.NewError(domain.ErrConflict, "event already exists", domain.WithCause(err))
			}
			return storeError("insert event", err)
		}
	}
	now := time.Now().UTC()
	result, err := tx.ExecContext(ctx, `
UPDATE sessions SET version = ?, updated_at = ?, updated_at_unix_nano = ?
WHERE session_id = ? AND version = ?`, newVersion, formatTime(now), now.UnixNano(), sessionID.String(), expectedVersion)
	if err != nil {
		return storeError("advance session version", err)
	}
	if affected, err := result.RowsAffected(); err != nil {
		return storeError("inspect session version update", err)
	} else if affected != 1 {
		return domain.NewError(domain.ErrConflict, "session version changed while appending events")
	}
	if _, err := tx.ExecContext(ctx, `
INSERT INTO checkpoints(checkpoint_id, session_id, sequence, data, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?)`, checkpoint.ID.String(), sessionID.String(), checkpoint.Sequence,
		data, formatTime(checkpoint.CreatedAt), checkpoint.CreatedAt.UTC().UnixNano()); err != nil {
		if isUniqueConstraint(err) {
			return domain.NewError(domain.ErrConflict, "checkpoint already exists", domain.WithCause(err))
		}
		return storeError("save checkpoint", err)
	}
	if err := tx.Commit(); err != nil {
		return storeError("commit append and checkpoint transaction", err)
	}
	return nil
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
SELECT event_id, sequence, type, timestamp, payload
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
		if err := rows.Scan(&id, &sequence, &eventType, &timestamp, &payload); err != nil {
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
			Payload: append(json.RawMessage(nil), payload...),
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

// ListSessions returns persisted sessions ordered by most recent update.
func (s *SQLiteStore) ListSessions(ctx context.Context, limit int) ([]SessionSummary, error) {
	if limit <= 0 || limit > 1000 {
		return nil, domain.NewError(domain.ErrInvalidInput, "session list limit must be between 1 and 1000")
	}
	rows, err := s.db.QueryContext(ctx, `
SELECT session_id, version, created_at, updated_at
FROM sessions ORDER BY updated_at_unix_nano DESC, session_id DESC LIMIT ?`, limit)
	if err != nil {
		return nil, storeError("list sessions", err)
	}
	defer rows.Close()

	result := make([]SessionSummary, 0)
	for rows.Next() {
		var id, createdAt, updatedAt string
		var version int64
		if err := rows.Scan(&id, &version, &createdAt, &updatedAt); err != nil {
			return nil, storeError("scan session", err)
		}
		sessionID, err := domain.ParseSessionID(id)
		if err != nil {
			return nil, storeError("decode session ID", err)
		}
		created, err := time.Parse(time.RFC3339Nano, createdAt)
		if err != nil {
			return nil, storeError("decode session creation time", err)
		}
		updated, err := time.Parse(time.RFC3339Nano, updatedAt)
		if err != nil {
			return nil, storeError("decode session update time", err)
		}
		result = append(result, SessionSummary{
			ID: sessionID, Version: version, CreatedAt: created, UpdatedAt: updated,
		})
	}
	if err := rows.Err(); err != nil {
		return nil, storeError("iterate sessions", err)
	}
	return result, nil
}

// InspectSession returns session metadata, its latest checkpoint, the recovered
// transcript, and the complete event timeline from one consistent read snapshot.
func (s *SQLiteStore) InspectSession(ctx context.Context, sessionID domain.SessionID) (SessionInspection, error) {
	if sessionID.IsZero() {
		return SessionInspection{}, domain.NewError(domain.ErrInvalidInput, "session ID is required")
	}
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return SessionInspection{}, storeError("begin session inspection", err)
	}
	defer func() { _ = tx.Rollback() }()

	var summary SessionSummary
	var id, createdAt, updatedAt string
	if err := tx.QueryRowContext(ctx, `
SELECT session_id, version, created_at, updated_at
FROM sessions WHERE session_id = ?`, sessionID.String()).Scan(
		&id, &summary.Version, &createdAt, &updatedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return SessionInspection{}, domain.NewError(domain.ErrInvalidInput, "session not found")
		}
		return SessionInspection{}, storeError("load session metadata", err)
	}
	summary.ID, err = domain.ParseSessionID(id)
	if err != nil {
		return SessionInspection{}, storeError("decode session ID", err)
	}
	summary.CreatedAt, err = time.Parse(time.RFC3339Nano, createdAt)
	if err != nil {
		return SessionInspection{}, storeError("decode session creation time", err)
	}
	summary.UpdatedAt, err = time.Parse(time.RFC3339Nano, updatedAt)
	if err != nil {
		return SessionInspection{}, storeError("decode session update time", err)
	}

	var checkpoint *domain.Checkpoint
	var checkpointData []byte
	err = tx.QueryRowContext(ctx, `
SELECT data FROM checkpoints WHERE session_id = ?
ORDER BY sequence DESC, created_at_unix_nano DESC, checkpoint_id DESC LIMIT 1`, sessionID.String()).Scan(&checkpointData)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return SessionInspection{}, storeError("load latest checkpoint", err)
	}
	if err == nil {
		var decoded domain.Checkpoint
		if err := json.Unmarshal(checkpointData, &decoded); err != nil {
			return SessionInspection{}, storeError("decode checkpoint", err)
		}
		if decoded.ID.IsZero() || decoded.SessionID != sessionID || decoded.Sequence > summary.Version {
			return SessionInspection{}, storeError("validate persisted checkpoint", errors.New("checkpoint identity or sequence mismatch"))
		}
		checkpoint = &decoded
	}

	rows, err := tx.QueryContext(ctx, `
SELECT event_id, sequence, type, timestamp, payload
FROM events WHERE session_id = ? ORDER BY sequence ASC`, sessionID.String())
	if err != nil {
		return SessionInspection{}, storeError("load session events", err)
	}
	events, err := scanEvents(rows, sessionID)
	if err != nil {
		return SessionInspection{}, err
	}
	if int64(len(events)) != summary.Version {
		return SessionInspection{}, storeError("validate session event log",
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
		return SessionInspection{}, storeError("recover session transcript", err)
	}
	if transcript.LastEventSequence != summary.Version {
		return SessionInspection{}, storeError("validate recovered transcript",
			fmt.Errorf("recovered sequence is %d but session version is %d", transcript.LastEventSequence, summary.Version))
	}
	if err := tx.Commit(); err != nil {
		return SessionInspection{}, storeError("commit session inspection", err)
	}
	return SessionInspection{
		Session: summary, Checkpoint: checkpoint, Transcript: transcript, Events: events,
	}, nil
}

func scanEvents(rows *sql.Rows, sessionID domain.SessionID) ([]domain.Event, error) {
	defer rows.Close()
	result := make([]domain.Event, 0)
	for rows.Next() {
		var id, eventType, timestamp string
		var sequence int64
		var payload []byte
		if err := rows.Scan(&id, &sequence, &eventType, &timestamp, &payload); err != nil {
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
			Payload: append(json.RawMessage(nil), payload...),
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
	_, err = tx.ExecContext(ctx, `
INSERT INTO checkpoints(checkpoint_id, session_id, sequence, data, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?)`, checkpoint.ID.String(), checkpoint.SessionID.String(), checkpoint.Sequence,
		data, formatTime(checkpoint.CreatedAt), checkpoint.CreatedAt.UTC().UnixNano())
	if err != nil {
		if isUniqueConstraint(err) {
			return domain.NewError(domain.ErrConflict, "checkpoint already exists", domain.WithCause(err))
		}
		return storeError("save checkpoint", err)
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
