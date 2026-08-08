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
// Created: 2026/08/06

package app

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Typed sentinel errors of the workspace registry (docs/WORKSPACE_DESIGN.md
// §8.1): transport adapters map them to their own error model.
var (
	// ErrWorkspaceNotFound reports a lookup against an unknown workspace ID.
	ErrWorkspaceNotFound = errors.New("workspace not found")
	// ErrWorkspaceUnavailable reports that a registered workspace's root is
	// no longer reachable (moved/deleted) or no longer canonical-consistent.
	ErrWorkspaceUnavailable = errors.New("workspace root is unavailable")
	// ErrWorkspaceInUse rejects deleting a workspace that is still referenced
	// by the running process: the default workspace (the launch directory,
	// W5) or one with live sessions. Transports map it to 409 Conflict.
	ErrWorkspaceInUse = errors.New("workspace is in use")
)

// WorkspaceRegistry manages the per-workspace Bootstraps of a process
// (docs/WORKSPACE_DESIGN.md §6). It indexes live workspaces by ID and by
// canonical root, lazily assembles their runtimes on first use, and persists
// registration through the WorkspaceStore. P1 keeps every assembled workspace
// resident (no LRU); assembly is serialized by buildMu — it is a low-frequency
// operation, so a plain mutex replaces a single-flight dependency.
type WorkspaceRegistry struct {
	proc  *ProcessRuntime
	store domain.WorkspaceStore

	mu     sync.RWMutex
	byID   map[domain.WorkspaceID]*Bootstrap
	byRoot map[string]domain.WorkspaceID
	def    domain.WorkspaceID

	// buildMu serializes workspace assembly (path validation, skill scan,
	// rules load) so concurrent Register/Resolve of the same workspace build
	// exactly one runtime.
	buildMu sync.Mutex
}

// NewWorkspaceRegistry creates a registry on the shared ProcessRuntime. The
// process store is used for workspace persistence when it implements
// domain.WorkspaceStore (session.SQLiteStore does); otherwise an in-memory
// store backs the registry (hand-assembled test runtimes, non-SQLite stores).
func NewWorkspaceRegistry(proc *ProcessRuntime) (*WorkspaceRegistry, error) {
	if proc == nil {
		return nil, fmt.Errorf("process runtime is required")
	}
	store, ok := proc.Store.(domain.WorkspaceStore)
	if !ok {
		store = newMemWorkspaceStore()
	}
	return &WorkspaceRegistry{
		proc:   proc,
		store:  store,
		byID:   make(map[domain.WorkspaceID]*Bootstrap),
		byRoot: make(map[string]domain.WorkspaceID),
	}, nil
}

// NewSingletonWorkspaceService wraps a single hand-assembled Bootstrap as the
// default (and only pre-assembled) workspace of a SessionService. It is the
// test/legacy-assembly counterpart of NewSessionService(proc, registry, …):
// callers that already hold a *Bootstrap keep their fixture unchanged.
func NewSingletonWorkspaceService(b *Bootstrap, broker *runtimeevent.Broker, cfg SessionServiceConfig) *SessionService {
	reg := newSingletonRegistry(b)
	return NewSessionService(b.ProcessRuntime, reg, broker, cfg)
}

// newSingletonRegistry builds a registry whose default workspace is the given
// pre-assembled bootstrap. It persists workspaces through the process store
// when that store implements domain.WorkspaceStore (so Register and the
// SessionService's workspaceStore view agree), else an in-memory store.
func newSingletonRegistry(b *Bootstrap) *WorkspaceRegistry {
	store, ok := b.ProcessRuntime.Store.(domain.WorkspaceStore)
	if !ok || store == nil {
		store = newMemWorkspaceStore()
	}
	reg := &WorkspaceRegistry{
		proc:   b.ProcessRuntime,
		store:  store,
		byID:   map[domain.WorkspaceID]*Bootstrap{b.WorkspaceID: b},
		byRoot: map[string]domain.WorkspaceID{b.WorkspaceRoot: b.WorkspaceID},
		def:    b.WorkspaceID,
	}
	return reg
}

// memWorkspaceStore is an in-memory domain.WorkspaceStore for tests and
// runtimes whose session store does not persist workspaces.
type memWorkspaceStore struct {
	mu        sync.Mutex
	byID      map[domain.WorkspaceID]domain.Workspace
	byRoot    map[string]domain.WorkspaceID
	sessionWs map[domain.SessionID]domain.WorkspaceID
}

func newMemWorkspaceStore() *memWorkspaceStore {
	return &memWorkspaceStore{
		byID:      make(map[domain.WorkspaceID]domain.Workspace),
		byRoot:    make(map[string]domain.WorkspaceID),
		sessionWs: make(map[domain.SessionID]domain.WorkspaceID),
	}
}

func (m *memWorkspaceStore) UpsertWorkspace(_ context.Context, ws domain.Workspace) (domain.Workspace, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if id, ok := m.byRoot[ws.RootPath]; ok {
		return m.byID[id], nil
	}
	m.byID[ws.ID] = ws
	m.byRoot[ws.RootPath] = ws.ID
	return ws, nil
}

func (m *memWorkspaceStore) GetWorkspace(_ context.Context, id domain.WorkspaceID) (domain.Workspace, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	ws, ok := m.byID[id]
	if !ok {
		return domain.Workspace{}, ErrWorkspaceNotFound
	}
	return ws, nil
}

func (m *memWorkspaceStore) GetWorkspaceByRoot(_ context.Context, root string) (domain.Workspace, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	id, ok := m.byRoot[root]
	if !ok {
		return domain.Workspace{}, ErrWorkspaceNotFound
	}
	return m.byID[id], nil
}

func (m *memWorkspaceStore) ListWorkspaces(_ context.Context) ([]domain.Workspace, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]domain.Workspace, 0, len(m.byID))
	for _, ws := range m.byID {
		out = append(out, ws)
	}
	return out, nil
}

