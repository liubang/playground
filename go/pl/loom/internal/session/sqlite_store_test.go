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

func TestSQLiteStoreAppPrefs(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)

	// Unset keys read as empty without error.
	if v, err := store.GetPref(ctx, "model"); err != nil || v != "" {
		t.Fatalf("GetPref(unset) = %q, %v; want empty, nil", v, err)
	}
	// Set then read back.
	if err := store.SetPref(ctx, "model", "test/model-a"); err != nil {
		t.Fatalf("SetPref: %v", err)
	}
	if err := store.SetPref(ctx, "reasoning", "high"); err != nil {
		t.Fatalf("SetPref: %v", err)
	}
	if v, _ := store.GetPref(ctx, "model"); v != "test/model-a" {
		t.Fatalf("GetPref(model) = %q", v)
	}
	// Upsert overwrites.
	if err := store.SetPref(ctx, "model", "test/model-b"); err != nil {
		t.Fatalf("SetPref upsert: %v", err)
	}
	// Preferences survive a reopen (the whole point: restart persistence).
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	store = openTestSQLiteStore(t, path)
	if v, _ := store.GetPref(ctx, "model"); v != "test/model-b" {
		t.Fatalf("GetPref(model) after reopen = %q, want test/model-b", v)
	}
	if v, _ := store.GetPref(ctx, "reasoning"); v != "high" {
		t.Fatalf("GetPref(reasoning) after reopen = %q, want high", v)
	}
}

func TestSQLiteStoreSessionShares(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()

	// Sharing a missing session is an explicit not-found error.
	if _, err := store.GetOrCreateShare(ctx, sessionID); err == nil || !strings.Contains(err.Error(), "session not found") {
		t.Fatalf("GetOrCreateShare(missing session) error = %v, want session not found", err)
	}
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	// Create is idempotent: the same token comes back until revoked.
	token, err := store.GetOrCreateShare(ctx, sessionID)
	if err != nil {
		t.Fatalf("GetOrCreateShare: %v", err)
	}
	if len(token) != 32 {
		t.Fatalf("share token = %q, want 32 hex chars", token)
	}
	again, err := store.GetOrCreateShare(ctx, sessionID)
	if err != nil || again != token {
		t.Fatalf("GetOrCreateShare repeat = %q, %v; want idempotent %q", again, err, token)
	}
	resolved, err := store.ResolveShare(ctx, token)
	if err != nil || resolved != sessionID {
		t.Fatalf("ResolveShare = %v, %v; want %s", resolved, err, sessionID)
	}

	// Shares survive a reopen (restart persistence).
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	store = openTestSQLiteStore(t, path)
	if resolved, err := store.ResolveShare(ctx, token); err != nil || resolved != sessionID {
		t.Fatalf("ResolveShare after reopen = %v, %v; want %s", resolved, err, sessionID)
	}

	// Revoke is idempotent and kills the link.
	if err := store.DeleteShare(ctx, sessionID); err != nil {
		t.Fatalf("DeleteShare: %v", err)
	}
	if _, err := store.ResolveShare(ctx, token); err == nil || !strings.Contains(err.Error(), "share not found") {
		t.Fatalf("ResolveShare(revoked) error = %v, want share not found", err)
	}
	if err := store.DeleteShare(ctx, sessionID); err != nil {
		t.Fatalf("DeleteShare repeat: %v", err)
	}

	// A re-share after revoke mints a fresh token.
	fresh, err := store.GetOrCreateShare(ctx, sessionID)
	if err != nil || fresh == token {
		t.Fatalf("re-share after revoke = %q, %v; want a new token", fresh, err)
	}

	// Deleting the session cascades to its share.
	if err := store.DeleteSession(ctx, sessionID); err != nil {
		t.Fatalf("DeleteSession: %v", err)
	}
	if _, err := store.ResolveShare(ctx, fresh); err == nil {
		t.Fatalf("ResolveShare(deleted session) = nil error, want share not found")
	}
}

