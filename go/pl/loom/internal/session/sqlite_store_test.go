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
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestSQLiteStorePersistsEventsAcrossReopen(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	events := transcriptEvents(t, sessionID)
	if err := store.AppendEvents(ctx, sessionID, 0, events); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	store = openTestSQLiteStore(t, path)
	loaded, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(loaded) != len(events) {
		t.Fatalf("loaded %d events, want %d", len(loaded), len(events))
	}
	for i := range loaded {
		if loaded[i].ID != events[i].ID || loaded[i].Sequence != events[i].Sequence ||
			loaded[i].Type != events[i].Type || !loaded[i].Timestamp.Equal(events[i].Timestamp) ||
			string(loaded[i].Payload) != string(events[i].Payload) {
			t.Fatalf("event[%d] mismatch: got %+v want %+v", i, loaded[i], events[i])
		}
	}
	transcript, err := Replay(loaded)
	if err != nil {
		t.Fatalf("Replay: %v", err)
	}
	if len(transcript.Messages) != 2 || transcript.Messages[1].TextParts()[0] != "world" {
		t.Fatalf("unexpected transcript: %+v", transcript.Messages)
	}
}

func TestSQLiteStoreLoadEventsAfterSequence(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.AppendEvents(ctx, sessionID, 0, transcriptEvents(t, sessionID)); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
	loaded, err := store.LoadEvents(ctx, sessionID, 1)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(loaded) != 2 || loaded[0].Sequence != 2 || loaded[1].Sequence != 3 {
		t.Fatalf("unexpected filtered events: %+v", loaded)
	}
}

func TestSQLiteStoreRejectsStaleVersionAndInvalidBatchAtomically(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	first := transcriptEvents(t, sessionID)[:1]
	if err := store.AppendEvents(ctx, sessionID, 0, first); err != nil {
		t.Fatalf("AppendEvents first: %v", err)
	}
	if err := store.AppendEvents(ctx, sessionID, 0, first); errorCode(err) != domain.ErrConflict {
		t.Fatalf("stale append error = %v, want conflict", err)
	}

	invalid := []domain.Event{
		newEvent(sessionID, 2, domain.EventRunCreated, nil),
		newEvent(sessionID, 4, domain.EventRunStateChanged, nil),
	}
	if err := store.AppendEvents(ctx, sessionID, 1, invalid); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("invalid batch error = %v, want invalid_input", err)
	}
	loaded, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(loaded) != 1 || loaded[0].Sequence != 1 {
		t.Fatalf("invalid batch partially persisted: %+v", loaded)
	}
}

func TestSQLiteStoreAppendEventsAndCheckpointIsAtomic(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	events := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	bad := testCheckpoint(sessionID, 1, time.Now().UTC())
	bad.ID = domain.CheckpointID{}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events, bad); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("invalid checkpoint error = %v, want invalid_input", err)
	}
	loaded, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(loaded) != 0 {
		t.Fatalf("events partially persisted: %+v", loaded)
	}

	checkpoint := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events, checkpoint); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession: %v", err)
	}
	if inspection.Session.Version != 1 || inspection.Checkpoint == nil || inspection.Checkpoint.ID != checkpoint.ID || len(inspection.Events) != 1 {
		t.Fatalf("unexpected atomic persistence result: %+v", inspection)
	}
}

func TestSQLiteStoreConcurrentAppendHasSingleWinner(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	start := make(chan struct{})
	errorsCh := make(chan error, 2)
	var wait sync.WaitGroup
	for range 2 {
		wait.Add(1)
		go func() {
			defer wait.Done()
			<-start
			errorsCh <- store.AppendEvents(ctx, sessionID, 0, []domain.Event{
				newEvent(sessionID, 1, domain.EventSessionCreated, nil),
			})
		}()
	}
	close(start)
	wait.Wait()
	close(errorsCh)

	successes, conflicts := 0, 0
	for err := range errorsCh {
		switch errorCode(err) {
		case "":
			successes++
		case domain.ErrConflict:
			conflicts++
		default:
			t.Fatalf("unexpected append error: %v", err)
		}
	}
	if successes != 1 || conflicts != 1 {
		t.Fatalf("successes=%d conflicts=%d, want 1 and 1", successes, conflicts)
	}
}

