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
// Created: 2026/07/22 21:10

package fakes

import (
	"context"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestFakeStoreCreateAndAppend(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	if err := store.CreateSession(ctx, sid); err != nil {
		t.Fatalf("CreateSession error: %v", err)
	}

	evt := domain.Event{
		ID:        domain.NewEventID(),
		Sequence:  1,
		SessionID: sid,
		Type:      domain.EventSessionCreated,
		Timestamp: time.Now(),
	}

	if err := store.AppendEvents(ctx, sid, 0, []domain.Event{evt}); err != nil {
		t.Fatalf("AppendEvents error: %v", err)
	}

	v, ok := store.Version(sid)
	if !ok || v != 1 {
		t.Fatalf("expected version 1, got %d, ok=%v", v, ok)
	}
}

func TestFakeStoreVersionMismatch(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	_ = store.CreateSession(ctx, sid)

	err := store.AppendEvents(ctx, sid, 999, nil)
	if err == nil {
		t.Fatal("expected version mismatch error")
	}
}

func TestFakeStoreSessionNotFound(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()

	err := store.AppendEvents(ctx, domain.NewSessionID(), 0, nil)
	if err == nil {
		t.Fatal("expected session not found error")
	}
}

func TestFakeStoreDuplicateSession(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	_ = store.CreateSession(ctx, sid)
	err := store.CreateSession(ctx, sid)
	if err == nil {
		t.Fatal("expected duplicate session error")
	}
}

func TestFakeStoreLoadEvents(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	_ = store.CreateSession(ctx, sid)

	evts := []domain.Event{
		{ID: domain.NewEventID(), Sequence: 1, SessionID: sid, Type: domain.EventSessionCreated, Timestamp: time.Now()},
		{ID: domain.NewEventID(), Sequence: 2, SessionID: sid, Type: domain.EventRunCreated, Timestamp: time.Now()},
	}
	_ = store.AppendEvents(ctx, sid, 0, evts)

	loaded, err := store.LoadEvents(ctx, sid, 0)
	if err != nil {
		t.Fatalf("LoadEvents error: %v", err)
	}
	if len(loaded) != 2 {
		t.Fatalf("expected 2 events, got %d", len(loaded))
	}
}

func TestFakeStoreLoadEventsAfterSequence(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	_ = store.CreateSession(ctx, sid)

	evts := []domain.Event{
		{ID: domain.NewEventID(), Sequence: 1, SessionID: sid, Type: domain.EventSessionCreated, Timestamp: time.Now()},
		{ID: domain.NewEventID(), Sequence: 2, SessionID: sid, Type: domain.EventRunCreated, Timestamp: time.Now()},
	}
	_ = store.AppendEvents(ctx, sid, 0, evts)

	loaded, err := store.LoadEvents(ctx, sid, 1)
	if err != nil {
		t.Fatalf("LoadEvents error: %v", err)
	}
	if len(loaded) != 1 {
		t.Fatalf("expected 1 event after seq 1, got %d", len(loaded))
	}
}

func TestFakeStoreCheckpoint(t *testing.T) {
	store := NewFakeStore()
	ctx := context.Background()
	sid := domain.NewSessionID()

	_ = store.CreateSession(ctx, sid)

	ckpt := domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: sid,
		Sequence:  1,
		State:     domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		CreatedAt: time.Now(),
	}
	if err := store.SaveCheckpoint(ctx, ckpt); err != nil {
		t.Fatalf("SaveCheckpoint error: %v", err)
	}

	loaded, err := store.LoadLatestCheckpoint(ctx, sid)
	if err != nil {
		t.Fatalf("LoadLatestCheckpoint error: %v", err)
	}
	if loaded.ID != ckpt.ID {
		t.Fatalf("expected checkpoint ID %s, got %s", ckpt.ID, loaded.ID)
	}
}
