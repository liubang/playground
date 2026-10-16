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
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
)

var _ memory.Repository = (*SQLiteStore)(nil)

func memoryID(request string) string {
	hash := sha256.Sum256([]byte(request))
	return "mem_" + hex.EncodeToString(hash[:16])
}

const memoryRecordColumns = `id, scope, kind, content, rollout, revision, sequence, source_session_id, source_version, updated_at`

func scanMemoryRecord(row interface{ Scan(...any) error }) (memory.Record, error) {
	var r memory.Record
	var sessionID, updated string
	if err := row.Scan(&r.ID, &r.Scope, &r.Kind, &r.Content, &r.Rollout, &r.Revision, &r.Sequence, &sessionID, &r.SourceVersion, &updated); err != nil {
		return r, err
	}
	var err error
	if sessionID != "" {
		r.SessionID, err = domain.ParseSessionID(sessionID)
		if err != nil {
			return r, err
		}
	}
	r.UpdatedAt, err = time.Parse(time.RFC3339Nano, updated)
	return r, err
}

func advanceMemoryScope(ctx context.Context, tx *sql.Tx, scope string, revoke bool) (int64, error) {
	delta := 0
	if revoke {
		delta = 1
	}
	var version int64
	err := tx.QueryRowContext(ctx, `INSERT INTO memory_scope_state(scope, version, epoch) VALUES (?, 1, ?)
 ON CONFLICT(scope) DO UPDATE SET version = version + 1, epoch = epoch + ? RETURNING version`, scope, delta, delta).Scan(&version)
	if err != nil {
		return 0, storeError("advance memory scope", err)
	}
	if revoke {
		if _, err := tx.ExecContext(ctx, "DELETE FROM memory_views WHERE scope = ?", scope); err != nil {
			return 0, storeError("invalidate memory view", err)
		}
	}
	return version, nil
}

// validateMemorySource rejects deleted/rewound origins before even replaying a
// receipt. Real agent calls also require their durable execution-start identity,
// which cannot be recreated merely by reusing an event sequence after rewind.
func validateMemorySource(ctx context.Context, tx *sql.Tx, m memory.Mutation) error {
	source := domain.ToolContextFrom(ctx)
	if !source.SessionID.IsZero() && (source.SessionID != m.SessionID || source.SourceVersion != m.SourceVersion) {
		return domain.NewError(domain.ErrConflict, "memory origin differs from tool context")
	}
	if m.SessionID.IsZero() && m.SourceVersion == 0 {
		return nil
	}
	if m.SessionID.IsZero() || m.SourceVersion <= 0 {
		return domain.NewError(domain.ErrInvalidInput, "memory origin requires session and positive version")
	}
	var exists int
	err := tx.QueryRowContext(ctx, `SELECT 1 FROM sessions s WHERE s.session_id = ? AND s.version >= ?
 AND EXISTS (SELECT 1 FROM events e WHERE e.session_id = s.session_id AND e.sequence = ?)`, m.SessionID.String(), m.SourceVersion, m.SourceVersion).Scan(&exists)
	if errors.Is(err, sql.ErrNoRows) {
		return domain.NewError(domain.ErrConflict, "memory origin no longer exists")
	}
	if err != nil {
		return storeError("validate memory origin", err)
	}
	if !source.SessionID.IsZero() {
		err = tx.QueryRowContext(ctx, `SELECT 1 FROM events WHERE session_id = ? AND sequence <= ? AND type = ? AND json_extract(payload, '$.call_id') = ? LIMIT 1`, m.SessionID.String(), m.SourceVersion, domain.EventToolExecutionStarted, m.RequestID).Scan(&exists)
		if errors.Is(err, sql.ErrNoRows) {
			return domain.NewError(domain.ErrConflict, "memory execution-start event no longer exists")
		}
		if err != nil {
			return storeError("validate memory execution start", err)
		}
	}
	return nil
}

