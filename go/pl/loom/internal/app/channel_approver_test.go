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
