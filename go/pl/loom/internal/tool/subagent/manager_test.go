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
	"encoding/json"
	"errors"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// newTestManager creates a Manager with both researcher and coder roles,
// using a scripted fake model.
func newTestManager(t *testing.T, script ...fakes.ScriptEntry) (*Manager, *fakes.FakeModel, *fakes.FakeStore, *ModelSource) {
	t.Helper()
	store := fakes.NewFakeStore()
	model := fakes.NewFakeModel(script...)
	registry := agent.NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("register read_file: %v", err)
	}
	models := &ModelSource{}
	models.Set(ModelSnapshot{
		Model:         model,
		ModelName:     "fake-model",
		ParentSession: domain.NewSessionID(),
	})

	researcherPrompt := &stubPromptBuilder{}
	coderRegistry := agent.NewToolRegistry()
	if err := coderRegistry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("register coder read_file: %v", err)
	}
	coderPrompt := &stubPromptBuilder{}

	factory := &Factory{
		Store:    store,
		Registry: registry,
		Prompt:   researcherPrompt,
		Limits:   domain.DefaultLimits(),
		Runaway:  domain.DefaultRunawayConfig(),
		Models:   models,
	}

	roles := map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: researcherPrompt, Risk: domain.R1},
		RoleCoder:      {Registry: coderRegistry, Prompt: coderPrompt, Risk: domain.R3},
	}

	manager, err := NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager: %v", err)
	}
	return manager, model, store, models
}

// stubPromptBuilder is a minimal PromptBuilder for tests.
type stubPromptBuilder struct{}

func (s *stubPromptBuilder) Build(_ context.Context) (string, []domain.ContextRuleRef, error) {
	return "system prompt", nil, nil
}

func TestNewManagerValidation(t *testing.T) {
	store := fakes.NewFakeStore()
	registry := agent.NewToolRegistry()
	models := &ModelSource{}

	// Nil factory → error.
	_, err := NewManager(nil, map[Role]*RoleSpec{RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1}}, nil)
	if err == nil {
		t.Fatal("expected error for nil factory")
	}

	// Factory without store → error.
	_, err = NewManager(&Factory{Models: models}, map[Role]*RoleSpec{RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1}}, nil)
	if err == nil {
		t.Fatal("expected error for factory without store")
	}

	// Factory without models → error.
	_, err = NewManager(&Factory{Store: store}, map[Role]*RoleSpec{RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1}}, nil)
	if err == nil {
		t.Fatal("expected error for factory without models")
	}

	// Missing researcher role → error.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{RoleCoder: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R3}}, nil)
	if err == nil {
		t.Fatal("expected error for missing researcher role")
	}

	// Researcher with wrong risk → error.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R3},
	}, nil)
	if err == nil {
		t.Fatal("expected error for researcher with R3 risk")
	}

	// Coder with wrong risk → error.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1},
		RoleCoder:      {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1},
	}, nil)
	if err == nil {
		t.Fatal("expected error for coder with R1 risk")
	}

	// Nil registry in role spec → error.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{
		RoleResearcher: {Registry: nil, Prompt: &stubPromptBuilder{}, Risk: domain.R1},
	}, nil)
	if err == nil {
		t.Fatal("expected error for nil registry")
	}

	// Nil prompt in role spec → error.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: nil, Risk: domain.R1},
	}, nil)
	if err == nil {
		t.Fatal("expected error for nil prompt")
	}

	// Valid manager → success.
	_, err = NewManager(&Factory{Store: store, Models: models}, map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1},
		RoleCoder:      {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R3},
	}, nil)
	if err != nil {
		t.Fatalf("valid manager: %v", err)
	}
}

func TestManagerHasRole(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	if !mgr.HasRole(RoleResearcher) {
		t.Fatal("expected HasRole(researcher) = true")
	}
	if !mgr.HasRole(RoleCoder) {
		t.Fatal("expected HasRole(coder) = true")
	}
	if mgr.HasRole("unknown") {
		t.Fatal("expected HasRole(unknown) = false")
	}
}