// forgetAutomaticEvidence deliberately revokes all automatic knowledge in the
// affected scope, without lexical matching. Global edits affect every workspace,
// including views built using globally visible evidence. The global cutoff also
// fences workspaces first created after this transaction.
func forgetAutomaticEvidence(ctx context.Context, tx *sql.Tx, scope string, cutoff int64) error {
	predicate := "kind = 'automatic' AND scope = ?"
	args := []any{scope}
	if scope == "" {
		predicate = "kind = 'automatic'"
		args = nil
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM memory_records WHERE "+predicate, args...); err != nil {
		return storeError("delete forgotten automatic evidence", err)
	}
	if scope == "" {
		if _, err := tx.ExecContext(ctx, `UPDATE memory_scope_state SET version = version + 1, epoch = epoch + 1 WHERE scope <> ''`); err != nil {
			return storeError("invalidate global-dependent memory scopes", err)
		}
		if _, err := tx.ExecContext(ctx, "DELETE FROM memory_views"); err != nil {
			return storeError("invalidate globally affected views", err)
		}
	}
	// MutateMemory increments this scope's version/epoch in the same transaction.
	_, err := tx.ExecContext(ctx, `INSERT INTO memory_scope_state(scope, evidence_cutoff_unix_nano) VALUES (?, ?)
 ON CONFLICT(scope) DO UPDATE SET evidence_cutoff_unix_nano = MAX(evidence_cutoff_unix_nano, excluded.evidence_cutoff_unix_nano)`, scope, cutoff)
	if err != nil {
		return storeError("publish memory forget barrier", err)
	}
	return nil
}