// Concurrent GetOrCreateShare callers must all observe the single persisted
// token (REVIEW H14): the former INSERT ... ON CONFLICT DO UPDATE let a later
// writer overwrite the row, leaving earlier callers holding a dead token.
func TestSQLiteStoreConcurrentShareCreationReturnsPersistedToken(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	for round := 0; round < 20; round++ {
		const workers = 8
		tokens := make([]string, workers)
		start := make(chan struct{})
		var wg sync.WaitGroup
		for i := range workers {
			wg.Add(1)
			go func() {
				defer wg.Done()
				<-start
				token, err := store.GetOrCreateShare(ctx, sessionID)
				if err != nil {
					t.Errorf("GetOrCreateShare: %v", err)
					return
				}
				tokens[i] = token
			}()
		}
		close(start)
		wg.Wait()

		persisted, err := store.GetOrCreateShare(ctx, sessionID)
		if err != nil {
			t.Fatalf("GetOrCreateShare: %v", err)
		}
		for i, token := range tokens {
			if token != persisted {
				t.Fatalf("round %d worker %d token = %q, want persisted %q", round, i, token, persisted)
			}
			if _, err := store.ResolveShare(ctx, token); err != nil {
				t.Fatalf("round %d worker %d token unresolvable: %v", round, i, err)
			}
		}
		if err := store.DeleteShare(ctx, sessionID); err != nil {
			t.Fatalf("DeleteShare: %v", err)
		}
	}
}

func TestSQLiteStorePersistsEventsAcrossReopen(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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
	if summaries, _, err := readOnly.ListSessions(ctx, "", 10, false, domain.WorkspaceID{}); err != nil || len(summaries) != 1 {
		t.Fatalf("ListSessions summaries=%+v error=%v", summaries, err)
	}
	if inspection, err := readOnly.InspectSession(ctx, sessionID); err != nil || inspection.Session.ID != sessionID {
		t.Fatalf("InspectSession inspection=%+v error=%v", inspection, err)
	}
	if err := readOnly.CreateSession(ctx, domain.NewSessionID(), domain.WorkspaceID{}); err == nil {
		t.Fatal("read-only store allowed session creation")
	}
}

func TestSQLiteStoreListSessionsMostRecentlyCreatedFirst(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	first := domain.NewSessionID()
	second := domain.NewSessionID()
	if err := store.CreateSession(ctx, first, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession first: %v", err)
	}
	if err := store.CreateSession(ctx, second, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession second: %v", err)
	}
	if err := store.AppendEvents(ctx, first, 0, []domain.Event{
		newEvent(first, 1, domain.EventSessionCreated, nil),
	}); err != nil {
		t.Fatalf("AppendEvents first: %v", err)
	}
	summaries, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	// second was created after first, so it leads the listing even though
	// first carries the more recent update.
	if len(summaries) != 2 || summaries[0].ID != second || summaries[1].ID != first || summaries[1].Version != 1 {
		t.Fatalf("unexpected summaries: %+v", summaries)
	}
	if _, _, err := store.ListSessions(ctx, "", 0, false, domain.WorkspaceID{}); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("ListSessions invalid limit error = %v", err)
	}
	if _, _, err := store.ListSessions(ctx, "bogus-cursor", 10, false, domain.WorkspaceID{}); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("ListSessions invalid cursor error = %v", err)
	}
}

// Keyset pagination: pages stitch without overlap or loss, and the final
// page reports an empty next cursor.
func TestSQLiteStoreListSessionsPagination(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	ids := make([]domain.SessionID, 5)
	for i := range ids {
		ids[i] = domain.NewSessionID()
		if err := store.CreateSession(ctx, ids[i], domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession %d: %v", i, err)
		}
		// Distinct creation order: append one event per session in id order.
		if err := store.AppendEvents(ctx, ids[i], 0, []domain.Event{
			newEvent(ids[i], 1, domain.EventSessionCreated, nil),
		}); err != nil {
			t.Fatalf("AppendEvents %d: %v", i, err)
		}
	}
	var seen []domain.SessionID
	cursor := ""
	for page := 0; page < 10; page++ {
		summaries, next, err := store.ListSessions(ctx, cursor, 2, false, domain.WorkspaceID{})
		if err != nil {
			t.Fatalf("ListSessions page %d: %v", page, err)
		}
		for _, s := range seen {
			for _, got := range summaries {
				if got.ID == s {
					t.Fatalf("page overlap: session %s listed twice", s)
				}
			}
		}
		for _, s := range summaries {
			seen = append(seen, s.ID)
		}
		if next == "" {
			break
		}
		cursor = next
	}
	if len(seen) != len(ids) {
		t.Fatalf("paginated listing covered %d sessions, want %d", len(seen), len(ids))
	}
}

