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
// Created: 2026/09/27

package subagent

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestNewCancelSubagentToolNilManager(t *testing.T) {
	_, err := NewCancelSubagentTool(nil)
	if err == nil {
		t.Fatal("expected error for nil manager")
	}
	if !strings.Contains(err.Error(), "non-nil manager") {
		t.Fatalf("error = %q, want non-nil manager hint", err.Error())
	}
}

func TestCancelSubagentToolDefinition(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}
	def := tool.Definition()
	if def.Name != "cancel_subagent" {
		t.Fatalf("name = %q, want cancel_subagent", def.Name)
	}
	if def.Source != domain.ToolSourceSubAgent {
		t.Fatalf("source = %q, want subagent", def.Source)
	}
	if !tool.ConcurrentSafe() {
		t.Fatal("ConcurrentSafe should be true")
	}
}

func TestCancelSubagentPrepareValidation(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}

	for name, raw := range map[string]string{
		"missing session ID": `{}`,
		"unknown field":      `{"child_session_id":"sess_01","extra":true}`,
		"empty session ID":   `{"child_session_id":""}`,
	} {
		_, err := tool.Prepare(context.Background(), domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "cancel_subagent",
			Arguments: json.RawMessage(raw),
		})
		if err == nil {
			t.Fatalf("%s: expected prepare error", name)
		}
	}
}

func TestCancelSubagentPrepareRisk(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}
	args, _ := json.Marshal(map[string]any{
		"child_session_id": domain.NewSessionID().String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "cancel_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if prepared.Risk != domain.R1 {
		t.Fatalf("risk = %v, want R1 (bookkeeping, no approval)", prepared.Risk)
	}
}

func TestCancelSubagentExecuteUnknownSession(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}
	args, _ := json.Marshal(map[string]any{
		"child_session_id": domain.NewSessionID().String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "cancel_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error for unknown session", result.Status)
	}
}

// stageRunning registers a fake in-flight child whose cancel func lands
// the terminal result and closes done, mimicking a loop that observes
// its context cancellation.
func stageRunning(t *testing.T, mgr *Manager) (domain.SessionID, *managedRun) {
	t.Helper()
	childID := domain.NewSessionID()
	if err := mgr.factory.Store.CreateSession(context.Background(), childID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("create session: %v", err)
	}
	mr := &managedRun{
		sessionID: childID,
		role:      RoleResearcher,
		done:      make(chan struct{}),
	}
	mr.cancel = func() {
		mr.result = WaitResult{
			SessionID:  childID,
			Role:       RoleResearcher,
			Outcome:    domain.OutcomeCancelled,
			Conclusion: "partial work",
			Usage:      domain.Usage{InputTokens: 12, OutputTokens: 4, CachedInputTokens: 8, ContextTokens: 12},
		}
		close(mr.done)
	}
	mgr.mu.Lock()
	mgr.running[childID] = mr
	mgr.wg.Add(1)
	mgr.mu.Unlock()
	t.Cleanup(func() {
		mgr.mu.Lock()
		if _, ok := mgr.running[childID]; ok {
			delete(mgr.running, childID)
			mgr.wg.Done()
		}
		mgr.mu.Unlock()
	})
	return childID, mr
}

func TestManagerCancelRunning(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	childID, _ := stageRunning(t, mgr)

	result, err := mgr.Cancel(context.Background(), childID)
	if err != nil {
		t.Fatalf("Cancel: %v", err)
	}
	if result.Outcome != domain.OutcomeCancelled {
		t.Fatalf("outcome = %q, want cancelled", result.Outcome)
	}
	// The registry entry is collected by the cancel.
	if status := mgr.Status(childID); status != StatusDone {
		t.Fatalf("status after cancel = %q, want done", status)
	}

	// Cancelling again: the entry is gone, the persisted session has no
	// terminal checkpoint — an error is expected.
	if _, err := mgr.Cancel(context.Background(), childID); err == nil {
		t.Fatal("expected error cancelling an already-collected agent without a terminal checkpoint")
	}
}

func TestCancelSubagentExecuteCancelled(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	childID, _ := stageRunning(t, mgr)

	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}
	args, _ := json.Marshal(map[string]any{"child_session_id": childID.String()})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "cancel_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, want success: %+v", result.Status, result.Error)
	}
	var payload map[string]any
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("decode payload: %v", err)
	}
	if payload["status"] != "cancelled" {
		t.Fatalf("status = %v, want cancelled", payload["status"])
	}
	if payload["outcome"] != string(domain.OutcomeCancelled) {
		t.Fatalf("outcome = %v, want cancelled", payload["outcome"])
	}
	// Usage rides the metadata so the parent loop accounts for it.
	if result.Metadata[domain.ToolMetaExternalInputTokens] != "12" {
		t.Fatalf("external input tokens = %q, want 12", result.Metadata[domain.ToolMetaExternalInputTokens])
	}
	if result.Metadata[domain.ToolMetaExternalCachedInputTokens] != "8" {
		t.Fatalf("external cached input tokens = %q, want 8", result.Metadata[domain.ToolMetaExternalCachedInputTokens])
	}
	if result.Metadata[domain.ToolMetaExternalContextTokens] != "12" {
		t.Fatalf("external context tokens = %q, want 12", result.Metadata[domain.ToolMetaExternalContextTokens])
	}
}

func TestCancelSubagentExecuteAlreadyDone(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	childID, mr := stageRunning(t, mgr)
	// Land the terminal state BEFORE the cancel: the agent finished on
	// its own, so the cancel reports already_done.
	mr.result = WaitResult{
		SessionID:  childID,
		Role:       RoleResearcher,
		Outcome:    domain.OutcomeSucceeded,
		Conclusion: "done",
	}
	close(mr.done)

	tool, err := NewCancelSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewCancelSubagentTool: %v", err)
	}
	args, _ := json.Marshal(map[string]any{"child_session_id": childID.String()})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "cancel_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, want success: %+v", result.Status, result.Error)
	}
	var payload map[string]any
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("decode payload: %v", err)
	}
	if payload["status"] != "already_done" {
		t.Fatalf("status = %v, want already_done", payload["status"])
	}
}