func (m *memWorkspaceStore) DeleteWorkspace(_ context.Context, id domain.WorkspaceID) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	ws, ok := m.byID[id]
	if !ok {
		return ErrWorkspaceNotFound
	}
	delete(m.byID, id)
	delete(m.byRoot, ws.RootPath)
	return nil
}

func (m *memWorkspaceStore) SessionWorkspace(_ context.Context, sessionID domain.SessionID) (domain.WorkspaceID, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.sessionWs[sessionID], nil
}

func (m *memWorkspaceStore) BackfillSessionWorkspaces(_ context.Context, id domain.WorkspaceID) (int64, error) {
	return 0, nil
}

func (m *memWorkspaceStore) CountSessionsPerWorkspace(_ context.Context) (map[domain.WorkspaceID]int, error) {
	return map[domain.WorkspaceID]int{}, nil
}

// canonicalizeRoot resolves root to an absolute, symlink-free directory path
// (docs/WORKSPACE_DESIGN.md W2). It must exist and be a directory.
func canonicalizeRoot(root string) (string, error) {
	if root == "" {
		return "", fmt.Errorf("workspace root is required")
	}
	abs, err := filepath.Abs(root)
	if err != nil {
		return "", fmt.Errorf("abs path: %w", err)
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("eval symlinks: %w", err)
	}
	info, err := os.Stat(resolved)
	if err != nil {
		return "", fmt.Errorf("stat root: %w", err)
	}
	if !info.IsDir() {
		return "", fmt.Errorf("root %q is not a directory", resolved)
	}
	return resolved, nil
}

// Register registers (or reuses by canonical root) a workspace and returns its
// assembled runtime, eagerly (docs/WORKSPACE_DESIGN.md §6). root must exist
// and be a directory; an empty name defaults to the root's basename.
func (r *WorkspaceRegistry) Register(ctx context.Context, root, name string) (*Bootstrap, error) {
	canonical, err := canonicalizeRoot(root)
	if err != nil {
		return nil, err
	}

	r.mu.Lock()
	if id, ok := r.byRoot[canonical]; ok {
		rt := r.byID[id]
		r.mu.Unlock()
		return rt, nil
	}
	r.mu.Unlock()

	r.buildMu.Lock()
	defer r.buildMu.Unlock()
	// Double-check after acquiring the assembly lock.
	r.mu.Lock()
	if id, ok := r.byRoot[canonical]; ok {
		rt := r.byID[id]
		r.mu.Unlock()
		return rt, nil
	}
	r.mu.Unlock()

	// Reuse a persisted entity for this root when present (process restart).
	ws, err := r.store.GetWorkspaceByRoot(ctx, canonical)
	if err != nil || ws.IsZero() {
		if name == "" {
			name = filepath.Base(canonical)
		}
		ws, err = r.store.UpsertWorkspace(ctx, domain.Workspace{
			ID:       domain.NewWorkspaceID(),
			Name:     name,
			RootPath: canonical,
		})
		if err != nil {
			return nil, fmt.Errorf("register workspace: %w", err)
		}
	}

	rt, err := NewWorkspaceBootstrap(ctx, r.proc, BootstrapConfig{
		WorkspaceRoot: ws.RootPath,
		WorkspaceID:   ws.ID,
	})
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	r.byID[ws.ID] = rt
	r.byRoot[canonical] = ws.ID
	r.mu.Unlock()
	return rt, nil
}