// The delegation edge persisted in the child's run.created event must
// surface as ParentSessionID in listings (hierarchical pickers).
func TestSQLiteStoreListSessionsProjectsDelegationParent(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	parent := domain.NewSessionID()
	child := domain.NewSessionID()
	for _, id := range []domain.SessionID{parent, child} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
	}
	delegation := struct {
		RunID           domain.RunID      `json:"run_id"`
		Delegated       bool              `json:"delegated"`
		ParentSessionID domain.SessionID  `json:"parent_session_id"`
		ParentToolCall  domain.ToolCallID `json:"parent_tool_call_id"`
	}{RunID: domain.NewRunID(), Delegated: true, ParentSessionID: parent, ParentToolCall: domain.NewToolCallID()}
	payload, err := json.Marshal(delegation)
	if err != nil {
		t.Fatalf("marshal delegation payload: %v", err)
	}
	if err := store.AppendEvents(ctx, child, 0, []domain.Event{
		newEvent(child, 1, domain.EventRunCreated, payload),
	}); err != nil {
		t.Fatalf("AppendEvents child: %v", err)
	}
	summaries, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	byID := make(map[domain.SessionID]domain.SessionSummary, len(summaries))
	for _, s := range summaries {
		byID[s.ID] = s
	}
	if got := byID[child].ParentSessionID; got != parent {
		t.Fatalf("child ParentSessionID = %q, want %q", got, parent)
	}
	if got := byID[parent].ParentSessionID; !got.IsZero() {
		t.Fatalf("parent ParentSessionID = %q, want zero", got)
	}
}

// DeleteSession removes the session row (events/checkpoints/artifact_refs
// cascade) and the FK-less file_changes rows.
func TestSQLiteStoreDeleteSession(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	keep := domain.NewSessionID()
	doomed := domain.NewSessionID()
	for _, id := range []domain.SessionID{keep, doomed} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
		if err := store.AppendEvents(ctx, id, 0, []domain.Event{
			newEvent(id, 1, domain.EventSessionCreated, nil),
		}); err != nil {
			t.Fatalf("AppendEvents: %v", err)
		}
	}
	if err := store.DeleteSession(ctx, doomed); err != nil {
		t.Fatalf("DeleteSession: %v", err)
	}
	summaries, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	if len(summaries) != 1 || summaries[0].ID != keep {
		t.Fatalf("after delete, summaries = %+v, want only %s", summaries, keep)
	}
	if _, err := store.InspectSession(ctx, doomed); err == nil {
		t.Fatal("deleted session is still inspectable")
	}
	if err := store.DeleteSession(ctx, doomed); err == nil {
		t.Fatal("double delete must report session not found")
	}
}

// Archived sessions are hidden from the default listing and surface in the
// archived view; unarchiving restores them.
func TestSQLiteStoreArchiveSession(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	active := domain.NewSessionID()
	archived := domain.NewSessionID()
	for _, id := range []domain.SessionID{active, archived} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
	}
	if err := store.SetSessionArchived(ctx, archived, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}
	def, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions default: %v", err)
	}
	if len(def) != 1 || def[0].ID != active {
		t.Fatalf("default listing = %+v, want only the active session", def)
	}
	arch, _, err := store.ListSessions(ctx, "", 10, true, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions archived: %v", err)
	}
	if len(arch) != 1 || arch[0].ID != archived {
		t.Fatalf("archived listing = %+v, want only the archived session", arch)
	}
	if err := store.SetSessionArchived(ctx, archived, false); err != nil {
		t.Fatalf("SetSessionArchived(false): %v", err)
	}
	def, _, err = store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions after unarchive: %v", err)
	}
	if len(def) != 2 {
		t.Fatalf("default listing after unarchive = %+v, want both sessions", def)
	}
	if err := store.SetSessionArchived(ctx, domain.NewSessionID(), true); err == nil {
		t.Fatal("archiving a missing session must fail")
	}
}

