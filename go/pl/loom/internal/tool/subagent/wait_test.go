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
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

func TestNewWaitSubagentToolNilManager(t *testing.T) {
	_, err := NewWaitSubagentTool(nil)
	if err == nil {
		t.Fatal("expected error for nil manager")
	}
	if !strings.Contains(err.Error(), "non-nil manager") {
		t.Fatalf("error = %q, want non-nil manager hint", err.Error())
	}
}

func TestWaitSubagentToolDefinition(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}
	def := tool.Definition()
	if def.Name != "wait_subagent" {
		t.Fatalf("name = %q, want wait_subagent", def.Name)
	}
	if def.Source != domain.ToolSourceSubAgent {
		t.Fatalf("source = %q, want subagent", def.Source)
	}
	if !tool.ConcurrentSafe() {
		t.Fatal("ConcurrentSafe should be true")
	}
}

func TestWaitSubagentPrepareValidation(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	for name, raw := range map[string]string{
		"missing session ID":  `{}`,
		"unknown field":       `{"child_session_id":"sess_01","extra":true}`,
		"empty session ID":   `{"child_session_id":""}`,
	} {
		_, err := tool.Prepare(context.Background(), domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "wait_subagent",
			Arguments: json.RawMessage(raw),
		})
		if err == nil {
			t.Fatalf("%s: expected prepare error", name)
		}
	}
}

func TestWaitSubagentPrepareCanonicalizesSessionID(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	childID := domain.NewSessionID()
	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if prepared.Risk != domain.R1 {
		t.Fatalf("risk = %d, want R1", prepared.Risk)
	}
	if !strings.Contains(prepared.ApprovalDesc, childID.String()) {
		t.Fatalf("approval desc = %q, want session ID", prepared.ApprovalDesc)
	}
}

func TestWaitSubagentExecuteCompleted(t *testing.T) {
	mgr, _, _, _ := newTestManager(t,
		fakes.ScriptEntry{Text: "结论：查找完毕", StopReason: domain.StopEndTurn, UsageIn: 80, UsageOut: 20},
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

	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, err = %+v", result.Status, result.Error)
	}

	var payload map[string]any
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload["status"] != "completed" {
		t.Fatalf("payload status = %v, want completed", payload["status"])
	}
	if payload["conclusion"] != "结论：查找完毕" {
		t.Fatalf("conclusion = %v, want 结论：查找完毕", payload["conclusion"])
	}
	if payload["role"] != "researcher" {
		t.Fatalf("role = %v, want researcher", payload["role"])
	}

	// External usage metadata should be present.
	if result.Metadata[domain.ToolMetaExternalInputTokens] != "80" {
		t.Fatalf("external input tokens = %q, want 80", result.Metadata[domain.ToolMetaExternalInputTokens])
	}
	if result.Metadata[domain.ToolMetaExternalOutputTokens] != "20" {
		t.Fatalf("external output tokens = %q, want 20", result.Metadata[domain.ToolMetaExternalOutputTokens])
	}
}

func TestWaitSubagentExecuteTimeout(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	// Manually insert a running entry that never finishes.
	childID := domain.NewSessionID()
	if err := mgr.factory.Store.CreateSession(context.Background(), childID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("create session: %v", err)
	}
	mgr.mu.Lock()
	mgr.running[childID] = &managedRun{
		sessionID: childID,
		role:      RoleResearcher,
		done:      make(chan struct{}),
	}
	mgr.wg.Add(1)
	mgr.mu.Unlock()

	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"timeout_seconds":  1,
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, want success (timeout is not an error status)", result.Status)
	}
	var payload map[string]any
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload["status"] != "timeout" {
		t.Fatalf("payload status = %v, want timeout", payload["status"])
	}

	// Clean up.
	mgr.mu.Lock()
	mr := mgr.running[childID]
	mr.result = WaitResult{SessionID: childID, Role: RoleResearcher, Outcome: domain.OutcomeSucceeded}
	close(mr.done)
	delete(mgr.running, childID)
	mgr.wg.Done()
	mgr.mu.Unlock()
}

func TestWaitSubagentExecuteUnknownSession(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)

	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	unknownID := domain.NewSessionID()
	args, _ := json.Marshal(map[string]any{
		"child_session_id": unknownID.String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	// Unknown session not in store → error result (loadPersistedResult fails).
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error for unknown session", result.Status)
	}
}

func TestWaitSubagentExecuteInvalidArgs(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	// Execute with bad arguments — should produce an error result.
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "wait_subagent",
			Arguments: json.RawMessage(`{}`),
		},
		Definition: tool.Definition(),
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error", result.Status)
	}
}

func TestWaitSubagentExecuteWithCoderRole(t *testing.T) {
	mgr, _, _, _ := newTestManager(t,
		fakes.ScriptEntry{Text: "实现完毕", StopReason: domain.StopEndTurn, UsageIn: 120, UsageOut: 40},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "implement X",
		Role:          RoleCoder,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn(coder): %v", err)
	}

	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, err = %+v", result.Status, result.Error)
	}

	var payload map[string]any
	if err := json.Unmarshal([]byte(result.Content[0].Text), &payload); err != nil {
		t.Fatalf("unmarshal payload: %v", err)
	}
	if payload["role"] != "coder" {
		t.Fatalf("role = %v, want coder", payload["role"])
	}
	if payload["status"] != "completed" {
		t.Fatalf("status = %v, want completed", payload["status"])
	}
}

func TestWaitSubagentZeroTimeoutWaitsIndefinitely(t *testing.T) {
	mgr, _, _, _ := newTestManager(t,
		fakes.ScriptEntry{Text: "快结果", StopReason: domain.StopEndTurn},
	)

	childID, err := mgr.Spawn(SpawnSpec{
		Task:          "fast task",
		Role:          RoleResearcher,
		ParentSession: domain.NewSessionID(),
		ParentCall:    domain.NewToolCallID(),
	})
	if err != nil {
		t.Fatalf("Spawn: %v", err)
	}

	tool, err := NewWaitSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewWaitSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		// no timeout_seconds → zero timeout → wait indefinitely
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "wait_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	// The child finishes quickly; zero-timeout Wait should still succeed.
	done := make(chan domain.ToolResult, 1)
	go func() {
		done <- tool.Execute(context.Background(), prepared)
	}()

	select {
	case result := <-done:
		if result.Status != domain.ToolStatusSuccess {
			t.Fatalf("status = %s", result.Status)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("wait with zero timeout should still complete when child finishes")
	}
}
