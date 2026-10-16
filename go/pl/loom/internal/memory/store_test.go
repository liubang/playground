// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// testRepo is an in-memory behavior fixture, not a SQLite implementation.
// It provides active snapshots, revision checks and request replay so Store and
// tool tests can exercise the facade without importing session (which imports memory).
type testRepo struct {
	mu        sync.Mutex
	snapshots map[string]Snapshot
	requests  map[string]testRequest
	nextID    int
	err       error
	seenCtx   context.Context
	mutations []Mutation
}

type testRequest struct {
	mutation Mutation
	record   Record
}

var _ Repository = (*testRepo)(nil)

func newTestStore(t *testing.T) *Store {
	t.Helper()
	return NewStore(&testRepo{snapshots: make(map[string]Snapshot), requests: make(map[string]testRequest)})
}

func (r *testRepo) MemorySnapshot(ctx context.Context, scope string) (Snapshot, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.seenCtx = ctx
	if err := ctx.Err(); err != nil {
		return Snapshot{}, err
	}
	if r.err != nil {
		return Snapshot{}, r.err
	}
	s := r.snapshots[scope]
	s.Scope = scope
	s.Records = slices.Clone(s.Records)
	return s, nil
}

// Consumer adapters preserve fixture behavior; SQLite bounds and consistency
// are tested in session rather than simulated by this in-memory fake.
func fakeMemoryContext(ctx context.Context, workspace string, load func(context.Context, string) (Snapshot, error)) ([]Snapshot, error) {
	scopes := []string{""}
	if workspace != "" {
		scopes = append(scopes, workspace)
	}
	var snapshots []Snapshot
	for _, scope := range scopes {
		snapshot, err := load(ctx, scope)
		if err != nil {
			return nil, err
		}
		snapshot.Scope = scope
		snapshots = append(snapshots, snapshot)
	}
	return snapshots, nil
}

func fakeReadMemory(ctx context.Context, workspace, id string, load func(context.Context, string) (Snapshot, error)) (Record, error) {
	snapshots, err := fakeMemoryContext(ctx, workspace, load)
	if err != nil {
		return Record{}, err
	}
	for _, snapshot := range snapshots {
		for _, record := range snapshot.Records {
			if record.ID == id {
				return record, nil
			}
		}
	}
	return Record{}, domain.NewError(domain.ErrInvalidInput, "memory not found; use action=list or action=search to find active memories")
}

func fakeQueryMemory(ctx context.Context, workspace, query string, limit int, load func(context.Context, string) (Snapshot, error)) (SearchResult, error) {
	snapshots, err := fakeMemoryContext(ctx, workspace, load)
	if err != nil {
		return SearchResult{}, err
	}
	records := []Record{}
	for _, snapshot := range snapshots {
		records = append(records, snapshot.Records...)
	}
	sortRecords(records)
	out := SearchResult{Records: []Record{}}
	for _, record := range records {
		text := strings.ToLower(record.Content + "\n" + record.Rollout)
		matches := true
		for term := range strings.FieldsSeq(strings.ToLower(query)) {
			if !strings.Contains(text, term) {
				matches = false
				break
			}
		}
		if !matches {
			continue
		}
		if len(out.Records) == limit {
			out.Truncated = true
			break
		}
		out.Records = append(out.Records, record)
	}
	return out, nil
}

func (r *testRepo) MemoryContext(ctx context.Context, workspace string) ([]Snapshot, error) {
	return fakeMemoryContext(ctx, workspace, r.MemorySnapshot)
}

func (r *testRepo) ReadMemory(ctx context.Context, workspace, id string) (Record, error) {
	return fakeReadMemory(ctx, workspace, id, r.MemorySnapshot)
}

func (r *testRepo) QueryMemory(ctx context.Context, workspace, query string, limit int) (SearchResult, error) {
	return fakeQueryMemory(ctx, workspace, query, limit, r.MemorySnapshot)
}