func TestSQLiteStoreFirstUserMessageTexts(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	withPrompt := domain.NewSessionID()
	noPrompt := domain.NewSessionID()
	for _, id := range []domain.SessionID{withPrompt, noPrompt} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
	}
	user := domain.Message{
		ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "fix the flaky test"}},
		CreatedAt: time.Now().UTC(),
	}
	second := domain.Message{
		ID: domain.NewMessageID(), Sequence: 2, Role: domain.RoleUser,
		Status: domain.MessageStatusFinal, Revision: 1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "follow-up prompt"}},
		CreatedAt: time.Now().UTC(),
	}
	if err := store.AppendEvents(ctx, withPrompt, 0, []domain.Event{
		newEvent(withPrompt, 1, domain.EventSessionCreated, nil),
		newEvent(withPrompt, 2, domain.EventUserMessageAdded, messagePayload(t, user)),
		newEvent(withPrompt, 3, domain.EventUserMessageAdded, messagePayload(t, second)),
	}); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}

	titles, err := store.FirstUserMessageTexts(ctx, []domain.SessionID{withPrompt, noPrompt})
	if err != nil {
		t.Fatalf("FirstUserMessageTexts: %v", err)
	}
	if titles[withPrompt] != "fix the flaky test" {
		t.Fatalf("title = %q, want the FIRST user message", titles[withPrompt])
	}
	if _, ok := titles[noPrompt]; ok {
		t.Fatalf("session without user message must be absent, got %q", titles[noPrompt])
	}
}

func TestSQLiteStoreIndexesArtifactReferencesAcrossCheckpoints(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
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

// Regression (REVIEW H1): compaction replacement messages record their
// artifact dependencies in Metadata[domain.MetadataCompactedArtifacts]
// because a marker message can only carry text parts. The store must scan
// that metadata so GC never reclaims artifacts a compacted transcript
// still points at.
func TestSQLiteStoreTracksCompactionMetadataArtifactRefs(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	firstID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("4", 64))
	secondID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("5", 64))
	encoded, err := json.Marshal([]domain.ArtifactRef{{ID: firstID, Size: 40}, {ID: secondID, Size: 50}})
	if err != nil {
		t.Fatalf("marshal refs: %v", err)
	}
	checkpoint := testCheckpoint(sessionID, 0, time.Now().UTC())
	checkpoint.Messages[0].Metadata = map[string]string{domain.MetadataCompactedArtifacts: string(encoded)}
	if err := store.SaveCheckpoint(ctx, checkpoint); err != nil {
		t.Fatalf("SaveCheckpoint: %v", err)
	}
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		t.Fatalf("ListArtifactRefs: %v", err)
	}
	if len(refs) != 2 || refs[firstID] != 40 || refs[secondID] != 50 {
		t.Fatalf("unexpected artifact refs: %+v", refs)
	}
}

func TestSQLiteStoreRejectsCheckpointAheadOfSession(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	checkpoint := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.SaveCheckpoint(ctx, checkpoint); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("SaveCheckpoint error = %v, want invalid_input", err)
	}
}

