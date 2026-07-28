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

package app

import (
	"context"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestChannelApproverRequiresExactBinding(t *testing.T) {
	approver := NewChannelApprover()
	approvalID := domain.NewEventID()
	callID := domain.NewToolCallID()
	request := domain.ApprovalRequest{
		ID: approvalID,
		Call: domain.PreparedCall{
			Call:     domain.ToolCall{ID: callID},
			ArgsHash: "canonical-args-hash",
		},
	}
	result := make(chan domain.Decision, 1)
	errs := make(chan error, 1)
	go func() {
		decision, err := approver.RequestApproval(context.Background(), request)
		result <- decision
		errs <- err
	}()

	deadline := time.After(time.Second)
	for approver.PendingCount() != 1 {
		select {
		case <-deadline:
			t.Fatal("approval was not registered")
		default:
			time.Sleep(time.Millisecond)
		}
	}

	if approver.ResolveApproval(ApprovalBinding{ApprovalID: approvalID, CallID: callID, ArgsHash: "tampered"}, domain.DecisionAllow) {
		t.Fatal("accepted approval with tampered args hash")
	}
	if approver.ResolveApproval(ApprovalBinding{ApprovalID: approvalID, CallID: domain.NewToolCallID(), ArgsHash: "canonical-args-hash"}, domain.DecisionAllow) {
		t.Fatal("accepted approval with mismatched call ID")
	}
	if !approver.ResolveApproval(ApprovalBinding{ApprovalID: approvalID, CallID: callID, ArgsHash: "canonical-args-hash"}, domain.DecisionAllow) {
		t.Fatal("rejected exact approval binding")
	}
	if decision := <-result; decision != domain.DecisionAllow {
		t.Fatalf("decision = %q, want allow", decision)
	}
	if err := <-errs; err != nil {
		t.Fatalf("RequestApproval: %v", err)
	}
}

// Regression (REVIEW H4): the agent loop publishes approval.requested (via
// the event flush) BEFORE RequestApproval registers the pending slot. A
// frontend answering inside that window used to get "binding does not match
// a pending request" while the agent kept waiting — the decision was lost.
// Early decisions must be cached and delivered to the matching request.
func TestChannelApproverDecisionBeforeRegistration(t *testing.T) {
	approver := NewChannelApprover()
	approvalID := domain.NewEventID()
	callID := domain.NewToolCallID()
	binding := ApprovalBinding{ApprovalID: approvalID, CallID: callID, ArgsHash: "canonical-args-hash"}

	// The frontend answers before the agent registers the pending slot.
	if !approver.ResolveApproval(binding, domain.DecisionAllow) {
		t.Fatal("early decision was rejected; it should be cached for the matching request")
	}

	decision, err := approver.RequestApproval(context.Background(), domain.ApprovalRequest{
		ID:   approvalID,
		Call: domain.PreparedCall{Call: domain.ToolCall{ID: callID}, ArgsHash: "canonical-args-hash"},
	})
	if err != nil {
		t.Fatalf("RequestApproval: %v", err)
	}
	if decision != domain.DecisionAllow {
		t.Fatalf("decision = %q, want allow", decision)
	}
	if got := approver.PendingCount(); got != 0 {
		t.Fatalf("pending = %d, want 0 after the early decision was consumed", got)
	}
}

// An early decision whose binding does not match the eventual request must
// never be delivered: the request blocks as usual.
func TestChannelApproverEarlyDecisionBindingMismatch(t *testing.T) {
	approver := NewChannelApprover()
	approvalID := domain.NewEventID()
	approver.ResolveApproval(ApprovalBinding{ApprovalID: approvalID, CallID: domain.NewToolCallID(), ArgsHash: "tampered"}, domain.DecisionAllow)

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	_, err := approver.RequestApproval(ctx, domain.ApprovalRequest{
		ID:   approvalID,
		Call: domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID()}, ArgsHash: "canonical"},
	})
	if err == nil {
		t.Fatal("mismatched early decision must not satisfy the request")
	}
}

func TestChannelApproverDenyAllIsIdempotent(t *testing.T) {
	approver := NewChannelApprover()
	approver.DenyAll()
	approver.DenyAll()
	_, err := approver.RequestApproval(context.Background(), domain.ApprovalRequest{
		ID:   domain.NewEventID(),
		Call: domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID()}},
	})
	if err == nil {
		t.Fatal("expected shutdown error")
	}
}
