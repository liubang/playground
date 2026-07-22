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
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// FakeApprover is a test double for domain.Approver.
type FakeApprover struct {
	mu       sync.Mutex
	decision domain.Decision
	requests []domain.ApprovalRequest
}

// NewFakeApprover creates a FakeApprover that always returns the given decision.
func NewFakeApprover(decision domain.Decision) *FakeApprover {
	return &FakeApprover{decision: decision}
}

func (a *FakeApprover) RequestApproval(_ context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.requests = append(a.requests, req)
	return a.decision, nil
}

// Requests returns all approval requests received.
func (a *FakeApprover) Requests() []domain.ApprovalRequest {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := make([]domain.ApprovalRequest, len(a.requests))
	copy(out, a.requests)
	return out
}