func TestSQLiteStoreCheckpointRoundTripAndLatest(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.AppendEvents(ctx, sessionID, 0, transcriptEvents(t, sessionID)); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}

	base := time.Date(2026, 7, 23, 10, 0, 0, 123, time.UTC)
	first := testCheckpoint(sessionID, 1, base)
	latest := testCheckpoint(sessionID, 3, base.Add(time.Second))
	if err := store.SaveCheckpoint(ctx, first); err != nil {
		t.Fatalf("SaveCheckpoint first: %v", err)
	}
	if err := store.SaveCheckpoint(ctx, latest); err != nil {
		t.Fatalf("SaveCheckpoint latest: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	store = openTestSQLiteStore(t, path)
	loaded, err := store.LoadLatestCheckpoint(ctx, sessionID)
	if err != nil {
		t.Fatalf("LoadLatestCheckpoint: %v", err)
	}
	if loaded.ID != latest.ID || loaded.Sequence != latest.Sequence ||
		len(loaded.Messages) != 1 || loaded.Messages[0].TextParts()[0] != "checkpoint" ||
		len(loaded.Plan.Items) != 1 || loaded.Usage.ToolCalls != 2 {
		t.Fatalf("checkpoint mismatch: got %+v want %+v", loaded, latest)
	}
}

func TestSQLiteStoreLatestCheckpointUsesChronologicalNanoseconds(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.AppendEvents(ctx, sessionID, 0, transcriptEvents(t, sessionID)); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
	base := time.Date(2026, 7, 23, 10, 0, 0, 0, time.UTC)
	older := testCheckpoint(sessionID, 3, base.Add(120*time.Millisecond))
	newer := testCheckpoint(sessionID, 3, base.Add(123*time.Millisecond))
	if err := store.SaveCheckpoint(ctx, older); err != nil {
		t.Fatalf("SaveCheckpoint older: %v", err)
	}
	if err := store.SaveCheckpoint(ctx, newer); err != nil {
		t.Fatalf("SaveCheckpoint newer: %v", err)
	}
	loaded, err := store.LoadLatestCheckpoint(ctx, sessionID)
	if err != nil {
		t.Fatalf("LoadLatestCheckpoint: %v", err)
	}
	if loaded.ID != newer.ID {
		t.Fatalf("loaded checkpoint %s, want newer %s", loaded.ID, newer.ID)
	}
}

func TestSQLiteStoreInspectSessionRecoversFromLatestCheckpoint(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	events := transcriptEvents(t, sessionID)
	if err := store.AppendEvents(ctx, sessionID, 0, events); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 1,
		State:     domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		CreatedAt: time.Now().UTC(),
	}
	if err := store.SaveCheckpoint(ctx, checkpoint); err != nil {
		t.Fatalf("SaveCheckpoint: %v", err)
	}

	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession: %v", err)
	}
	if inspection.Session.ID != sessionID || inspection.Session.Version != 3 {
		t.Fatalf("unexpected session: %+v", inspection.Session)
	}
	if inspection.Checkpoint == nil || inspection.Checkpoint.ID != checkpoint.ID {
		t.Fatalf("unexpected checkpoint: %+v", inspection.Checkpoint)
	}
	if len(inspection.Events) != 3 || inspection.Transcript.LastEventSequence != 3 ||
		len(inspection.Transcript.Messages) != 2 || inspection.Transcript.Messages[1].TextParts()[0] != "world" {
		t.Fatalf("unexpected inspection: %+v", inspection)
	}
}

func TestSQLiteStoreInspectEmptySessionWithoutCheckpoint(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession: %v", err)
	}
	if inspection.Checkpoint != nil || len(inspection.Events) != 0 || len(inspection.Transcript.Messages) != 0 ||
		inspection.Transcript.SessionID != sessionID || inspection.Transcript.LastEventSequence != 0 {
		t.Fatalf("unexpected empty inspection: %+v", inspection)
	}
	if _, err := store.InspectSession(ctx, domain.NewSessionID()); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("missing session error = %v, want invalid_input", err)
	}
}