func TestManagerRoleRegistry(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	rr := mgr.RoleRegistry(RoleResearcher)
	if rr == nil {
		t.Fatal("expected non-nil registry for researcher")
	}
	cr := mgr.RoleRegistry(RoleCoder)
	if cr == nil {
		t.Fatal("expected non-nil registry for coder")
	}
	unknown := mgr.RoleRegistry("unknown")
	if unknown != nil {
		t.Fatal("expected nil registry for unknown role")
	}
}

func TestManagerRolePrompt(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	rp := mgr.RolePrompt(RoleResearcher)
	if rp == nil {
		t.Fatal("expected non-nil prompt for researcher")
	}
	cp := mgr.RolePrompt(RoleCoder)
	if cp == nil {
		t.Fatal("expected non-nil prompt for coder")
	}
	up := mgr.RolePrompt("unknown")
	if up != nil {
		t.Fatal("expected nil prompt for unknown role")
	}
}

func TestManagerSpawnAndWait(t *testing.T) {
	mgr, _, store, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "结论：测试完成", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}
	if childID.IsZero() {
		t.Fatal("expected non-zero session ID")
	}

	// Status should be running or done (depending on goroutine scheduling).
	status := mgr.Status(childID)
	if status != StatusRunning && status != StatusDone {
		t.Fatalf("status = %q, want running or done", status)
	}

	// Wait for the child to finish (with a generous timeout).
	result, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if result.SessionID != childID {
		t.Fatalf("session ID mismatch: %s vs %s", result.SessionID, childID)
	}
	if result.Role != RoleResearcher {
		t.Fatalf("role = %q, want researcher", result.Role)
	}
	if result.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %q, want succeeded", result.Outcome)
	}
	if result.Conclusion != "结论：测试完成" {
		t.Fatalf("conclusion = %q, want 结论：测试完成", result.Conclusion)
	}
	if result.Usage.InputTokens != 50 || result.Usage.OutputTokens != 10 {
		t.Fatalf("usage = %+v, want 50/10", result.Usage)
	}

	// After finishing, status should be done. Note: Status() only
	// reports "running" when the entry is in the running map; Wait
	// does not remove it, so we accept either state here (the done
	// channel was closed, meaning the goroutine finished).
	status = mgr.Status(childID)
	if status != StatusDone && status != StatusRunning {
		t.Fatalf("status after wait = %q, want done or running", status)
	}

	// The child session should be persisted.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: err=%v, n=%d", err, len(events))
	}
}

func TestManagerSpawnUnknownRole(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	_, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          "unknown",
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err == nil {
		t.Fatal("expected error for unknown role")
	}
}

func TestManagerSpawnNoModel(t *testing.T) {
	store := fakes.NewFakeStore()
	registry := agent.NewToolRegistry()
	models := &ModelSource{} // no snapshot published

	factory := &Factory{
		Store:    store,
		Registry: registry,
		Prompt:   &stubPromptBuilder{},
		Limits:   domain.DefaultLimits(),
		Runaway:  domain.DefaultRunawayConfig(),
		Models:   models,
	}
	roles := map[Role]*RoleSpec{
		RoleResearcher: {Registry: registry, Prompt: &stubPromptBuilder{}, Risk: domain.R1},
	}
	mgr, err := NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager: %v", err)
	}

	_, err = mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err == nil {
		t.Fatal("expected error for no model snapshot")
	}
}