// Resolve returns the runtime for id, lazily assembling it on first use
// (docs/WORKSPACE_DESIGN.md §6). The workspace's root must still exist and be
// canonical-consistent, otherwise ErrWorkspaceUnavailable.
func (r *WorkspaceRegistry) Resolve(ctx context.Context, id domain.WorkspaceID) (*Bootstrap, error) {
	if id.IsZero() {
		return nil, ErrWorkspaceNotFound
	}
	r.mu.Lock()
	if rt, ok := r.byID[id]; ok {
		r.mu.Unlock()
		return rt, nil
	}
	r.mu.Unlock()

	r.buildMu.Lock()
	defer r.buildMu.Unlock()
	r.mu.Lock()
	if rt, ok := r.byID[id]; ok {
		r.mu.Unlock()
		return rt, nil
	}
	r.mu.Unlock()

	ws, err := r.store.GetWorkspace(ctx, id)
	if err != nil || ws.IsZero() {
		return nil, ErrWorkspaceNotFound
	}
	// Health check: the root must still resolve to the registered canonical
	// path (guards against the directory being moved or symlink-swapped).
	canonical, err := canonicalizeRoot(ws.RootPath)
	if err != nil || canonical != ws.RootPath {
		return nil, fmt.Errorf("%w: %s", ErrWorkspaceUnavailable, ws.RootPath)
	}
	rt, err := NewWorkspaceBootstrap(ctx, r.proc, BootstrapConfig{
		WorkspaceRoot: ws.RootPath,
		WorkspaceID:   ws.ID,
	})
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	r.byID[ws.ID] = rt
	r.byRoot[canonical] = ws.ID
	r.mu.Unlock()
	return rt, nil
}

// List returns every registered workspace, newest first.
func (r *WorkspaceRegistry) List(ctx context.Context) ([]domain.Workspace, error) {
	return r.store.ListWorkspaces(ctx)
}

// Delete removes a workspace entity: the persisted row and the in-memory
// indexes. The on-disk root directory is never touched; the workspace's
// sessions survive as read-only history (their workspace_id dangles by
// design, docs/WORKSPACE_DESIGN.md §7.1). The default workspace (W5) cannot
// be deleted — every legacy entry point falls back to it. Live-session
// occupancy is checked by the caller (SessionService owns the live handles).
//
// Delete returns the evicted runtime WITHOUT closing it: the caller
// (SessionService.DeleteWorkspace) holds its session lock across this call
// to serialize against concurrent session creation, and closing the runtime
// inside that critical section would stall it — the caller closes the
// returned runtime after releasing the lock.
func (r *WorkspaceRegistry) Delete(ctx context.Context, id domain.WorkspaceID) (*Bootstrap, error) {
	if id.IsZero() {
		return nil, ErrWorkspaceNotFound
	}
	r.mu.Lock()
	if id == r.def {
		r.mu.Unlock()
		return nil, fmt.Errorf("%w: default workspace", ErrWorkspaceInUse)
	}
	r.mu.Unlock()

	// Serialize against a concurrent lazy assembly (Resolve): the workspace
	// must not re-appear in the indexes after its row is gone.
	r.buildMu.Lock()
	defer r.buildMu.Unlock()

	ws, err := r.store.GetWorkspace(ctx, id)
	if err != nil || ws.IsZero() {
		return nil, ErrWorkspaceNotFound
	}
	if err := r.store.DeleteWorkspace(ctx, id); err != nil {
		return nil, err
	}
	r.mu.Lock()
	rt := r.byID[id]
	delete(r.byID, id)
	delete(r.byRoot, ws.RootPath)
	r.mu.Unlock()
	return rt, nil
}

// RegisterDefault registers root as the process's default workspace (the
// startup directory, docs/WORKSPACE_DESIGN.md W5) and marks it as the
// fallback for clients that do not name a workspace.
func (r *WorkspaceRegistry) RegisterDefault(ctx context.Context, root string) (*Bootstrap, error) {
	rt, err := r.Register(ctx, root, "")
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	r.def = rt.WorkspaceID
	r.mu.Unlock()
	return rt, nil
}

// Default returns the default workspace's runtime, or nil when none was
// registered.
func (r *WorkspaceRegistry) Default() *Bootstrap {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.byID[r.def]
}

// DefaultID returns the default workspace's ID (zero when unset).
func (r *WorkspaceRegistry) DefaultID() domain.WorkspaceID {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.def
}

// Get returns the live runtime for id without assembling it.
func (r *WorkspaceRegistry) Get(id domain.WorkspaceID) (*Bootstrap, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	rt, ok := r.byID[id]
	return rt, ok
}

// Bootstraps returns every assembled workspace runtime (config hot-reload
// iterates them to re-apply policy, prompt, and MCP tool changes).
func (r *WorkspaceRegistry) Bootstraps() []*Bootstrap {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]*Bootstrap, 0, len(r.byID))
	for _, rt := range r.byID {
		out = append(out, rt)
	}
	return out
}

// Close closes every assembled workspace Bootstrap (docs/WORKSPACE_DESIGN.md
// §5.4): callers then close the shared ProcessRuntime.
func (r *WorkspaceRegistry) Close() {
	r.mu.Lock()
	rts := make([]*Bootstrap, 0, len(r.byID))
	for _, rt := range r.byID {
		rts = append(rts, rt)
	}
	r.mu.Unlock()
	for _, rt := range rts {
		rt.Close()
	}
}
