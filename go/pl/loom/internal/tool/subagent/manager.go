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
// Created: 2026/08/02

package subagent

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// DefaultMaxConcurrent bounds simultaneously running sub-agents. Each
// sub-agent is a full model loop; unbounded fan-out amplifies provider
// pressure (docs/SUBAGENT_DESIGN.md §11: the same reasoning as the
// parallel tool execution cap).
const DefaultMaxConcurrent = 4

// RunStatus is the observable lifecycle of a managed sub-agent.
type RunStatus string

const (
	StatusRunning RunStatus = "running"
	StatusDone    RunStatus = "done"
)

// SpawnSpec describes one asynchronous delegation.
type SpawnSpec struct {
	Task          string
	Focus         []string
	Role          Role
	ParentSession domain.SessionID
	ParentCall    domain.ToolCallID
}

// WaitResult is the terminal projection of a sub-agent run.
type WaitResult struct {
	SessionID  domain.SessionID
	Role       Role
	Outcome    domain.Outcome
	Usage      domain.Usage
	Conclusion string
	// ExecErr is the loop-level failure (provider outage, persistence
	// error) — distinct from a run that merely ended on a budget or
	// runaway outcome.
	ExecErr error
}

// managedRun tracks one in-flight (or finished-but-not-yet-collected)
// child run inside the manager. A finished run is COLLECTED — removed
// from the registry — by the first Wait that observes its done channel,
// or by a Resume that replaces it; this bounds the registry's growth and
// keeps Status honest.
type managedRun struct {
	sessionID     domain.SessionID
	parentSession domain.SessionID
	task          string
	role          Role
	startedAt     time.Time
	done          chan struct{}
	result        WaitResult
}

// Manager runs delegated sub-agent loops asynchronously (the V2 model):
// spawn returns a child session reference immediately, the child loop
// drives itself on its own goroutine, and wait blocks for the terminal
// state — possibly across parent turns or even process restarts, since
// every child is a fully persisted event-sourced session.
//
// Concurrency contract: child goroutines are the ONLY writers of the
// managedRun result; readers synchronize on the done channel. The
// manager's mutex guards the registry map itself.
type Manager struct {
	factory *Factory
	roles   map[Role]*RoleSpec
	files   agent.FileStateReader

	rootCtx context.Context
	cancel  context.CancelFunc

	mu      sync.Mutex
	running map[domain.SessionID]*managedRun
	slots   chan struct{}
	closed  bool
	wg      sync.WaitGroup
}

// NewManager creates the asynchronous delegation runtime. factory carries
// the shared child infrastructure (store, models, limits); roles maps
// each supported role to its registry/prompt pair and must contain at
// least RoleResearcher. files restores interrupted child runs on resume.
func NewManager(factory *Factory, roles map[Role]*RoleSpec, files agent.FileStateReader) (*Manager, error) {
	if factory == nil || factory.Store == nil || factory.Models == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "sub-agent manager requires a factory with a store and a model source")
	}
	spec, ok := roles[RoleResearcher]
	if !ok || spec == nil || spec.Registry == nil || spec.Prompt == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "sub-agent manager requires a researcher role spec")
	}
	for role, rs := range roles {
		if rs == nil || rs.Registry == nil || rs.Prompt == nil {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("sub-agent role %q requires a registry and a prompt builder", role))
		}
		if rs.Risk != riskOf(role) {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("sub-agent role %q risk %d does not match the expected tier %d", role, rs.Risk, riskOf(role)))
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	return &Manager{
		factory: factory,
		roles:   roles,
		files:   files,
		rootCtx: ctx,
		cancel:  cancel,
		running: make(map[domain.SessionID]*managedRun),
		slots:   make(chan struct{}, DefaultMaxConcurrent),
	}, nil
}

// HasRole reports whether the manager can spawn the given role.
func (m *Manager) HasRole(role Role) bool {
	_, ok := m.roles[role]
	return ok
}

// RoleRegistry returns the tool registry for the given role, or nil if
// the role is not configured on this manager.
func (m *Manager) RoleRegistry(role Role) *agent.ToolRegistry {
	if spec, ok := m.roles[role]; ok {
		return spec.Registry
	}
	return nil
}

// RolePrompt returns the prompt builder for the given role, or nil if
// the role is not configured on this manager.
func (m *Manager) RolePrompt(role Role) agent.PromptBuilder {
	if spec, ok := m.roles[role]; ok {
		return spec.Prompt
	}
	return nil
}