// MutateMemory commits the record and its tool-call receipt atomically.
func (s *SQLiteStore) MutateMemory(ctx context.Context, m memory.Mutation) (memory.Record, error) {
	if m.RequestID == "" || m.SourceVersion < 0 || (m.Forget && m.ID == "") || (m.ID != "" && m.Revision < 1) {
		return memory.Record{}, domain.NewError(domain.ErrInvalidInput, "invalid memory mutation identity or revision")
	}
	m.Content = strings.TrimSpace(memory.RedactSecrets(m.Content))
	if !m.Forget && (m.Content == "" || len(m.Content) > memory.MaxContentBytes || !utf8.ValidString(m.Content)) {
		return memory.Record{}, domain.NewError(domain.ErrInvalidInput, "invalid memory content")
	}
	payload, err := json.Marshal(m)
	if err != nil {
		return memory.Record{}, storeError("encode memory mutation", err)
	}
	hash := sha256.Sum256(payload)
	payloadHash := hex.EncodeToString(hash[:])
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return memory.Record{}, storeError("begin memory mutation", err)
	}
	defer tx.Rollback()
	if err := validateMemorySource(ctx, tx, m); err != nil {
		return memory.Record{}, err
	}
	var storedHash string
	var response []byte
	err = tx.QueryRowContext(ctx, "SELECT payload_hash, response FROM memory_requests WHERE request_id = ?", m.RequestID).Scan(&storedHash, &response)
	if err == nil {
		if storedHash != payloadHash {
			return memory.Record{}, domain.NewError(domain.ErrConflict, "memory request ID reused with different payload")
		}
		var r memory.Record
		if err := json.Unmarshal(response, &r); err != nil {
			return r, storeError("decode memory receipt", err)
		}
		// A scrubbed remember receipt was superseded by a later mutation.
		// Replaying it must neither report current visibility nor resurrect
		// revoked content. Forget receipts intentionally have empty content.
		if !m.Forget && r.Content == "" {
			return memory.Record{}, domain.NewError(domain.ErrConflict, "memory mutation was superseded; use list/read to inspect current records")
		}
		return r, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return memory.Record{}, storeError("read memory receipt", err)
	}
	if m.ID == "" {
		// Exact equality after redaction/trim is safe; semantic or case-folded
		// matching could collapse distinct facts. The write transaction serializes
		// concurrent remembers, and each call still receives its own receipt.
		r, err := scanMemoryRecord(tx.QueryRowContext(ctx, "SELECT "+memoryRecordColumns+" FROM memory_records WHERE scope = ? AND kind = 'explicit' AND content = ? ORDER BY sequence DESC, id LIMIT 1", m.Scope, m.Content))
		if err == nil {
			return commitMemoryMutation(ctx, tx, m, payloadHash, r)
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return memory.Record{}, storeError("find identical explicit memory", err)
		}
	}
	r := memory.Record{ID: memoryID(m.RequestID), Scope: m.Scope, Kind: memory.Explicit, Revision: 1}
	if m.ID != "" {
		r, err = scanMemoryRecord(tx.QueryRowContext(ctx, "SELECT "+memoryRecordColumns+" FROM memory_records WHERE id = ? AND scope = ?", m.ID, m.Scope))
		if errors.Is(err, sql.ErrNoRows) {
			return memory.Record{}, domain.NewError(domain.ErrInvalidInput, "memory not found in scope")
		}
		if err != nil {
			return memory.Record{}, storeError("read memory target", err)
		}
		if r.Revision != m.Revision {
			return memory.Record{}, domain.NewError(domain.ErrConflict, "memory revision mismatch")
		}
		if !m.Forget && r.Kind == memory.Explicit && r.Content == m.Content {
			return commitMemoryMutation(ctx, tx, m, payloadHash, r)
		}
		r.Revision++
		// Keep request identities and revision metadata for collision detection,
		// but erase obsolete full text instead of retaining memory history.
		if _, err := tx.ExecContext(ctx, `UPDATE memory_requests SET response = json_set(CAST(response AS TEXT), '$.content', '', '$.rollout', '') WHERE record_id = ?`, r.ID); err != nil {
			return memory.Record{}, storeError("scrub obsolete memory receipts", err)
		}
	}
	r.Sequence, err = advanceMemoryScope(ctx, tx, m.Scope, true)
	if err != nil {
		return memory.Record{}, err
	}
	r.Kind, r.Content, r.Rollout = memory.Explicit, m.Content, ""
	r.SessionID, r.SourceVersion, r.UpdatedAt = m.SessionID, m.SourceVersion, time.Now().UTC()
	var result sql.Result
	if m.Forget {
		r.Content = ""
		result, err = tx.ExecContext(ctx, "DELETE FROM memory_records WHERE id = ? AND scope = ?", r.ID, r.Scope)
	} else if m.ID == "" {
		result, err = tx.ExecContext(ctx, `INSERT INTO memory_records (`+memoryRecordColumns+`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.ID, r.Scope, r.Kind, r.Content, r.Rollout, r.Revision, r.Sequence, r.SessionID.String(), r.SourceVersion, formatTime(r.UpdatedAt))
	} else {
		result, err = tx.ExecContext(ctx, `UPDATE memory_records SET kind = ?, content = ?, rollout = '', revision = ?, sequence = ?, source_session_id = ?, source_version = ?, updated_at = ? WHERE id = ? AND scope = ?`,
			r.Kind, r.Content, r.Revision, r.Sequence, r.SessionID.String(), r.SourceVersion, formatTime(r.UpdatedAt), r.ID, r.Scope)
	}
	if err != nil {
		if isUniqueConstraint(err) {
			return memory.Record{}, domain.NewError(domain.ErrConflict, "memory ID already exists")
		}
		return memory.Record{}, storeError("write memory record", err)
	}
	if count, err := result.RowsAffected(); err != nil {
		return memory.Record{}, storeError("check memory mutation", err)
	} else if count != 1 {
		return memory.Record{}, domain.NewError(domain.ErrConflict, "memory target changed during mutation")
	}
	if m.ID != "" {
		// Promote an automatic target before clearing automatic evidence so the
		// authoritative replacement survives the scope-wide revocation.
		if err := forgetAutomaticEvidence(ctx, tx, m.Scope, time.Now().UTC().UnixNano()); err != nil {
			return memory.Record{}, err
		}
	}
	return commitMemoryMutation(ctx, tx, m, payloadHash, r)
}

// commitMemoryMutation gives every accepted request its own atomic receipt,
// including a no-op remember that reuses an unchanged authoritative record.
func commitMemoryMutation(ctx context.Context, tx *sql.Tx, m memory.Mutation, payloadHash string, r memory.Record) (memory.Record, error) {
	response, err := json.Marshal(r)
	if err != nil {
		return memory.Record{}, storeError("encode memory receipt", err)
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO memory_requests(request_id, payload_hash, record_id, source_session_id, source_version, response) VALUES (?, ?, ?, ?, ?, ?)`, m.RequestID, payloadHash, r.ID, m.SessionID.String(), m.SourceVersion, response); err != nil {
		return memory.Record{}, storeError("write memory receipt", err)
	}
	if err := tx.Commit(); err != nil {
		return memory.Record{}, storeError("commit memory mutation", err)
	}
	return r, nil
}

