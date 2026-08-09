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
//
// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/23

package app

import (
	"context"
	"fmt"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ApprovalBinding is the immutable identity of a prepared call shown to a
// frontend. A decision is accepted only when all fields match the request the
// agent is currently waiting on.
type ApprovalBinding struct {
	ApprovalID domain.EventID
	CallID     domain.ToolCallID
	ArgsHash   string
}

type pendingApproval struct {
	binding  ApprovalBinding
	resultCh chan domain.Decision
}

// earlyDecision is a frontend decision that arrived before the agent
// registered its pending slot. The agent loop publishes approval.requested
// (via the event flush) before RequestApproval runs, so a fast frontend can
// legitimately answer inside that window.
type earlyDecision struct {
	binding  ApprovalBinding
	decision domain.Decision
}

// maxEarlyDecisions bounds the early-decision cache. Entries whose request
// never arrives (for example answers to approvals a rule auto-resolved, or
// duplicate clicks) are evicted oldest-first.
const maxEarlyDecisions = 16

// ChannelApprover bridges approval requests between the agent loop and a
// frontend (TUI or other UI). It implements domain.Approver and uses channels
// to receive a decision bound to the original PreparedCall.
type ChannelApprover struct {
	mu         sync.Mutex
	pending    map[domain.EventID]pendingApproval
	early      map[domain.EventID]earlyDecision
	earlyOrder []domain.EventID
	done       chan struct{}
	doneOnce   sync.Once
}

// NewChannelApprover creates a new ChannelApprover.
func NewChannelApprover() *ChannelApprover {
	return &ChannelApprover{
		pending: make(map[domain.EventID]pendingApproval),
		early:   make(map[domain.EventID]earlyDecision),
		done:    make(chan struct{}),
	}
}

// RequestApproval implements domain.Approver. It creates a pending approval
// slot and blocks until the frontend resolves the exact bound call or the
// context is cancelled.
func (a *ChannelApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	binding := ApprovalBinding{
		ApprovalID: req.ID,
		CallID:     req.Call.Call.ID,
		ArgsHash:   req.Call.ArgsHash,
	}
	resultCh := make(chan domain.Decision, 1)
	a.mu.Lock()
	select {
	case <-a.done:
		a.mu.Unlock()
		return domain.DecisionDeny, fmt.Errorf("approver shut down")
	default:
	}
	// Consume a decision that arrived before registration. A mismatched
	// cached decision can never satisfy this request; drop it either way.
	if early, ok := a.early[req.ID]; ok {
		a.removeEarlyLocked(req.ID)
		if early.binding == binding {
			a.mu.Unlock()
			return early.decision, nil
		}
	}
	a.pending[req.ID] = pendingApproval{binding: binding, resultCh: resultCh}
	a.mu.Unlock()
	defer func() {
		a.mu.Lock()
		delete(a.pending, req.ID)
		a.mu.Unlock()
	}()

	// The run is now blocked on a human, and the human is often away
	// from the terminal during long-horizon runs: surface a desktop
	// notification alongside the in-app prompt.
	NotifyApproval(req.Call.Call.Name, req.Description)

	select {
	case decision := <-resultCh:
		return decision, nil
	case <-a.done:
		return domain.DecisionDeny, fmt.Errorf("approver shut down")
	case <-ctx.Done():
		return domain.DecisionDeny, ctx.Err()
	}
}

// PendingBinding returns the binding registered for the given approval ID,
// or ok=false when no request with that ID is currently waiting. Callers use
// it to validate a frontend decision BEFORE acting on it (e.g. persisting an
// "allow always" memory) without consuming the pending slot.
func (a *ChannelApprover) PendingBinding(id domain.EventID) (ApprovalBinding, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	pending, ok := a.pending[id]
	if !ok {
		return ApprovalBinding{}, false
	}
	return pending.binding, true
}

// ResolveApproval resolves a pending approval only when the binding returned
// by the frontend still matches the canonical prepared call.
func (a *ChannelApprover) ResolveApproval(binding ApprovalBinding, decision domain.Decision) bool {
	a.mu.Lock()
	pending, ok := a.pending[binding.ApprovalID]
	if !ok {
		// No slot yet: the decision beat the registration (the
		// approval.requested event is published first). Cache it so the
		// matching RequestApproval consumes it instead of losing it.
		a.rememberEarlyLocked(binding, decision)
		a.mu.Unlock()
		return true
	}
	if pending.binding != binding {
		a.mu.Unlock()
		return false
	}
	// Delete while holding the lock to provide one-shot compare-and-swap
	// semantics for duplicate or racing decisions.
	delete(a.pending, binding.ApprovalID)
	a.mu.Unlock()

	pending.resultCh <- decision
	return true
}

// rememberEarlyLocked caches a decision that arrived before its request.
// Callers must hold a.mu.
func (a *ChannelApprover) rememberEarlyLocked(binding ApprovalBinding, decision domain.Decision) {
	if _, exists := a.early[binding.ApprovalID]; !exists {
		a.earlyOrder = append(a.earlyOrder, binding.ApprovalID)
	}
	a.early[binding.ApprovalID] = earlyDecision{binding: binding, decision: decision}
	for len(a.earlyOrder) > maxEarlyDecisions {
		oldest := a.earlyOrder[0]
		a.earlyOrder = a.earlyOrder[1:]
		delete(a.early, oldest)
	}
}

// removeEarlyLocked drops a cached early decision. Callers must hold a.mu.
func (a *ChannelApprover) removeEarlyLocked(id domain.EventID) {
	if _, ok := a.early[id]; !ok {
		return
	}
	delete(a.early, id)
	for i, queued := range a.earlyOrder {
		if queued == id {
			a.earlyOrder = append(a.earlyOrder[:i], a.earlyOrder[i+1:]...)
			break
		}
	}
}

// DenyAll denies all pending approvals and prevents future requests. It is
// idempotent and safe to call during concurrent shutdown paths.
func (a *ChannelApprover) DenyAll() {
	a.doneOnce.Do(func() { close(a.done) })
}

// PendingCount returns the number of pending approvals.
func (a *ChannelApprover) PendingCount() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.pending)
}

// PendingApprovals returns a list of all pending approval IDs.
func (a *ChannelApprover) PendingApprovals() []domain.EventID {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := make([]domain.EventID, 0, len(a.pending))
	for id := range a.pending {
		out = append(out, id)
	}
	return out
}