// Spawn creates a child session and starts its loop on a background
// goroutine, returning the child session reference immediately. The
// child's context derives from the manager's root — NOT from the spawn
// tool call's context, which dies when the tool returns — so the child
// survives the parent turn; Shutdown cancels everything at once.
func (m *Manager) Spawn(spec SpawnSpec) (domain.SessionID, error) {
	snap, ok := m.factory.Models.Get()
	if !ok || snap.Model == nil {
		return domain.SessionID{}, domain.NewError(domain.ErrInvalidInput,
			"sub-agent is unavailable: no model selection is active for this turn")
	}
	roleSpec, ok := m.roles[spec.Role]
	if !ok {
		return domain.SessionID{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("sub-agent role %q is not available", spec.Role))
	}
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return domain.SessionID{}, domain.NewError(domain.ErrUnavailable, "sub-agent manager is shut down")
	}
	select {
	case m.slots <- struct{}{}:
	default:
		m.mu.Unlock()
		return domain.SessionID{}, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("sub-agent concurrency limit reached (%d); wait for a running agent to finish", DefaultMaxConcurrent))
	}
	m.mu.Unlock()

	childSessionID := domain.NewSessionID()
	if err := m.factory.Store.CreateSession(m.rootCtx, childSessionID, m.factory.WorkspaceID); err != nil {
		<-m.slots
		return domain.SessionID{}, domain.NewError(domain.ErrInternal,
			"failed to create sub-agent session", domain.WithCause(err))
	}

	mr := &managedRun{
		sessionID:     childSessionID,
		parentSession: spec.ParentSession,
		task:          spec.Task,
		role:          spec.Role,
		startedAt:     time.Now(),
		done:          make(chan struct{}),
	}
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		<-m.slots
		return domain.SessionID{}, domain.NewError(domain.ErrUnavailable, "sub-agent manager is shut down")
	}
	m.running[childSessionID] = mr
	m.wg.Add(1)
	m.mu.Unlock()

	if obs := m.factory.Observer; obs != nil && obs.Started != nil {
		obs.Started(ChildStart{
			CallID:        spec.ParentCall,
			SessionID:     childSessionID,
			ParentSession: spec.ParentSession,
			Task:          spec.Task,
			StartedAt:     mr.startedAt,
		})
	}

	clock := domain.RealClock{}
	run := agent.NewRun(childSessionID, m.factory.Limits, clock)
	run.RecordDelegation(spec.ParentSession, spec.ParentCall, string(spec.Role))
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: childTaskPrompt(delegateArgs{Task: spec.Task, Focus: spec.Focus})}},
		CreatedAt: clock.Now(),
	})

	childCtx, cancel := context.WithCancel(m.rootCtx)
	go m.drive(childCtx, cancel, mr, run, snap, roleSpec, spec.ParentCall)
	return childSessionID, nil
}

// drive is the child goroutine: run the loop to a terminal state, record
// the outcome, notify waiters, release the concurrency slot.
func (m *Manager) drive(ctx context.Context, cancel context.CancelFunc, mr *managedRun, run *agent.Run, snap ModelSnapshot, roleSpec *RoleSpec, callID domain.ToolCallID) {
	defer cancel()
	defer m.wg.Done()
	defer func() { <-m.slots }()
	defer close(mr.done)

	loop := &agent.Loop{
		Run:       run,
		Model:     snap.Model,
		ModelName: snap.ModelName,
		Store:     m.factory.Store,
		// The child policy never escalates to Ask, so this approver is
		// pure defense-in-depth against a policy regression.
		Approver:     denyApprover{},
		Policy:       childPolicyFor(roleSpec.Registry),
		Registry:     roleSpec.Registry,
		Logger:       m.logger(),
		SystemPrompt: roleSpec.Prompt,
		Artifacts:    m.factory.Artifacts,
		Recorder:     m.factory.Recorder,
		Prompt:       mr.task,
		Workspace:    m.factory.Workspace,
		Window:       snap.Window,
		Runaway:      m.factory.Runaway,
		Reasoning:    snap.Reasoning,
		// No GoalCell/PlanCell/SteerCell: the child is single-purpose.
		CostInputUSDPerMTok:  m.factory.CostInputUSDPerMTok,
		CostOutputUSDPerMTok: m.factory.CostOutputUSDPerMTok,
		// The delegation edge binds this child's model calls to their own
		// record/replay fixture shard.
		ParentToolCallID: callID,
	}

	execErr := loop.Execute(ctx)
	mr.result = WaitResult{
		SessionID:  mr.sessionID,
		Role:       mr.role,
		Outcome:    run.State.Outcome,
		Usage:      run.Usage,
		Conclusion: agent.LastAssistantText(run.Messages),
		ExecErr:    execErr,
	}
	if obs := m.factory.Observer; obs != nil && obs.Finished != nil {
		obs.Finished(ChildFinish{
			CallID:        callID,
			SessionID:     mr.sessionID,
			ParentSession: mr.parentSession,
			Outcome:       run.State.Outcome,
			Usage:         run.Usage,
		})
	}
}