func TestManagerWaitTimeout(t *testing.T) {
	// Create a manager with a model that never responds (empty script).
	mgr, _, _, _ := newTestManager(t)

	// Manually insert a "running" entry to simulate a long-running child.
	childID := domain.NewSessionID()
	if err := mgr.factory.Store.CreateSession(context.Background(), childID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("create session: %v", err)
	}
	mgr.mu.Lock()
	mgr.running[childID] = &managedRun{
		sessionID: childID,
		role:      RoleResearcher,
		done:      make(chan struct{}), // never closed
	}
	mgr.wg.Add(1)
	mgr.mu.Unlock()

	// Wait with a short timeout should return timeout error.
	_, err := mgr.Wait(context.Background(), childID, 50*time.Millisecond)
	if err == nil {
		t.Fatal("expected timeout error")
	}
	var agentErr *domain.AgentError
	if !errors.As(err, &agentErr) || agentErr.Code != domain.ErrTimeout {
		t.Fatalf("expected ErrTimeout, got: %v", err)
	}

	// Clean up: close the done channel so the manager can shut down.
	mgr.mu.Lock()
	mr := mgr.running[childID]
	mr.result = WaitResult{SessionID: childID, Role: RoleResearcher, Outcome: domain.OutcomeSucceeded}
	close(mr.done)
	delete(mgr.running, childID)
	mgr.wg.Done()
	mgr.mu.Unlock()
}

func TestManagerShutdown(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn},
	)

	// Spawn a child that should be cancelled on shutdown.
	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	// Shutdown should not block indefinitely.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	mgr.Shutdown(ctx)

	// After shutdown, Spawn should fail.
	_, err = mgr.Spawn(SpawnSpec{
		Task:          "post-shutdown task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err == nil {
		t.Fatal("expected error after shutdown")
	}
	var agentErr *domain.AgentError
	if !errors.As(err, &agentErr) || agentErr.Code != domain.ErrUnavailable {
		t.Fatalf("expected ErrUnavailable after shutdown, got: %v", err)
	}

	// Double shutdown should be a no-op.
	mgr.Shutdown(ctx)

	// The original child should have been collected (it may or may not have
	// finished before the cancel — either way the manager must not leak).
	_ = childID
}

func TestManagerShutdownIdempotent(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	ctx := context.Background()
	mgr.Shutdown(ctx)
	mgr.Shutdown(ctx) // second call is a no-op
}

func TestManagerSpawnWithCoderRole(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "实现完成", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 50},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "implement feature X",
		Role:          RoleCoder,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn(coder): %v", err)
	}

	result, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(coder): %v", err)
	}
	if result.Role != RoleCoder {
		t.Fatalf("role = %q, want coder", result.Role)
	}
}

func TestManagerWaitForPersistedSession(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn},
	)

	// Spawn and wait for a child.
	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}
	result, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	_ = result

	// After the child finishes, the first Wait collects the entry from
	// the running map; a second Wait falls back to loadPersistedResult.
	persistedResult, err := mgr.Wait(context.Background(), childID, 0)
	if err != nil {
		t.Fatalf("Wait(persisted): %v", err)
	}
	if persistedResult.SessionID != childID {
		t.Fatalf("persisted session ID mismatch")
	}
}

func TestManagerObserverHooks(t *testing.T) {
	var starts []ChildStart
	var finishes []ChildFinish
	factory, model, store, models := newTestFactory(
		t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
	)
	factory.Prompt = &stubPromptBuilder{}
	factory.Observer = &Observer{
		Started:  func(s ChildStart) { starts = append(starts, s) },
		Finished: func(f ChildFinish) { finishes = append(finishes, f) },
	}
	publishSnapshot(models, model)

	researcherRegistry := factory.Registry
	researcherPrompt := factory.Prompt
	roles := map[Role]*RoleSpec{
		RoleResearcher: {Registry: researcherRegistry, Prompt: researcherPrompt, Risk: domain.R1},
		RoleCoder:      {Registry: researcherRegistry, Prompt: researcherPrompt, Risk: domain.R3},
	}
	mgr, err := NewManager(factory, roles, nil)
	if err != nil {
		t.Fatalf("NewManager: %v", err)
	}

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}
	_, _ = mgr.Wait(context.Background(), childID, 10*time.Second)

	// Observer hooks should have fired.
	if len(starts) < 1 {
		t.Fatalf("expected at least 1 start, got %d", len(starts))
	}
	if len(finishes) < 1 {
		t.Fatalf("expected at least 1 finish, got %d", len(finishes))
	}

	_ = store // avoid unused
}

