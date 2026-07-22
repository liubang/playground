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

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestFakeApproverAllow(t *testing.T) {
	a := NewFakeApprover(domain.DecisionAllow)
	dec, err := a.RequestApproval(context.Background(), domain.ApprovalRequest{
		ID:          domain.NewEventID(),
		Description: "test",
	})
	if err != nil {
		t.Fatalf("RequestApproval error: %v", err)
	}
	if dec != domain.DecisionAllow {
		t.Fatalf("expected allow, got %s", dec)
	}
}

func TestFakeApproverDeny(t *testing.T) {
	a := NewFakeApprover(domain.DecisionDeny)
	dec, _ := a.RequestApproval(context.Background(), domain.ApprovalRequest{
		ID: domain.NewEventID(),
	})
	if dec != domain.DecisionDeny {
		t.Fatalf("expected deny, got %s", dec)
	}
}

func TestFakeApproverAsk(t *testing.T) {
	a := NewFakeApprover(domain.DecisionAsk)
	dec, _ := a.RequestApproval(context.Background(), domain.ApprovalRequest{
		ID: domain.NewEventID(),
	})
	if dec != domain.DecisionAsk {
		t.Fatalf("expected ask, got %s", dec)
	}
}

func TestFakeApproverTracksRequests(t *testing.T) {
	a := NewFakeApprover(domain.DecisionAllow)

	req1 := domain.ApprovalRequest{ID: domain.NewEventID(), Description: "first"}
	req2 := domain.ApprovalRequest{ID: domain.NewEventID(), Description: "second"}

	_, _ = a.RequestApproval(context.Background(), req1)
	_, _ = a.RequestApproval(context.Background(), req2)

	requests := a.Requests()
	if len(requests) != 2 {
		t.Fatalf("expected 2 requests, got %d", len(requests))
	}
	if requests[0].Description != "first" {
		t.Errorf("expected first, got %s", requests[0].Description)
	}
	if requests[1].Description != "second" {
		t.Errorf("expected second, got %s", requests[1].Description)
	}
}
