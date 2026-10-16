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

// Package memory provides durable, scoped memories and asynchronously built
// views. Records are authoritative; model-written views are disposable caches.
package memory

import (
	"cmp"
	"context"
	"fmt"
	"slices"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	MaxContentBytes         = 4096
	MaxQueryBytes           = 512
	MaxQueryRecords         = 200
	ContextExplicitRecords  = 100
	ContextAutomaticRecords = 50
)

type Kind string

const (
	Explicit  Kind = "explicit"
	Automatic Kind = "automatic"
)

// An empty Scope denotes user-global memory; otherwise it is a workspace root.
// Scope is assigned by the host, never by a model-supplied filesystem path.
type Record struct {
	ID            string           `json:"id"`
	Scope         string           `json:"scope,omitempty"`
	Kind          Kind             `json:"kind"`
	Content       string           `json:"content"`
	Rollout       string           `json:"rollout,omitempty"`
	Revision      int64            `json:"revision"`
	Sequence      int64            `json:"sequence"`
	SessionID     domain.SessionID `json:"session_id,omitzero"`
	SourceVersion int64            `json:"source_version,omitzero"`
	UpdatedAt     time.Time        `json:"updated_at"`
}

// Mutation is optimistic and idempotent. RequestID is the durable tool call ID;
// an update/forget names both the target ID and the revision observed by read.
type Mutation struct {
	RequestID     string
	ID            string
	Revision      int64
	Scope         string
	Content       string
	Forget        bool
	SessionID     domain.SessionID
	SourceVersion int64
}

// View covers automatic evidence up to Version in a snapshot. Epoch changes
// when evidence is revoked or explicit constraints change, invalidating older
// views without duplicating authoritative explicit records into them.
type View struct {
	Version int64
	Epoch   int64
	Main    string
	Summary string
}

type Snapshot struct {
	Scope   string
	Version int64
	Epoch   int64
	Records []Record
	View    View
	// Omitted means active evidence beyond the view/record projection remains.
	Omitted bool
}

func (s Snapshot) ValidView() bool {
	return s.View.Version > 0 && s.View.Epoch == s.Epoch
}

// Repository owns transactions, including record mutation and view publication.
// Implementations must read each Snapshot consistently and fence PublishView
// against revoked evidence and already-published newer snapshots.
type Repository interface {
	MutateMemory(context.Context, Mutation) (Record, error)
	// MemorySnapshot is complete and reserved for background consolidation.
	MemorySnapshot(context.Context, string) (Snapshot, error)
	// MemoryContext reads all visible scopes consistently with bounded records.
	MemoryContext(context.Context, string) ([]Snapshot, error)
	ReadMemory(context.Context, string, string) (Record, error)
	QueryMemory(context.Context, string, string, int) (SearchResult, error)
	MemoryScopes(context.Context) ([]string, error)
	PublishMemoryView(context.Context, Snapshot, string, string) (bool, error)
}

// Store is a workspace-bound facade over a process-shared repository. Binding a
// workspace makes both tools and prompt injection observe identical visibility.
type Store struct {
	repo      Repository
	workspace string
}

func NewStore(repo Repository) *Store { return &Store{repo: repo} }

func (s *Store) ForWorkspace(root string) *Store {
	return &Store{repo: s.repo, workspace: root}
}

func (s *Store) Workspace() string { return s.workspace }

func (s *Store) Snapshots(ctx context.Context) ([]Snapshot, error) {
	return s.repo.MemoryContext(ctx, s.workspace)
}

func (s *Store) Mutate(ctx context.Context, m Mutation) (Record, error) {
	if m.Scope != "" && m.Scope != s.workspace {
		return Record{}, domain.NewError(domain.ErrInvalidInput, "memory scope is outside the current workspace")
	}
	if m.RequestID == "" {
		return Record{}, domain.NewError(domain.ErrInvalidInput, "memory request ID is required")
	}
	m.Content = strings.TrimSpace(RedactSecrets(m.Content))
	if !m.Forget && (m.Content == "" || len(m.Content) > MaxContentBytes || !utf8.ValidString(m.Content)) {
		return Record{}, domain.NewError(domain.ErrInvalidInput, "memory content must be valid UTF-8 and between 1 and 4096 bytes")
	}
	if m.ID != "" && m.Revision < 1 {
		return Record{}, domain.NewError(domain.ErrInvalidInput, "updating or forgetting a memory requires its current revision")
	}
	if m.Forget && m.ID == "" {
		return Record{}, domain.NewError(domain.ErrInvalidInput, "forget requires a memory ID")
	}
	return s.repo.MutateMemory(ctx, m)
}

// Records returns only active, visible evidence. New explicit records sort ahead
// of automatically extracted knowledge, independently of consolidation state.
func (s *Store) Records(ctx context.Context) ([]Record, error) {
	result, err := s.repo.QueryMemory(ctx, s.workspace, "", MaxQueryRecords)
	return result.Records, err
}

func (s *Store) Read(ctx context.Context, id string) (Record, error) {
	return s.repo.ReadMemory(ctx, s.workspace, id)
}

type SearchResult struct {
	Records   []Record `json:"records"`
	Truncated bool     `json:"truncated"`
}

func sortRecords(records []Record) {
	slices.SortFunc(records, func(a, b Record) int {
		if a.Kind != b.Kind {
			if a.Kind == Explicit {
				return -1
			}
			return 1
		}
		if order := b.UpdatedAt.Compare(a.UpdatedAt); order != 0 {
			return order
		}
		return cmp.Compare(a.ID, b.ID)
	})
}

func (s *Store) Search(ctx context.Context, query string, limit int) (SearchResult, error) {
	query = strings.TrimSpace(query)
	if query == "" || len(query) > MaxQueryBytes {
		return SearchResult{}, domain.NewError(domain.ErrInvalidInput, "query must be between 1 and 512 bytes")
	}
	if limit < 1 || limit > MaxQueryRecords {
		return SearchResult{}, domain.NewError(domain.ErrInvalidInput, "memory query limit must be between 1 and 200")
	}
	return s.repo.QueryMemory(ctx, s.workspace, query, limit)
}

func renderRecord(record Record) string {
	scope := "global"
	if record.Scope != "" {
		scope = "workspace"
	}
	return fmt.Sprintf("## %s (revision=%d, %s, %s)\n%s\n", record.ID, record.Revision, scope, record.Kind, record.Content)
}