// TestSQLiteStorePreservesIgnorableFlag pins the v9 persistence contract:
// the writer's informational mark survives the store round-trip — it is
// what lets an older binary skip an unknown event type safely.
func TestSQLiteStorePreservesIgnorableFlag(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	created := newEvent(sessionID, 1, domain.EventSessionCreated, nil)
	audit := newEvent(sessionID, 2, domain.EventModelRequestStarted, nil)
	audit.Ignorable = true
	ckpt := testCheckpoint(sessionID, 2, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, []domain.Event{created, audit}, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
	loaded, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if loaded[0].Ignorable {
		t.Fatal("session.created must not be ignorable")
	}
	if !loaded[1].Ignorable {
		t.Fatal("the ignorable mark did not survive the store round-trip")
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
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); errorCode(err) != domain.ErrConflict {
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

func TestSQLiteStoreRecordFileChangePersistsLedger(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "a.txt", true, "h1", []byte("old-a"), "h2"); err != nil {
		t.Fatalf("RecordFileChange: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "b.txt", false, "", nil, "h3"); err != nil {
		t.Fatalf("RecordFileChange new file: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "", true, "h1", nil, "h2"); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("empty path error = %v, want invalid_input", err)
	}
	if err := store.RecordFileChange(ctx, domain.SessionID{}, "a.txt", true, "h1", nil, "h2"); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("zero session error = %v, want invalid_input", err)
	}
}

func TestSQLiteStoreRecordFileChangeCapsOversizedContent(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	oversized := make([]byte, MaxFileChangeSnapshotBytes+1)
	for i := range oversized {
		oversized[i] = 'X'
	}
	// First checkpoint at seq 1 (before any file changes).
	events1 := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint 1: %v", err)
	}
	// Record oversized file change AFTER checkpoint 1.
	if err := store.RecordFileChange(ctx, sessionID, "big.txt", true, "h1", oversized, "h2"); err != nil {
		t.Fatalf("RecordFileChange oversized: %v", err)
	}
	// Second checkpoint at seq 2 (captures the ledger position after the file change).
	events2 := []domain.Event{newEvent(sessionID, 2, domain.EventUserMessageAdded, nil)}
	ckpt2 := testCheckpoint(sessionID, 2, time.Now().UTC().Add(time.Second))
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint 2: %v", err)
	}
	// Rewind to checkpoint 1 — the oversized file change is after it.
	result, err := store.RewindSession(ctx, sessionID, 1)
	if err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	if len(result.Changes) != 1 {
		t.Fatalf("expected 1 change, got %d", len(result.Changes))
	}
	if result.Changes[0].Restorable {
		t.Fatalf("oversized content should be unrestorable")
	}
}

func TestSQLiteStoreRewindSessionRestoresFilesAndTruncatesEvents(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store := openTestSQLiteStore(t, path)
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	// Event 1, checkpoint at seq 1 (ledger position = 0, no file changes yet)
	events1 := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("first checkpoint: %v", err)
	}

	// Record a file change AFTER checkpoint 1
	if err := store.RecordFileChange(ctx, sessionID, "hello.go", true, "hash1", []byte("package main\n"), "hash2"); err != nil {
		t.Fatalf("RecordFileChange: %v", err)
	}

	// Event 2-3, checkpoint at seq 3
	events2 := []domain.Event{
		newEvent(sessionID, 2, domain.EventUserMessageAdded, nil),
		newEvent(sessionID, 3, domain.EventModelResponseCompleted, nil),
	}
	ckpt2 := testCheckpoint(sessionID, 3, time.Now().UTC().Add(time.Second))
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("second checkpoint: %v", err)
	}

	// Rewind to checkpoint 1
	result, err := store.RewindSession(ctx, sessionID, 1)
	if err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	if result.Checkpoint.ID != ckpt1.ID {
		t.Fatalf("rewind checkpoint ID = %s, want %s", result.Checkpoint.ID, ckpt1.ID)
	}
	if len(result.Changes) != 1 || result.Changes[0].Path != "hello.go" {
		t.Fatalf("unexpected rewind changes: %+v", result.Changes)
	}
	if !result.Changes[0].BeforeExisted || string(result.Changes[0].BeforeContent) != "package main\n" {
		t.Fatalf("unexpected before content: existed=%v content=%q", result.Changes[0].BeforeExisted, result.Changes[0].BeforeContent)
	}

	// Verify events and checkpoints are truncated
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		t.Fatalf("InspectSession after rewind: %v", err)
	}
	if inspection.Session.Version != 1 {
		t.Fatalf("session version after rewind = %d, want 1", inspection.Session.Version)
	}
	if len(inspection.Events) != 1 {
		t.Fatalf("events after rewind = %d, want 1", len(inspection.Events))
	}
	if inspection.Checkpoint == nil || inspection.Checkpoint.Sequence != 1 {
		t.Fatalf("checkpoint after rewind = %+v, want seq 1", inspection.Checkpoint)
	}
}