func TestSQLiteStoreReadOnlyOpenDoesNotAllowWrites(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	readOnly, err := OpenSQLiteStoreReadOnly(ctx, path)
	if err != nil {
		t.Fatalf("OpenSQLiteStoreReadOnly: %v", err)
	}
	defer readOnly.Close()
	if summaries, err := readOnly.ListSessions(ctx, 10); err != nil || len(summaries) != 1 {
		t.Fatalf("ListSessions summaries=%+v error=%v", summaries, err)
	}
	if inspection, err := readOnly.InspectSession(ctx, sessionID); err != nil || inspection.Session.ID != sessionID {
		t.Fatalf("InspectSession inspection=%+v error=%v", inspection, err)
	}
	if err := readOnly.CreateSession(ctx, domain.NewSessionID()); err == nil {
		t.Fatal("read-only store allowed session creation")
	}
}

func TestSQLiteStoreListSessionsMostRecentlyUpdatedFirst(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	first := domain.NewSessionID()
	second := domain.NewSessionID()
	if err := store.CreateSession(ctx, first); err != nil {
		t.Fatalf("CreateSession first: %v", err)
	}
	if err := store.CreateSession(ctx, second); err != nil {
		t.Fatalf("CreateSession second: %v", err)
	}
	if err := store.AppendEvents(ctx, first, 0, []domain.Event{
		newEvent(first, 1, domain.EventSessionCreated, nil),
	}); err != nil {
		t.Fatalf("AppendEvents first: %v", err)
	}
	summaries, err := store.ListSessions(ctx, 10)
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	if len(summaries) != 2 || summaries[0].ID != first || summaries[0].Version != 1 || summaries[1].ID != second {
		t.Fatalf("unexpected summaries: %+v", summaries)
	}
	if _, err := store.ListSessions(ctx, 0); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("ListSessions invalid limit error = %v", err)
	}
}

func TestSQLiteStoreIndexesArtifactReferencesAcrossCheckpoints(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	firstID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("1", 64))
	secondID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("2", 64))
	first := testCheckpoint(sessionID, 0, time.Now().UTC())
	first.Messages[0].Parts = []domain.ContentPart{{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: firstID, Size: 12}}}
	if err := store.SaveCheckpoint(ctx, first); err != nil {
		t.Fatalf("SaveCheckpoint first: %v", err)
	}
	second := testCheckpoint(sessionID, 0, time.Now().UTC().Add(time.Second))
	second.Messages[0].Parts = []domain.ContentPart{{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: secondID, Size: 23}}}
	if err := store.SaveCheckpoint(ctx, second); err != nil {
		t.Fatalf("SaveCheckpoint second: %v", err)
	}
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		t.Fatalf("ListArtifactRefs: %v", err)
	}
	if len(refs) != 2 || refs[firstID] != 12 || refs[secondID] != 23 {
		t.Fatalf("unexpected artifact refs: %+v", refs)
	}
}

func TestSQLiteStoreMigratesVersionOneArtifactReferences(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	artifactID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("3", 64))
	checkpoint := testCheckpoint(sessionID, 0, time.Now().UTC())
	checkpoint.Messages[0].Parts = []domain.ContentPart{{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: artifactID, Size: 34}}}
	data, err := json.Marshal(checkpoint)
	if err != nil {
		t.Fatalf("Marshal checkpoint: %v", err)
	}
	if _, err := store.db.ExecContext(ctx, `
INSERT INTO checkpoints(checkpoint_id, session_id, sequence, data, created_at, created_at_unix_nano)
VALUES (?, ?, ?, ?, ?, ?)`, checkpoint.ID.String(), sessionID.String(), 0, data,
		formatTime(checkpoint.CreatedAt), checkpoint.CreatedAt.UnixNano()); err != nil {
		t.Fatalf("insert legacy checkpoint: %v", err)
	}
	if _, err := store.db.ExecContext(ctx, "DELETE FROM artifact_refs"); err != nil {
		t.Fatalf("clear artifact refs: %v", err)
	}
	if _, err := store.db.ExecContext(ctx, "DELETE FROM schema_migrations WHERE version = 2"); err != nil {
		t.Fatalf("downgrade schema marker: %v", err)
	}
	if _, err := store.db.ExecContext(ctx, "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (1, ?)", formatTime(time.Now().UTC())); err != nil {
		t.Fatalf("record v1 marker: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	store = openTestSQLiteStore(t, path)
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		t.Fatalf("ListArtifactRefs: %v", err)
	}
	if len(refs) != 1 || refs[artifactID] != 34 {
		t.Fatalf("migrated refs = %+v", refs)
	}
}