func (r *testRepo) MemoryScopes(ctx context.Context) ([]string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if r.err != nil {
		return nil, r.err
	}
	var scopes []string
	for scope := range r.snapshots {
		scopes = append(scopes, scope)
	}
	slices.Sort(scopes)
	return scopes, nil
}

func (r *testRepo) PublishMemoryView(ctx context.Context, s Snapshot, main, summary string) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return false, err
	}
	if r.err != nil {
		return false, r.err
	}
	current := r.snapshots[s.Scope]
	if current.Version != s.Version || current.Epoch != s.Epoch {
		return false, nil
	}
	current.View = View{Version: s.Version, Epoch: s.Epoch, Main: main, Summary: summary}
	r.snapshots[s.Scope] = current
	return true, nil
}

func (r *testRepo) MutateMemory(ctx context.Context, m Mutation) (Record, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.seenCtx = ctx
	if err := ctx.Err(); err != nil {
		return Record{}, err
	}
	if r.err != nil {
		return Record{}, r.err
	}
	r.mutations = append(r.mutations, m)
	if old, ok := r.requests[m.RequestID]; ok {
		if old.mutation != m {
			return Record{}, domain.NewError(domain.ErrConflict, "request ID reused with different mutation")
		}
		return old.record, nil
	}
	s := r.snapshots[m.Scope]
	s.Scope = m.Scope
	index := slices.IndexFunc(s.Records, func(record Record) bool { return record.ID == m.ID })
	var record Record
	if m.ID != "" {
		if index < 0 {
			return Record{}, domain.NewError(domain.ErrInvalidInput, "memory not found")
		}
		record = s.Records[index]
		if record.Revision != m.Revision {
			return Record{}, domain.NewError(domain.ErrConflict, "memory revision changed")
		}
		s.Epoch++ // Any replacement or revocation makes the old view unusable.
	} else {
		r.nextID++
		record = Record{ID: fmt.Sprintf("record-%06d", r.nextID), Scope: m.Scope, Kind: Explicit}
	}
	s.Version++
	record.Revision++
	record.Sequence = s.Version
	record.Content = m.Content
	record.SessionID, record.SourceVersion = m.SessionID, m.SourceVersion
	record.UpdatedAt = time.Unix(s.Version, 0).UTC()
	if m.Forget {
		s.Records = slices.Delete(s.Records, index, index+1)
	} else if index >= 0 {
		s.Records[index] = record
	} else {
		s.Records = append(s.Records, record)
	}
	r.snapshots[m.Scope] = s
	r.requests[m.RequestID] = testRequest{mutation: m, record: record}
	return record, nil
}

func (r *testRepo) fixture(s Snapshot) {
	r.mu.Lock()
	defer r.mu.Unlock()
	s.Records = slices.Clone(s.Records)
	r.snapshots[s.Scope] = s
}

func mustMutateMemory(t *testing.T, s *Store, m Mutation) Record {
	t.Helper()
	record, err := s.Mutate(t.Context(), m)
	if err != nil {
		t.Fatalf("Mutate(%+v): %v", m, err)
	}
	return record
}