// Wait blocks until the named sub-agent reaches a terminal state and
// returns its conclusion. A zero timeout waits indefinitely (bounded by
// ctx); a positive timeout returns ErrTimeout with the agent still
// running — the caller may wait again later. Waiting on an unknown
// agent falls back to the persisted session state so a wait issued
// after a process restart (or against a finished-and-collected agent)
// still resolves from the durable record.
func (m *Manager) Wait(ctx context.Context, sessionID domain.SessionID, timeout time.Duration) (WaitResult, error) {
	m.mu.Lock()
	mr, ok := m.running[sessionID]
	m.mu.Unlock()
	if ok {
		if timeout <= 0 {
			select {
			case <-mr.done:
				m.collect(sessionID, mr)
				return mr.result, nil
			case <-ctx.Done():
				return WaitResult{}, ctx.Err()
			}
		}
		timer := time.NewTimer(timeout)
		defer timer.Stop()
		select {
		case <-mr.done:
			m.collect(sessionID, mr)
			return mr.result, nil
		case <-timer.C:
			return WaitResult{}, domain.NewError(domain.ErrTimeout,
				fmt.Sprintf("sub-agent %s is still running after %s; wait again with a longer timeout", sessionID, timeout))
		case <-ctx.Done():
			return WaitResult{}, ctx.Err()
		}
	}
	// Not in memory: resolve from the persisted session (finished before
	// this process started, or the manager was recreated).
	return m.loadPersistedResult(ctx, sessionID)
}

// collect removes a finished run from the registry — but only when the
// registry still holds THIS run: a concurrent Resume may already have
// replaced the entry with a fresh managedRun, which must not be dropped.
// After collection, later Waits resolve from the persisted terminal
// checkpoint, which is always durable by the time done closes.
func (m *Manager) collect(sessionID domain.SessionID, mr *managedRun) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if cur, ok := m.running[sessionID]; ok && cur == mr {
		delete(m.running, sessionID)
	}
}

// loadPersistedResult projects a terminal child session from the store.
// A session that never reached a terminal checkpoint was interrupted
// (process crash); report it as resumable rather than fabricating a
// conclusion.
func (m *Manager) loadPersistedResult(ctx context.Context, sessionID domain.SessionID) (WaitResult, error) {
	inspection, err := m.factory.Store.InspectSession(ctx, sessionID)
	if err != nil {
		return WaitResult{}, err
	}
	ckpt := inspection.Checkpoint
	if ckpt == nil || ckpt.State.Lifecycle != domain.LifecycleTerminal {
		return WaitResult{}, domain.NewError(domain.ErrConflict,
			fmt.Sprintf("sub-agent %s has no terminal state in this process; resume it to continue", sessionID))
	}
	return WaitResult{
		SessionID:  sessionID,
		Role:       roleOf(inspection.Events),
		Outcome:    ckpt.State.Outcome,
		Usage:      ckpt.Usage,
		Conclusion: agent.LastAssistantText(inspection.Transcript.Messages),
	}, nil
}