func (s *SQLiteStore) MemorySnapshot(ctx context.Context, scope string) (memory.Snapshot, error) {
	out := memory.Snapshot{Scope: scope, Records: []memory.Record{}}
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return out, storeError("begin memory snapshot", err)
	}
	defer tx.Rollback()
	err = tx.QueryRowContext(ctx, "SELECT version, epoch FROM memory_scope_state WHERE scope = ?", scope).Scan(&out.Version, &out.Epoch)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return out, storeError("read memory state", err)
	}
	rows, err := tx.QueryContext(ctx, "SELECT "+memoryRecordColumns+" FROM memory_records WHERE scope = ? ORDER BY CASE kind WHEN 'explicit' THEN 0 ELSE 1 END, sequence DESC, id", scope)
	if err != nil {
		return out, storeError("read memory records", err)
	}
	for rows.Next() {
		r, err := scanMemoryRecord(rows)
		if err != nil {
			rows.Close()
			return out, storeError("decode memory record", err)
		}
		out.Records = append(out.Records, r)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return out, storeError("iterate memory records", err)
	}
	err = tx.QueryRowContext(ctx, "SELECT version, epoch, main, summary FROM memory_views WHERE scope = ?", scope).Scan(&out.View.Version, &out.View.Epoch, &out.View.Main, &out.View.Summary)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return out, storeError("read memory view", err)
	}
	if err := tx.Commit(); err != nil {
		return out, storeError("finish memory snapshot", err)
	}
	return out, nil
}

// MemoryContext keeps global/workspace state, views and bounded evidence on
// one SQLite snapshot. It never advances view coverage to hide omitted records.
func (s *SQLiteStore) MemoryContext(ctx context.Context, workspace string) ([]memory.Snapshot, error) {
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return nil, storeError("begin memory context", err)
	}
	defer tx.Rollback()
	scopes := []string{""}
	if workspace != "" {
		scopes = append(scopes, workspace)
	}
	out := make([]memory.Snapshot, 0, len(scopes))
	for _, scope := range scopes {
		snapshot := memory.Snapshot{Scope: scope, Records: []memory.Record{}}
		err := tx.QueryRowContext(ctx, "SELECT version, epoch FROM memory_scope_state WHERE scope = ?", scope).Scan(&snapshot.Version, &snapshot.Epoch)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return nil, storeError("read context memory state", err)
		}
		err = tx.QueryRowContext(ctx, "SELECT version, epoch, main, summary FROM memory_views WHERE scope = ?", scope).Scan(&snapshot.View.Version, &snapshot.View.Epoch, &snapshot.View.Main, &snapshot.View.Summary)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return nil, storeError("read context memory view", err)
		}
		covered := int64(0)
		if snapshot.ValidView() {
			covered = snapshot.View.Version
		}
		for _, kind := range []memory.Kind{memory.Explicit, memory.Automatic} {
			limit := memory.ContextExplicitRecords
			lower := int64(0)
			if kind == memory.Automatic {
				limit = memory.ContextAutomaticRecords
				lower = covered
			}
			rows, err := tx.QueryContext(ctx, "SELECT "+memoryRecordColumns+" FROM memory_records WHERE scope = ? AND kind = ? AND sequence > ? ORDER BY updated_at DESC, id LIMIT ?", scope, kind, lower, limit+1)
			if err != nil {
				return nil, storeError("read bounded context records", err)
			}
			count := 0
			for rows.Next() {
				// The extra row signals omission without decoding/retaining its content.
				if count == limit {
					snapshot.Omitted = true
					break
				}
				record, err := scanMemoryRecord(rows)
				if err != nil {
					rows.Close()
					return nil, storeError("scan context memory record", err)
				}
				snapshot.Records = append(snapshot.Records, record)
				count++
			}
			err = rows.Err()
			rows.Close()
			if err != nil {
				return nil, storeError("iterate context memory records", err)
			}
		}
		out = append(out, snapshot)
	}
	if err := tx.Commit(); err != nil {
		return nil, storeError("finish memory context", err)
	}
	return out, nil
}

// ReadMemory point-reads only active evidence visible to the bound workspace.
func (s *SQLiteStore) ReadMemory(ctx context.Context, workspace, id string) (memory.Record, error) {
	record, err := scanMemoryRecord(s.db.QueryRowContext(ctx, "SELECT "+memoryRecordColumns+" FROM memory_records WHERE id = ? AND scope IN ('', ?)", id, workspace))
	if errors.Is(err, sql.ErrNoRows) {
		return memory.Record{}, domain.NewError(domain.ErrInvalidInput, "memory not found; use action=list or action=search to find active memories")
	}
	if err != nil {
		return memory.Record{}, storeError("read visible memory", err)
	}
	return record, nil
}