func TestStoreWorkspaceVisibilityAndReadOrdering(t *testing.T) {
	s := newTestStore(t)
	r := s.repo.(*testRepo)
	now := time.Unix(100, 0)
	r.fixture(Snapshot{Version: 3, Records: []Record{
		{ID: "automatic", Kind: Automatic, Content: "inferred", UpdatedAt: now.Add(time.Hour)},
		{ID: "global-b", Kind: Explicit, Content: "personal", UpdatedAt: now},
		{ID: "global-a", Kind: Explicit, Content: "tie", UpdatedAt: now},
	}})
	r.fixture(Snapshot{Scope: "/project/a", Version: 1, Records: []Record{{ID: "workspace-a", Scope: "/project/a", Kind: Explicit, Content: "project A", UpdatedAt: now.Add(time.Minute)}}})
	r.fixture(Snapshot{Scope: "/project/b", Version: 1, Records: []Record{{ID: "workspace-b", Scope: "/project/b", Kind: Explicit, Content: "project B"}}})
	for _, tc := range []struct {
		workspace string
		want      []string
	}{
		{"", []string{"global-a", "global-b", "automatic"}},
		{"/project/a", []string{"workspace-a", "global-a", "global-b", "automatic"}},
		{"/project/b", []string{"global-a", "global-b", "workspace-b", "automatic"}},
	} {
		t.Run(tc.workspace, func(t *testing.T) {
			bound := s.ForWorkspace(tc.workspace)
			if bound.Workspace() != tc.workspace || s.Workspace() != "" {
				t.Fatal("ForWorkspace changed the original binding")
			}
			records, err := bound.Records(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var ids []string
			for _, record := range records {
				ids = append(ids, record.ID)
				got, err := bound.Read(t.Context(), record.ID)
				if err != nil || got != record {
					t.Fatalf("Read(%s)=%+v, %v", record.ID, got, err)
				}
			}
			if !slices.Equal(ids, tc.want) {
				t.Fatalf("IDs=%v want %v", ids, tc.want)
			}
		})
	}
	for _, id := range []string{"workspace-b", "missing"} {
		if _, err := s.ForWorkspace("/project/a").Read(t.Context(), id); err == nil || !strings.Contains(err.Error(), "action=list") {
			t.Fatalf("Read(%s) should reject invisible/missing ID with discovery guidance: %v", id, err)
		}
	}
}

func TestStoreSearchActiveEvidence(t *testing.T) {
	s := newTestStore(t)
	s.repo.(*testRepo).fixture(Snapshot{Version: 4, View: View{Version: 4, Summary: "summary-only keyword"}, Records: []Record{
		{ID: "one", Kind: Explicit, Content: "Go prefers bazel"},
		{ID: "two", Kind: Automatic, Content: "Go", Rollout: "PREFERS bazel"},
		{ID: "three", Kind: Automatic, Content: "prefers Python"},
		{ID: "four", Kind: Automatic, Content: "prefers Go"},
	}})
	got, err := s.Search(t.Context(), "  PREFERS go  ", 1)
	if err != nil || len(got.Records) != 1 || got.Records[0].ID != "one" || !got.Truncated {
		t.Fatalf("limited search=%+v err=%v", got, err)
	}
	got, err = s.Search(t.Context(), "prefers go", 3)
	if err != nil || len(got.Records) != 3 || got.Truncated {
		t.Fatalf("exact limit search=%+v err=%v", got, err)
	}
	for _, query := range []string{"summary-only", "absent"} {
		got, err := s.Search(t.Context(), query, 20)
		if err != nil || len(got.Records) != 0 || got.Truncated {
			t.Fatalf("search(%q)=%+v err=%v", query, got, err)
		}
	}
	for _, query := range []string{"", " \t ", strings.Repeat("界", MaxQueryBytes/3+1)} {
		if _, err := s.Search(t.Context(), query, 20); err == nil {
			t.Fatalf("invalid query %q accepted", query)
		}
	}
}

func TestStoreMutateValidationAndRedaction(t *testing.T) {
	s := newTestStore(t).ForWorkspace("/project/a")
	for name, m := range map[string]Mutation{
		"request required": {Content: "fact"},
		"foreign scope":    {RequestID: "bad", Scope: "/project/b", Content: "fact"},
		"empty":            {RequestID: "bad", Content: " \n "},
		"oversized bytes":  {RequestID: "bad", Content: strings.Repeat("界", MaxContentBytes/3+1)},
		"invalid UTF8":     {RequestID: "bad", Content: "\xff"},
		"update revision":  {RequestID: "bad", ID: "id", Content: "fact"},
		"forget ID":        {RequestID: "bad", Forget: true},
		"forget revision":  {RequestID: "bad", ID: "id", Forget: true},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := s.Mutate(t.Context(), m); err == nil {
				t.Fatal("invalid mutation succeeded")
			}
		})
	}
	if len(s.repo.(*testRepo).mutations) != 0 {
		t.Fatal("invalid mutation reached repository")
	}
	secret := "sk-abcdefghijklmnopqrstuv"
	record := mustMutateMemory(t, s, Mutation{RequestID: "redacted", Content: "  preference " + secret + "  "})
	if record.Content != "preference "+redactPlaceholder {
		t.Fatalf("stored content=%q", record.Content)
	}
	boundary := strings.Repeat("界", MaxContentBytes/3) + "x"
	if len(boundary) != MaxContentBytes {
		t.Fatal("invalid boundary fixture")
	}
	mustMutateMemory(t, s, Mutation{RequestID: "boundary", Scope: s.Workspace(), Content: boundary})
}

