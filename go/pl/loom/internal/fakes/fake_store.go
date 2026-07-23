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
	"fmt"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// FakeStore is an in-memory implementation of domain.SessionStore for testing.
type FakeStore struct {
	mu          sync.Mutex
	sessions    map[domain.SessionID]bool
	events      map[domain.SessionID][]domain.Event
	versions    map[domain.SessionID]int64
	checkpoints map[domain.SessionID][]domain.Checkpoint
}

// NewFakeStore creates a new FakeStore.
func NewFakeStore() *FakeStore {
	return &FakeStore{
		sessions:    make(map[domain.SessionID]bool),
		events:      make(map[domain.SessionID][]domain.Event),
		versions:    make(map[domain.SessionID]int64),
		checkpoints: make(map[domain.SessionID][]domain.Checkpoint),
	}
}

func (s *FakeStore) CreateSession(_ context.Context, id domain.SessionID) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sessions[id] {
		return fmt.Errorf("session %s already exists", id)
	}
	s.sessions[id] = true
	s.events[id] = nil
	s.versions[id] = 0
	return nil
}

func (s *FakeStore) AppendEvents(_ context.Context, id domain.SessionID, expectedVersion int64, events []domain.Event) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.sessions[id] {
		return fmt.Errorf("session %s not found", id)
	}
	if s.versions[id] != expectedVersion {
		return fmt.Errorf("version mismatch: expected %d, got %d", s.versions[id], expectedVersion)
	}

	s.events[id] = append(s.events[id], events...)
	s.versions[id] += int64(len(events))
	return nil
}

func (s *FakeStore) AppendEventsAndCheckpoint(_ context.Context, id domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.sessions[id] {
		return fmt.Errorf("session %s not found", id)
	}
	if s.versions[id] != expectedVersion {
		return fmt.Errorf("version mismatch: expected %d, got %d", s.versions[id], expectedVersion)
	}
	newVersion := expectedVersion + int64(len(events))
	if checkpoint.SessionID != id || checkpoint.Sequence != newVersion {
		return fmt.Errorf("checkpoint does not cover resulting version %d", newVersion)
	}
	s.events[id] = append(s.events[id], events...)
	s.versions[id] = newVersion
	s.checkpoints[id] = append(s.checkpoints[id], checkpoint)
	return nil
}

func (s *FakeStore) LoadEvents(_ context.Context, id domain.SessionID, after int64) ([]domain.Event, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.sessions[id] {
		return nil, fmt.Errorf("session %s not found", id)
	}

	all := s.events[id]
	if after <= 0 {
		return all, nil
	}

	// Filter events with sequence > after
	var result []domain.Event
	for _, e := range all {
		if e.Sequence > after {
			result = append(result, e)
		}
	}
	return result, nil
}

func (s *FakeStore) SaveCheckpoint(_ context.Context, ckpt domain.Checkpoint) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.sessions[ckpt.SessionID] {
		return fmt.Errorf("session %s not found", ckpt.SessionID)
	}
	s.checkpoints[ckpt.SessionID] = append(s.checkpoints[ckpt.SessionID], ckpt)
	return nil
}

func (s *FakeStore) LoadLatestCheckpoint(_ context.Context, id domain.SessionID) (domain.Checkpoint, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	ckpts := s.checkpoints[id]
	if len(ckpts) == 0 {
		return domain.Checkpoint{}, fmt.Errorf("no checkpoints for session %s", id)
	}
	return ckpts[len(ckpts)-1], nil
}

// Version returns the current version for a session (for testing).
func (s *FakeStore) Version(id domain.SessionID) (int64, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	v, ok := s.versions[id]
	return v, ok
}