// QueryMemory performs filtering and limit+1 truncation in SQL. Only fixed SQL
// fragments are composed; scopes, terms and limits are always bound parameters.
// SQLite lower() provides ASCII case folding; non-ASCII text is matched literally.
func (s *SQLiteStore) QueryMemory(ctx context.Context, workspace, query string, limit int) (memory.SearchResult, error) {
	out := memory.SearchResult{Records: []memory.Record{}}
	if limit < 1 || limit > memory.MaxQueryRecords || len(query) > memory.MaxQueryBytes || !utf8.ValidString(query) {
		return out, domain.NewError(domain.ErrInvalidInput, "invalid memory query or limit")
	}
	var statement strings.Builder
	statement.WriteString("SELECT " + memoryRecordColumns + " FROM memory_records WHERE scope IN ('', ?)")
	args := []any{workspace}
	for term := range strings.FieldsSeq(query) {
		statement.WriteString(" AND instr(lower(content || char(10) || rollout), lower(?)) > 0")
		args = append(args, term)
	}
	statement.WriteString(" ORDER BY CASE kind WHEN 'explicit' THEN 0 ELSE 1 END, updated_at DESC, id LIMIT ?")
	args = append(args, limit+1)
	rows, err := s.db.QueryContext(ctx, statement.String(), args...)
	if err != nil {
		return out, storeError("query visible memory", err)
	}
	defer rows.Close()
	for rows.Next() {
		if len(out.Records) == limit {
			out.Truncated = true
			break
		}
		record, err := scanMemoryRecord(rows)
		if err != nil {
			return out, storeError("scan queried memory", err)
		}
		out.Records = append(out.Records, record)
	}
	if err := rows.Err(); err != nil {
		return out, storeError("iterate queried memory", err)
	}
	return out, nil
}

func (s *SQLiteStore) MemoryScopes(ctx context.Context) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT s.scope FROM memory_scope_state s LEFT JOIN memory_views v ON v.scope = s.scope WHERE v.scope IS NULL OR v.version < s.version OR v.epoch <> s.epoch ORDER BY s.scope`)
	if err != nil {
		return nil, storeError("list pending memory scopes", err)
	}
	defer rows.Close()
	var scopes []string
	for rows.Next() {
		var scope string
		if err := rows.Scan(&scope); err != nil {
			return nil, storeError("scan memory scope", err)
		}
		scopes = append(scopes, scope)
	}
	return scopes, rows.Err()
}

func (s *SQLiteStore) PublishMemoryView(ctx context.Context, snapshot memory.Snapshot, main, summary string) (bool, error) {
	if snapshot.Version <= 0 {
		return false, nil
	}
	result, err := s.db.ExecContext(ctx, `INSERT INTO memory_views(scope, version, epoch, main, summary)
 SELECT scope, ?, epoch, ?, ? FROM memory_scope_state WHERE scope = ? AND epoch = ? AND version >= ?
 ON CONFLICT(scope) DO UPDATE SET version = excluded.version, epoch = excluded.epoch, main = excluded.main, summary = excluded.summary
 WHERE memory_views.epoch = excluded.epoch AND memory_views.version < excluded.version`, snapshot.Version, main, summary, snapshot.Scope, snapshot.Epoch, snapshot.Version)
	if err != nil {
		return false, storeError("publish memory view", err)
	}
	n, err := result.RowsAffected()
	return n == 1, err
}

// revokeMemoryTx erases matching evidence, receipts and derived views together.
func revokeMemoryTx(ctx context.Context, tx *sql.Tx, predicate string, args ...any) error {
	rows, err := tx.QueryContext(ctx, "SELECT DISTINCT scope FROM memory_records WHERE "+predicate, args...)
	if err != nil {
		return storeError("find revoked memory scopes", err)
	}
	var scopes []string
	for rows.Next() {
		var scope string
		if err := rows.Scan(&scope); err != nil {
			rows.Close()
			return storeError("scan revoked scope", err)
		}
		scopes = append(scopes, scope)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return storeError("iterate revoked scopes", err)
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM memory_requests WHERE record_id IN (SELECT id FROM memory_records WHERE "+predicate+")", args...); err != nil {
		return storeError("delete revoked receipts", err)
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM memory_records WHERE "+predicate, args...); err != nil {
		return storeError("delete revoked memories", err)
	}
	for _, scope := range scopes {
		if _, err := advanceMemoryScope(ctx, tx, scope, true); err != nil {
			return err
		}
	}
	return nil
}