func TestStoreContextAndRepositoryErrorsPropagate(t *testing.T) {
	s := newTestStore(t)
	r := s.repo.(*testRepo)
	type key struct{}
	ctx := context.WithValue(t.Context(), key{}, "trace")
	failure := errors.New("snapshot unavailable")
	r.err = failure
	if _, err := s.Snapshots(ctx); !errors.Is(err, failure) {
		t.Fatalf("Snapshots error=%v", err)
	}
	if r.seenCtx != ctx {
		t.Fatal("snapshot context not forwarded")
	}
	if _, err := s.Records(ctx); !errors.Is(err, failure) {
		t.Fatalf("Records error=%v", err)
	}
	if _, err := s.Read(ctx, "id"); !errors.Is(err, failure) {
		t.Fatalf("Read swallowed repository error: %v", err)
	}
	if _, err := s.Search(ctx, "fact", 1); !errors.Is(err, failure) {
		t.Fatalf("Search swallowed repository error: %v", err)
	}
	if _, err := s.Mutate(ctx, Mutation{RequestID: "call", Content: "fact"}); !errors.Is(err, failure) || r.seenCtx != ctx {
		t.Fatalf("Mutate error/context=%v/%v", err, r.seenCtx)
	}
	r.err = nil
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if _, err := s.Snapshots(cancelled); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled Snapshots=%v", err)
	}
	if _, err := s.Records(cancelled); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled Records=%v", err)
	}
	if _, err := s.Mutate(cancelled, Mutation{RequestID: "cancel", Content: "fact"}); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled Mutate=%v", err)
	}
}

func TestStoreSnapshotsExposeStoredViewsAndActiveRecords(t *testing.T) {
	s := newTestStore(t)
	snapshots, err := s.Snapshots(t.Context())
	if err != nil || len(snapshots) != 1 || snapshots[0].ValidView() || len(snapshots[0].Records) != 0 {
		t.Fatalf("empty snapshots=%+v err=%v", snapshots, err)
	}
	view := View{Version: 2, Epoch: 1, Main: "consolidated automatic handbook", Summary: "stored automatic summary"}
	s.repo.(*testRepo).fixture(Snapshot{Version: 2, Epoch: 1, View: view, Records: []Record{
		{ID: "manual", Kind: Explicit, Revision: 1, Content: "explicit fact"},
		{ID: "auto", Kind: Automatic, Revision: 2, Sequence: 2, Content: "automatic fact", Rollout: "rollout evidence"},
	}})
	snapshots, err = s.Snapshots(t.Context())
	if err != nil || len(snapshots) != 1 || !snapshots[0].ValidView() || snapshots[0].View != view {
		t.Fatalf("stored view not preserved: snapshots=%+v err=%v", snapshots, err)
	}
	records, err := s.Records(t.Context())
	if err != nil || len(records) != 2 || records[0].Content != "explicit fact" || records[1].Content != "automatic fact" || records[1].Rollout != "rollout evidence" {
		t.Fatalf("active records=%+v err=%v", records, err)
	}
	// Snapshot consumers must distinguish a stored cache from valid evidence.
	stale := snapshots[0]
	stale.Epoch++
	s.repo.(*testRepo).fixture(stale)
	snapshots, err = s.Snapshots(t.Context())
	if err != nil || snapshots[0].ValidView() || snapshots[0].View != view {
		t.Fatalf("stale view metadata lost: snapshots=%+v err=%v", snapshots, err)
	}
}