// Resume continues a terminal (or crash-interrupted) child session with
// a new task: the run is recovered from its checkpoint, the follow-up
// becomes a fresh user message, and the loop restarts asynchronously.
// The same concurrency slot discipline applies as for Spawn.
func (m *Manager) Resume(spec SpawnSpec, sessionID domain.SessionID) error {
	snap, ok := m.factory.Models.Get()
	if !ok || snap.Model == nil {
		return domain.NewError(domain.ErrInvalidInput,
			"sub-agent is unavailable: no model selection is active for this turn")
	}
	role := spec.Role
	if role == "" {
		role = RoleResearcher
	}
	roleSpec, ok := m.roles[role]
	if !ok {
		return domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("sub-agent role %q is not available", role))
	}

	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return domain.NewError(domain.ErrUnavailable, "sub-agent manager is shut down")
	}
	if mr, running := m.running[sessionID]; running {
		select {
		case <-mr.done:
			// Finished but not yet collected: the entry is stale, so the
			// resume may replace it. A genuinely in-flight run rejects.
			delete(m.running, sessionID)
		default:
			m.mu.Unlock()
			return domain.NewError(domain.ErrConflict,
				fmt.Sprintf("sub-agent %s is still running; wait for it before resuming", sessionID))
		}
	}
	select {
	case m.slots <- struct{}{}:
	default:
		m.mu.Unlock()
		return domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("sub-agent concurrency limit reached (%d); wait for a running agent to finish", DefaultMaxConcurrent))
	}
	m.mu.Unlock()

	inspection, err := m.factory.Store.InspectSession(m.rootCtx, sessionID)
	if err != nil {
		<-m.slots
		return err
	}
	if ckpt := inspection.Checkpoint; ckpt == nil || ckpt.State.Lifecycle != domain.LifecycleTerminal {
		<-m.slots
		return domain.NewError(domain.ErrConflict,
			fmt.Sprintf("sub-agent %s is not in a terminal state; only finished agents can be resumed", sessionID))
	}
	// The resume keeps the role the session was spawned with unless the
	// caller explicitly overrides it; the persisted delegation edge is
	// the source of truth across process restarts.
	if spec.Role == "" {
		if persisted := roleOf(inspection.Events); persisted != "" {
			role = persisted
			roleSpec = m.roles[role]
			if roleSpec == nil {
				<-m.slots
				return domain.NewError(domain.ErrInvalidInput,
					fmt.Sprintf("sub-agent %s was spawned with role %q which is not available", sessionID, role))
			}
		}
	}

	clock := domain.RealClock{}
	run, err := agent.RecoverRun(sessionID, inspection.Checkpoint, inspection.Transcript.Messages,
		inspection.Events, inspection.Session.Version, m.factory.Limits, clock, m.files)
	if err != nil {
		<-m.slots
		return domain.NewError(domain.ErrInternal,
			fmt.Sprintf("failed to recover sub-agent %s", sessionID), domain.WithCause(err))
	}
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: childTaskPrompt(delegateArgs{Task: spec.Task, Focus: spec.Focus})}},
		CreatedAt: clock.Now(),
	})

	mr := &managedRun{
		sessionID:     sessionID,
		parentSession: spec.ParentSession,
		task:          spec.Task,
		role:          role,
		startedAt:     time.Now(),
		done:          make(chan struct{}),
	}
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		<-m.slots
		return domain.NewError(domain.ErrUnavailable, "sub-agent manager is shut down")
	}
	m.running[sessionID] = mr
	m.wg.Add(1)
	m.mu.Unlock()

	if obs := m.factory.Observer; obs != nil && obs.Started != nil {
		obs.Started(ChildStart{
			CallID:        spec.ParentCall,
			SessionID:     sessionID,
			ParentSession: spec.ParentSession,
			Task:          spec.Task,
			StartedAt:     mr.startedAt,
		})
	}

	childCtx, cancel := context.WithCancel(m.rootCtx)
	go m.drive(childCtx, cancel, mr, run, snap, roleSpec, spec.ParentCall)
	return nil
}

// Status reports whether the named sub-agent is currently running in
// this process. A finished-but-not-yet-collected run reports done: the
// registry entry existing only means the result has not been picked up.
func (m *Manager) Status(sessionID domain.SessionID) RunStatus {
	m.mu.Lock()
	mr, ok := m.running[sessionID]
	m.mu.Unlock()
	if !ok {
		return StatusDone
	}
	select {
	case <-mr.done:
		return StatusDone
	default:
		return StatusRunning
	}
}

// Shutdown cancels every in-flight sub-agent and waits (bounded by ctx)
// for their loops to land in a terminal persisted state. After Shutdown
// returns, Spawn and Resume fail with ErrUnavailable.
func (m *Manager) Shutdown(ctx context.Context) {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	m.mu.Unlock()
	m.cancel()
	done := make(chan struct{})
	go func() {
		m.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-ctx.Done():
	}
}

func (m *Manager) logger() *slog.Logger {
	if m.factory.Logger != nil {
		return m.factory.Logger
	}
	return slog.Default()
}
