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

// Package exsession implements the exec_session / write_stdin tool pair:
// long-running interactive process sessions that the model can start, poll,
// and feed input to across multiple tool calls (dev servers, REPLs,
// watch-mode runners) — the asynchronous counterpart of run_cmd.
package exsession

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// DefaultIdleTTL is how long a session may sit untouched (no start, write,
// or read) before the reaper kills it. The cap exists so a forgotten dev
// server cannot outlive the conversation that spawned it.
const DefaultIdleTTL = 30 * time.Minute

const reapInterval = 30 * time.Second

// sessionEntry tracks one live or recently-finished session. The zero
// commit fields latch the artifact refs once the process exits and the
// staged output is committed.
type sessionEntry struct {
	id      string
	session *process.Session
	argv    string
	cwd     string

	lastTouch time.Time

	stdoutStage domain.StagedArtifact
	stderrStage domain.StagedArtifact

	commitMu  sync.Mutex
	committed bool
	stdoutRef *domain.ArtifactRef
	stderrRef *domain.ArtifactRef
}

// commitArtifacts commits the staged stdout/stderr exactly once, after the
// process has exited. It is a no-op while the session is still running.
func (e *sessionEntry) commitArtifacts(ctx context.Context) error {
	e.commitMu.Lock()
	defer e.commitMu.Unlock()
	if e.committed {
		return nil
	}
	if e.session.Running() {
		return nil
	}
	e.committed = true
	if e.stdoutStage != nil {
		defer e.stdoutStage.Abort()
	}
	if e.stderrStage != nil {
		defer e.stderrStage.Abort()
	}
	commitCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	defer cancel()
	if e.stdoutStage != nil && e.stdoutStage.TotalBytes() > 0 {
		ref, err := e.stdoutStage.Commit(commitCtx)
		if err != nil {
			return fmt.Errorf("commit stdout artifact: %w", err)
		}
		e.stdoutRef = &ref
	}
	if e.stderrStage != nil && e.stderrStage.TotalBytes() > 0 {
		ref, err := e.stderrStage.Commit(commitCtx)
		if err != nil {
			return fmt.Errorf("commit stderr artifact: %w", err)
		}
		e.stderrRef = &ref
	}
	return nil
}

// Manager owns every exec session in the process. It is shared by the
// exec_session and write_stdin tools and must be Closed on shutdown to
// reclaim surviving process groups.
type Manager struct {
	runner    *process.Runner
	artifacts domain.ArtifactStore
	idleTTL   time.Duration

	mu        sync.Mutex
	sessions  map[string]*sessionEntry
	closeOnce sync.Once
	done      chan struct{}
}

// NewManager creates a session manager and starts its idle reaper. A nil
// artifact store disables output externalization (tests).
func NewManager(runner *process.Runner, artifacts domain.ArtifactStore, idleTTL time.Duration) (*Manager, error) {
	if runner == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "process runner is required")
	}
	if idleTTL <= 0 {
		idleTTL = DefaultIdleTTL
	}
	m := &Manager{
		runner:    runner,
		artifacts: artifacts,
		idleTTL:   idleTTL,
		sessions:  map[string]*sessionEntry{},
		done:      make(chan struct{}),
	}
	go m.reapLoop()
	return m, nil
}

// Start launches a new session under the granted sandbox mode and registers
// it. displayArgv/displayCwd feed approval and result rendering.
func (m *Manager) Start(ctx context.Context, spec process.CommandSpec, grant process.Grant, displayArgv, displayCwd string) (*sessionEntry, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	var stdoutStage, stderrStage domain.StagedArtifact
	if m.artifacts != nil {
		var err error
		stdoutStage, err = m.artifacts.Begin(ctx)
		if err != nil {
			return nil, domain.NewError(domain.ErrUnavailable, "begin stdout artifact", domain.WithCause(err))
		}
		stderrStage, err = m.artifacts.Begin(ctx)
		if err != nil {
			_ = stdoutStage.Abort()
			return nil, domain.NewError(domain.ErrUnavailable, "begin stderr artifact", domain.WithCause(err))
		}
	}
	spec.StdoutWriter = stdoutStage
	spec.StderrWriter = stderrStage

	session, err := m.runner.StartSessionWithGrant(spec, grant)
	if err != nil {
		if stdoutStage != nil {
			_ = stdoutStage.Abort()
		}
		if stderrStage != nil {
			_ = stderrStage.Abort()
		}
		return nil, err
	}

	entry := &sessionEntry{
		id:          newSessionID(),
		session:     session,
		argv:        displayArgv,
		cwd:         displayCwd,
		lastTouch:   time.Now(),
		stdoutStage: stdoutStage,
		stderrStage: stderrStage,
	}
	m.mu.Lock()
	m.sessions[entry.id] = entry
	m.mu.Unlock()
	return entry, nil
}

// Get returns the entry for id, refreshing its idle timer.
func (m *Manager) Get(id string) (*sessionEntry, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	entry, ok := m.sessions[id]
	if ok {
		entry.lastTouch = time.Now()
	}
	return entry, ok
}

// Close kills every remaining session and stops the reaper. Idempotent.
func (m *Manager) Close() {
	m.closeOnce.Do(func() {
		close(m.done)
		m.mu.Lock()
		entries := make([]*sessionEntry, 0, len(m.sessions))
		for _, entry := range m.sessions {
			entries = append(entries, entry)
		}
		m.sessions = map[string]*sessionEntry{}
		m.mu.Unlock()
		for _, entry := range entries {
			entry.session.Kill()
			abortStages(entry)
		}
	})
}

// reapLoop periodically kills sessions that have been idle past the TTL and
// drops their registry entries (committing staged output first, so the
// transcript keeps a durable reference to whatever the session produced).
func (m *Manager) reapLoop() {
	ticker := time.NewTicker(reapInterval)
	defer ticker.Stop()
	for {
		select {
		case <-m.done:
			return
		case now := <-ticker.C:
			m.mu.Lock()
			var expired []*sessionEntry
			for id, entry := range m.sessions {
				if now.Sub(entry.lastTouch) > m.idleTTL {
					expired = append(expired, entry)
					delete(m.sessions, id)
				}
			}
			m.mu.Unlock()
			for _, entry := range expired {
				if entry.session.Running() {
					entry.session.Kill()
				}
				_ = entry.commitArtifacts(context.Background())
				abortStages(entry)
			}
		}
	}
}

// abortStages discards uncommitted staging areas (start failures and
// artifact-less managers); Commit-then-Abort is safe by contract.
func abortStages(entry *sessionEntry) {
	entry.commitMu.Lock()
	defer entry.commitMu.Unlock()
	if entry.stdoutStage != nil {
		_ = entry.stdoutStage.Abort()
	}
	if entry.stderrStage != nil {
		_ = entry.stderrStage.Abort()
	}
}

// newSessionID returns a short random session identifier.
func newSessionID() string {
	var buf [6]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return fmt.Sprintf("sess_%d", time.Now().UnixNano())
	}
	return "sess_" + hex.EncodeToString(buf[:])
}