func TestSQLiteStoreRewindSessionDeduplicatesByPath(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	// Event + checkpoint at seq 1
	events1 := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("first checkpoint: %v", err)
	}

	// Multiple edits to the same file AFTER checkpoint 1
	if err := store.RecordFileChange(ctx, sessionID, "a.go", true, "h1", []byte("v1"), "h2"); err != nil {
		t.Fatalf("RecordFileChange 1: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "a.go", true, "h2", []byte("v2"), "h3"); err != nil {
		t.Fatalf("RecordFileChange 2: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "b.go", true, "h4", []byte("v3"), "h5"); err != nil {
		t.Fatalf("RecordFileChange 3: %v", err)
	}

	// Need another checkpoint to give us a rewind target past seq 1
	events2 := []domain.Event{newEvent(sessionID, 2, domain.EventUserMessageAdded, nil)}
	ckpt2 := testCheckpoint(sessionID, 2, time.Now().UTC().Add(time.Second))
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("second checkpoint: %v", err)
	}

	result, err := store.RewindSession(ctx, sessionID, 1)
	if err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	// Should get 2 changes: a.go (earliest = v1) and b.go
	if len(result.Changes) != 2 {
		t.Fatalf("dedup changes = %d, want 2", len(result.Changes))
	}
	byPath := make(map[string]FileChange, len(result.Changes))
	for _, c := range result.Changes {
		byPath[c.Path] = c
	}
	if string(byPath["a.go"].BeforeContent) != "v1" {
		t.Fatalf("a.go before content = %q, want v1", byPath["a.go"].BeforeContent)
	}
	if string(byPath["b.go"].BeforeContent) != "v3" {
		t.Fatalf("b.go before content = %q, want v3", byPath["b.go"].BeforeContent)
	}
}

func TestSQLiteStoreRewindSessionRejectsInvalidInput(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, err := store.RewindSession(ctx, domain.SessionID{}, 1); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("zero session error = %v, want invalid_input", err)
	}
	if _, err := store.RewindSession(ctx, sessionID, 0); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("zero sequence error = %v, want invalid_input", err)
	}
	if _, err := store.RewindSession(ctx, sessionID, 1); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("missing checkpoint error = %v, want invalid_input", err)
	}
}

func TestSQLiteStoreListCheckpointsReturnsSummary(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	events := transcriptEvents(t, sessionID)
	if err := store.AppendEvents(ctx, sessionID, 0, events); err != nil {
		t.Fatalf("AppendEvents: %v", err)
	}
	base := time.Date(2026, 7, 23, 10, 0, 0, 0, time.UTC)
	first := testCheckpoint(sessionID, 1, base)
	second := testCheckpoint(sessionID, 3, base.Add(time.Second))
	if err := store.SaveCheckpoint(ctx, first); err != nil {
		t.Fatalf("SaveCheckpoint first: %v", err)
	}
	if err := store.SaveCheckpoint(ctx, second); err != nil {
		t.Fatalf("SaveCheckpoint second: %v", err)
	}
	summaries, err := store.ListCheckpoints(ctx, sessionID, 10)
	if err != nil {
		t.Fatalf("ListCheckpoints: %v", err)
	}
	if len(summaries) != 2 {
		t.Fatalf("expected 2 summaries, got %d", len(summaries))
	}
	// Most recent first
	if summaries[0].Sequence != 3 {
		t.Fatalf("first summary seq = %d, want 3", summaries[0].Sequence)
	}
	if summaries[1].Sequence != 1 {
		t.Fatalf("second summary seq = %d, want 1", summaries[1].Sequence)
	}
	// Limit validation
	if _, err := store.ListCheckpoints(ctx, sessionID, 0); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("zero limit error = %v, want invalid_input", err)
	}
	if _, err := store.ListCheckpoints(ctx, domain.SessionID{}, 10); errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("zero session error = %v, want invalid_input", err)
	}
}

