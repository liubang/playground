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

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

func TestNewResumeSubagentToolNilManager(t *testing.T) {
	_, err := NewResumeSubagentTool(nil)
	if err == nil {
		t.Fatal("expected error for nil manager")
	}
	if !strings.Contains(err.Error(), "non-nil manager") {
		t.Fatalf("error = %q, want non-nil manager hint", err.Error())
	}
}

func TestResumeSubagentToolDefinition(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}
	def := tool.Definition()
	if def.Name != "resume_subagent" {
		t.Fatalf("name = %q, want resume_subagent", def.Name)
	}
	if def.Source != domain.ToolSourceSubAgent {
		t.Fatalf("source = %q, want subagent", def.Source)
	}
	if !tool.ConcurrentSafe() {
		t.Fatal("ConcurrentSafe should be true")
	}
}

func TestResumeSubagentPrepareValidation(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	for name, raw := range map[string]string{
		"missing session ID": `{"task":"follow-up"}`,
		"empty task":        `{"child_session_id":"sess_00000000000000000000000000","task":""}`,
		"blank task":        `{"child_session_id":"sess_00000000000000000000000000","task":"   "}`,
		"unknown field":     `{"child_session_id":"sess_00000000000000000000000000","task":"ok","extra":1}`,
		"invalid role":      `{"child_session_id":"sess_00000000000000000000000000","task":"ok","role":"admin"}`,
	} {
		_, err := tool.Prepare(context.Background(), domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "resume_subagent",
			Arguments: json.RawMessage(raw),
		})
		if err == nil {
			t.Fatalf("%s: expected prepare error", name)
		}
	}
}

func TestResumeSubagentPrepareRiskByRole(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	childID := domain.NewSessionID()

	// Default (empty) role → researcher → R1.
	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"task":             "follow-up",
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if prepared.Risk != domain.R1 {
		t.Fatalf("default risk = %d, want R1", prepared.Risk)
	}

	// Coder role → R3.
	args, _ = json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"task":             "follow-up",
		"role":             "coder",
	})
	prepared, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if prepared.Risk != domain.R3 {
		t.Fatalf("coder risk = %d, want R3", prepared.Risk)
	}
}

func TestResumeSubagentExecuteResumed(t *testing.T) {
	// Phase 1: Spawn a child that completes quickly.
	mgr, _, _, _ := newTestManager(t,
		fakes.ScriptEntry{Text: "结论：初步研究完成", StopReason: domain.StopEndTurn, UsageIn: 50, UsageOut: 10},
		fakes.ScriptEntry{Text: "结论：后续研究完成", StopReason: domain.StopEndTurn, UsageIn: 60, UsageOut: 15},
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

	// Wait for the initial run to complete (collects from the registry).
	_, err = mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(initial): %v", err)
	}

	// Phase 2: Resume with a follow-up task (no manual delete needed —
	// Wait collected the stale entry; Resume handles done-but-uncollected
	// entries too).
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"task":             "follow-up research",
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
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
	if payload["status"] != "resumed" {
		t.Fatalf("payload status = %v, want resumed", payload["status"])
	}
	if payload["child_session_id"] != childID.String() {
		t.Fatalf("child_session_id = %v, want %s", payload["child_session_id"], childID)
	}
	if payload["role"] != "researcher" {
		t.Fatalf("role = %v, want researcher", payload["role"])
	}

	// Metadata should contain the child session reference.
	if result.Metadata["child_session_id"] != childID.String() {
		t.Fatalf("metadata child_session_id = %q, want %s", result.Metadata["child_session_id"], childID)
	}
	if result.Metadata["spawn_status"] != "resumed" {
		t.Fatalf("metadata spawn_status = %q, want resumed", result.Metadata["spawn_status"])
	}

	// Wait for the resumed child to finish and verify it produced the follow-up conclusion.
	resumeResult, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(resumed): %v", err)
	}
	if resumeResult.Conclusion != "结论：后续研究完成" {
		t.Fatalf("resumed conclusion = %q, want 结论：后续研究完成", resumeResult.Conclusion)
	}
}

func TestResumeSubagentExecuteUnknownSession(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	unknownID := domain.NewSessionID()
	args, _ := json.Marshal(map[string]any{
		"child_session_id": unknownID.String(),
		"task":             "follow-up",
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
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

func TestResumeSubagentExecuteNoModel(t *testing.T) {
	store := fakes.NewFakeStore()
	registry := agent.NewToolRegistry()
	models := &ModelSource{} // no snapshot

	// ... existing code pattern from manager_test.go ...
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

	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	childID := domain.NewSessionID()
	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"task":             "follow-up",
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error for no model", result.Status)
	}
	if !strings.Contains(result.Error.Message, "no model selection") {
		t.Fatalf("error message = %q, want no model selection hint", result.Error.Message)
	}
}

func TestResumeSubagentExecuteInvalidArgs(t *testing.T) {
	mgr, _, _, _ := newTestManager(t)
	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	// Execute with bad arguments — should produce an error result.
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "resume_subagent",
			Arguments: json.RawMessage(`{}`),
		},
		Definition: tool.Definition(),
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error", result.Status)
	}
}

func TestTrimAndValidateTask(t *testing.T) {
	// Whitespace is trimmed.
	if got := trimAndValidateTask("  hello  "); got != "hello" {
		t.Fatalf("trim = %q, want hello", got)
	}
	// Overlong task is truncated.
	long := strings.Repeat("x", maxTaskBytes+100)
	if got := trimAndValidateTask(long); len(got) != maxTaskBytes {
		t.Fatalf("truncated length = %d, want %d", len(got), maxTaskBytes)
	}
	// Normal task passes through.
	if got := trimAndValidateTask("normal task"); got != "normal task" {
		t.Fatalf("normal = %q, want normal task", got)
	}
}

func TestResumeSubagentWithFocusPaths(t *testing.T) {
	mgr, _, _, _ := newTestManager(t,
		fakes.ScriptEntry{Text: "结论：已完成", StopReason: domain.StopEndTurn},
		fakes.ScriptEntry{Text: "结论：聚焦完成", StopReason: domain.StopEndTurn},
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
	_, _ = mgr.Wait(context.Background(), childID, 10*time.Second)
	// Wait collected the stale entry; no manual delete needed.

	tool, err := NewResumeSubagentTool(mgr)
	if err != nil {
		t.Fatalf("NewResumeSubagentTool: %v", err)
	}

	args, _ := json.Marshal(map[string]any{
		"child_session_id": childID.String(),
		"task":             "look deeper",
		"focus":            []string{"main.go", "util.go"},
	})
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "resume_subagent",
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
	if payload["status"] != "resumed" {
		t.Fatalf("payload status = %v, want resumed", payload["status"])
	}

	// Wait for the resumed child to finish.
	resumeResult, err := mgr.Wait(context.Background(), childID, 10*time.Second)
	if err != nil {
		t.Fatalf("Wait(resumed): %v", err)
	}
	if resumeResult.Conclusion != "结论：聚焦完成" {
		t.Fatalf("conclusion = %q, want 结论：聚焦完成", resumeResult.Conclusion)
	}
}
