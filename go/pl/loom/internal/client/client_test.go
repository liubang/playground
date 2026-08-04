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
// Created: 2026/08/03

package client

import (
	"bytes"
	"encoding/json"
	"reflect"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// TestClientTypesAreJSONSerializable is the interface hard-constraint guard
// (docs/SERVE_DESIGN.md §17.5): every request/response type crossing the
// Client boundary must survive a JSON roundtrip unchanged, so the inproc
// and http implementations can never drift apart in shape.
func TestClientTypesAreJSONSerializable(t *testing.T) {
	approvalID := domain.NewEventID()
	samples := map[string]any{
		"Snapshot": Snapshot{
			State:         ControllerStateRunning,
			SessionID:     domain.NewSessionID(),
			RunID:         domain.NewRunID(),
			ModelName:     "gpt-5",
			TurnCount:     2,
			Usage:         domain.Usage{InputTokens: 10, OutputTokens: 20},
			EventSeq:      42,
			Timestamp:     time.Now().UTC(),
			PendingSteers: []string{"steer note"},
			Messages: []domain.Message{{
				ID:        domain.NewMessageID(),
				Role:      domain.RoleUser,
				Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hello"}},
				CreatedAt: time.Now().UTC(),
			}},
			PendingRequests: []PendingRequest{
				{
					Kind: PendingRequestApproval,
					ID:   approvalID,
					Approval: &runtimeevent.ApprovalRequestedPayload{
						ApprovalID:  approvalID,
						CallID:      domain.NewToolCallID(),
						ToolName:    "edit",
						Risk:        domain.R2,
						Description: "edit file",
						ArgsHash:    "abc123",
						Diff:        "- a\n+ b",
						Arguments:   json.RawMessage(`{"path":"x.go"}`),
					},
				},
				{
					Kind: PendingRequestQuestion,
					ID:   domain.NewEventID(),
					Question: &domain.Question{
						ID:      domain.NewEventID(),
						Text:    "which one?",
						Options: []domain.QuestionOption{{Label: "a"}, {Label: "b", Description: "the b one"}},
					},
				},
			},
		},
		"SubmitResult":            SubmitResult{Steered: true, QueueLen: 2, Turn: 3},
		"ApprovalBinding":         ApprovalBinding{ApprovalID: approvalID, CallID: domain.NewToolCallID(), ArgsHash: "hash"},
		"AnswerQuestionResult":    AnswerQuestionResult{Resolved: true},
		"SetReasoningResult":      SetReasoningResult{},
		"RequestCompactionResult": RequestCompactionResult{AlreadyPending: true},
		"SessionSummary":          SessionSummary{ID: domain.NewSessionID(), Version: 7, CreatedAt: time.Now().UTC(), UpdatedAt: time.Now().UTC()},
		"RuleSet":                 &permission.RuleSet{},
		"SubagentView":            SubagentView{SessionID: domain.NewSessionID(), Active: true},
		"SkillsListing":           SkillsListing{Skills: []SkillInfo{{Name: "s", Description: "d", Scope: "user", Path: "/p"}}, Issues: []string{"i"}},
		"MCPServerInfo":           MCPServerInfo{Name: "m", Connected: true, Tools: []string{"t1"}},
		"SetModelResult":          SetModelResult{Prev: config.ProviderModelRef{Provider: "p", Model: "m1"}, Cur: config.ProviderModelRef{Provider: "p", Model: "m2"}},
		"CheckpointInfo":          CheckpointInfo{Sequence: 3, CreatedAt: time.Now().UTC(), Label: "l", Turns: 2},
		"RewindOutcome":           RewindOutcome{Checkpoint: CheckpointInfo{Sequence: 1}, Restored: []string{"a.go"}},
	}
	for name, sample := range samples {
		t.Run(name, func(t *testing.T) {
			data, err := json.Marshal(sample)
			if err != nil {
				t.Fatalf("marshal: %v", err)
			}
			typ := reflect.TypeOf(sample)
			if typ.Kind() == reflect.Ptr {
				typ = typ.Elem()
			}
			fresh := reflect.New(typ)
			if err := json.Unmarshal(data, fresh.Interface()); err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			again, err := json.Marshal(fresh.Interface())
			if err != nil {
				t.Fatalf("re-marshal: %v", err)
			}
			if !bytes.Equal(data, again) {
				t.Fatalf("roundtrip mismatch:\nfirst:  %s\nsecond: %s", data, again)
			}
		})
	}
}