// TestSQLiteStoreRewindSessionRecomputesArtifactRefs is the M29 regression
// lock: references registered by checkpoints the rewind deletes must not
// survive it — otherwise the artifacts they pin leak past the GC forever.
// References from the surviving (rewind-target) checkpoint must be kept.
func TestSQLiteStoreRewindSessionRecomputesArtifactRefs(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	keptID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("6", 64))
	droppedID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("7", 64))

	// Checkpoint at seq 1 references the kept artifact.
	ckpt1 := testCheckpoint(sessionID, 1, time.Now().UTC())
	ckpt1.Messages[0].Parts = []domain.ContentPart{
		{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: keptID, Size: 11}},
	}
	events1 := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events1, ckpt1); err != nil {
		t.Fatalf("first checkpoint: %v", err)
	}
	// Checkpoint at seq 2 references the dropped artifact.
	ckpt2 := testCheckpoint(sessionID, 2, time.Now().UTC().Add(time.Second))
	ckpt2.Messages[0].Parts = []domain.ContentPart{
		{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: droppedID, Size: 22}},
	}
	events2 := []domain.Event{newEvent(sessionID, 2, domain.EventUserMessageAdded, nil)}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 1, events2, ckpt2); err != nil {
		t.Fatalf("second checkpoint: %v", err)
	}

	if _, err := store.RewindSession(ctx, sessionID, 1); err != nil {
		t.Fatalf("RewindSession: %v", err)
	}
	refs, err := store.ListArtifactRefs(ctx)
	if err != nil {
		t.Fatalf("ListArtifactRefs: %v", err)
	}
	if _, ok := refs[droppedID]; ok {
		t.Fatalf("dropped artifact %s still referenced after rewind: %+v", droppedID, refs)
	}
	if got := refs[keptID]; got != 11 {
		t.Fatalf("kept artifact ref size = %d, want 11 (refs %+v)", got, refs)
	}
}

// TestSQLiteStoreDeleteSessionRemovesAllSessionData locks the M29
// transactional delete: one call removes every per-session row — events,
// checkpoints and artifact_refs by cascade, file_changes and memory_jobs
// explicitly.
func TestSQLiteStoreDeleteSessionRemovesAllSessionData(t *testing.T) {
	ctx := context.Background()
	store := openTestSQLiteStore(t, filepath.Join(t.TempDir(), "sessions.db"))
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	artifactID, _ := domain.ParseArtifactID("art_sha256_" + strings.Repeat("8", 64))
	ckpt := testCheckpoint(sessionID, 1, time.Now().UTC())
	ckpt.Messages[0].Parts = []domain.ContentPart{
		{Kind: domain.PartArtifact, Artifact: &domain.ArtifactRef{ID: artifactID, Size: 7}},
	}
	events := []domain.Event{newEvent(sessionID, 1, domain.EventSessionCreated, nil)}
	if err := store.AppendEventsAndCheckpoint(ctx, sessionID, 0, events, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
	if err := store.RecordFileChange(ctx, sessionID, "a.go", true, "h1", []byte("v1"), "h2"); err != nil {
		t.Fatalf("RecordFileChange: %v", err)
	}
	if err := store.EnqueueMemoryJob(ctx, sessionID, "/ws"); err != nil {
		t.Fatalf("EnqueueMemoryJob: %v", err)
	}

	if err := store.DeleteSession(ctx, sessionID); err != nil {
		t.Fatalf("DeleteSession: %v", err)
	}
	for _, table := range []string{"sessions", "events", "checkpoints", "artifact_refs", "file_changes", "memory_jobs"} {
		var n int
		if err := store.db.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM "+table+" WHERE session_id = ?", sessionID.String()).Scan(&n); err != nil {
			t.Fatalf("count %s: %v", table, err)
		}
		if n != 0 {
			t.Fatalf("%s rows after DeleteSession = %d, want 0", table, n)
		}
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