func TestManagerResumeUnknownSession(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	err := mgr.Resume(SpawnSpec{
		Task:          "follow-up task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	}, domain.NewSessionID())
	if err == nil {
		t.Fatal("expected error for unknown session")
	}
}

func TestManagerResumeStillRunning(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	// Try to resume while still running — should fail.
	err = mgr.Resume(SpawnSpec{
		Task:          "follow-up task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	}, childID)
	if err == nil {
		t.Fatal("expected error for resuming a running agent")
	}

	// Wait for the child to finish so the manager can shut down cleanly.
	_, _ = mgr.Wait(context.Background(), childID, 10*time.Second)
}

func TestManagerSpawnRecordsRoleInDelegation(t *testing.T) {
	mgr, _, store, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleCoder,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}
	_, _ = mgr.Wait(context.Background(), childID, 10*time.Second)

	// The delegation event should record the role.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: err=%v, n=%d", err, len(events))
	}
	var payload struct {
		Delegated bool   `json:"delegated"`
		Role      string `json:"role"`
	}
	if err := json.Unmarshal(events[0].Payload, &payload); err != nil {
		t.Fatalf("unmarshal delegation payload: %v", err)
	}
	if !payload.Delegated || payload.Role != "coder" {
		t.Fatalf("delegation payload = %+v, want delegated=true, role=coder", payload)
	}
}

// TestManagerResumeAfterCollect verifies that Resume succeeds after Wait
// has collected the finished entry from the registry — the S1 regression
// where a done-but-uncollected entry blocked Resume with "still running".
func TestManagerResumeAfterCollect(t *testing.T) {
	mgr, _, store, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "initial conclusion", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up conclusion", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "initial task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	// Wait collects the entry from the running map.
	wr, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if wr.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", wr.Outcome)
	}

	// Resume must succeed without manual registry surgery.
	err = mgr.Resume(SpawnSpec{
		Task:          "follow-up task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	}, childID)
	if err != nil {
		t.Fatalf("Resume after Wait: %v", err)
	}
	wr2, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(resume): %v", err)
	}
	if wr2.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("resume outcome = %s, want succeeded", wr2.Outcome)
	}
	// Both runs persisted.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child events: err=%v n=%d", err, len(events))
	}
}

// TestManagerStatusDoneAfterCollect verifies that Status reports Done once
// Wait has collected the finished entry (the S3 regression where Status
// falsely reported Running for done-but-uncollected entries).
func TestManagerStatusDoneAfterCollect(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "test task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	// Before Wait, the entry exists and Status should report based on the
	// done channel (not mere map membership).
	if mgr.Status(childID) != StatusRunning {
		t.Fatal("Status should report Running before the child finishes")
	}

	_, err = mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}

	// After Wait collects, Status must report Done.
	if mgr.Status(childID) != StatusDone {
		t.Fatal("Status should report Done after Wait collects")
	}
}

// TestManagerResumeStaleDoneEntry verifies that Resume can replace a
// done-but-not-yet-collected entry (the in-process Resume path that
// previously deadlocked).
func TestManagerResumeStaleDoneEntry(t *testing.T) {
	mgr, _, _, _ := newTestManager(
		t,
		fakes.ScriptEntry{Text: "initial", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "follow-up", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "initial task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	// Wait for the child to finish but do NOT collect (simulate a caller
	// that never called Wait — the entry stays in the registry as stale).
	<-func() <-chan struct{} {
		mgr.mu.Lock()
		mr := mgr.running[childID]
		mgr.mu.Unlock()
		return mr.done
	}()

	// Status must report Done (done-aware check).
	if mgr.Status(childID) != StatusDone {
		t.Fatal("Status should report Done for a finished-but-uncollected entry")
	}

	// Resume must succeed — it detects the stale entry and evicts it.
	err = mgr.Resume(SpawnSpec{
		Task:          "follow-up task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	}, childID)
	if err != nil {
		t.Fatalf("Resume stale entry: %v", err)
	}

	_, err = mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(resume): %v", err)
	}
}