func TestSQLiteStoreRejectsCheckpointAheadOfSession(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	checkpoint := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.SaveCheckpoint(ctx, checkpoint); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("SaveCheckpoint error = %v, want invalid_input", err)
	}
}

func TestSQLiteStoreRejectsNewerSchema(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	if _, err := store.db.ExecContext(ctx,
		"INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
		sqliteSchemaVersion+1, formatTime(time.Now().UTC())); err != nil {
		t.Fatalf("insert newer migration: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	_, err := OpenSQLiteStore(ctx, path)
	if errorCode(err) != domain.ErrUnavailable {
		t.Fatalf("OpenSQLiteStore error = %v, want unavailable", err)
	}
}

func TestSQLiteStoreContextCancellationAndDuplicateSession(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.CreateSession(ctx, sessionID); errorCode(err) != domain.ErrConflict {
		t.Fatalf("duplicate session error = %v, want conflict", err)
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if err := store.AppendEvents(cancelled, sessionID, 0, nil); !errors.Is(err, context.Canceled) || errorCode(err) != domain.ErrCancelled {
		t.Fatalf("cancelled AppendEvents error = %v, want cancelled code and context.Canceled in chain", err)
	}
}

func openTestSQLiteStore(t *testing.T, path string) *SQLiteStore {
	t.Helper()
	store, err := OpenSQLiteStore(context.Background(), path)
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })
	return store
}

func transcriptEvents(t *testing.T, sessionID domain.SessionID) []domain.Event {
	t.Helper()
	base := time.Date(2026, 7, 23, 9, 0, 0, 123, time.UTC)
	user := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}}, CreatedAt: base,
	}
	assistant := domain.Message{
		ID: domain.NewMessageID(), Sequence: 2, Role: domain.RoleAssistant,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "world"}}, CreatedAt: base.Add(time.Second),
	}
	return []domain.Event{
		newEventAt(sessionID, 1, domain.EventSessionCreated, nil, base),
		newEventAt(sessionID, 2, domain.EventUserMessageAdded, messagePayload(t, user), base.Add(time.Second)),
		newEventAt(sessionID, 3, domain.EventModelResponseCompleted, messagePayload(t, assistant), base.Add(2*time.Second)),
	}
}

func messagePayload(t *testing.T, message domain.Message) json.RawMessage {
	t.Helper()
	payload, err := domain.MarshalPayload(domain.MessageEventPayload{Message: message})
	if err != nil {
		t.Fatalf("MarshalPayload: %v", err)
	}
	return payload
}

func newEvent(sessionID domain.SessionID, sequence int64, eventType domain.EventType, payload json.RawMessage) domain.Event {
	return newEventAt(sessionID, sequence, eventType, payload, time.Now().UTC())
}

func newEventAt(sessionID domain.SessionID, sequence int64, eventType domain.EventType, payload json.RawMessage, timestamp time.Time) domain.Event {
	return domain.Event{
		ID: domain.NewEventID(), SessionID: sessionID, Sequence: sequence,
		Type: eventType, Timestamp: timestamp, Payload: payload,
	}
}

func testCheckpoint(sessionID domain.SessionID, sequence int64, createdAt time.Time) domain.Checkpoint {
	return domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: sequence,
		State: domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		Messages: []domain.Message{{
			ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1,
			Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "checkpoint"}}, CreatedAt: createdAt,
		}},
		Plan:      domain.Plan{Items: []domain.PlanItem{{Index: 0, Goal: "persist", Status: domain.PlanItemInProgress}}},
		Usage:     domain.Usage{Turns: 1, ToolCalls: 2, InputTokens: 3, OutputTokens: 4},
		CreatedAt: createdAt,
	}
}

func errorCode(err error) domain.ErrorCode {
	if err == nil {
		return ""
	}
	var agentError *domain.AgentError
	if errors.As(err, &agentError) {
		return agentError.Code
	}
	return domain.ErrInternal
}
