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
// Created: 2026/07/31

package subagent

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// newTestFactory assembles a factory with a scripted child model and a
// read-only registry, and returns everything the assertions need.
func newTestFactory(t *testing.T, script ...fakes.ScriptEntry) (*Factory, *fakes.FakeModel, *fakes.FakeStore, *ModelSource) {
	t.Helper()
	store := fakes.NewFakeStore()
	model := fakes.NewFakeModel(script...)
	registry := agent.NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("register read_file: %v", err)
	}
	models := &ModelSource{}
	factory := &Factory{
		Store:    store,
		Registry: registry,
		Limits:   domain.DefaultLimits(),
		Runaway:  domain.DefaultRunawayConfig(),
		Models:   models,
	}
	return factory, model, store, models
}

func publishSnapshot(models *ModelSource, model domain.Model) {
	models.Set(ModelSnapshot{
		Model:         model,
		ModelName:     "fake-model",
		ParentSession: domain.NewSessionID(),
	})
}

func delegateCall(t *testing.T, tool *DelegateTaskTool, task string) domain.PreparedCall {
	t.Helper()
	args, err := json.Marshal(map[string]any{"task": task})
	if err != nil {
		t.Fatalf("marshal args: %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "delegate_task",
		Arguments: args,
	})
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	return prepared
}

func TestDelegatePrepareValidation(t *testing.T) {
	factory, _, _, _ := newTestFactory(t, fakes.ScriptEntry{Text: "unused", StopReason: domain.StopEndTurn})
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}

	for name, raw := range map[string]string{
		"empty task":    `{"task":""}`,
		"blank task":    `{"task":"   "}`,
		"missing task":  `{"focus":["a.go"]}`,
		"unknown field": `{"task":"find X","agent_type":"explorer"}`,
		"oversized":     `{"task":"` + strings.Repeat("x", maxTaskBytes+1) + `"}`,
	} {
		_, err := tool.Prepare(context.Background(), domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "delegate_task",
			Arguments: json.RawMessage(raw),
		})
		if err == nil {
			t.Fatalf("%s: expected prepare error", name)
		}
	}

	prepared := delegateCall(t, tool, "find where retries are configured")
	// The declared risk is R1 (read-only child): crash recovery must stay
	// on the read-only path.
	if prepared.Risk != domain.R1 {
		t.Fatalf("risk = %d, want R1", prepared.Risk)
	}
	if !strings.Contains(prepared.ApprovalDesc, "find where retries") {
		t.Fatalf("approval desc = %q, want task excerpt", prepared.ApprovalDesc)
	}
	if tool.Definition().Source != domain.ToolSourceSubAgent {
		t.Fatalf("source = %q, want subagent", tool.Definition().Source)
	}
}

func TestDelegateExecuteSuccess(t *testing.T) {
	factory, model, store, models := newTestFactory(t,
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"main.go"}`)},
			},
			StopReason: domain.StopToolUse,
			UsageIn:    100,
			UsageOut:   20,
		},
		fakes.ScriptEntry{
			Text:       "结论：入口在 cmd/loom/main.go。",
			StopReason: domain.StopEndTurn,
			UsageIn:    200,
			UsageOut:   30,
		},
	)
	publishSnapshot(models, model)
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}

	result := tool.Execute(context.Background(), delegateCall(t, tool, "找到 loom 的入口"))
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, err = %+v", result.Status, result.Error)
	}
	content := result.Content[0].Text
	if !strings.Contains(content, "结论：入口在 cmd/loom/main.go。") {
		t.Fatalf("content missing conclusion: %s", content)
	}

	// Metadata: child session reference + fold-back usage (100+200 in,
	// 20+30 out from the two scripted calls).
	childID, err := domain.ParseSessionID(result.Metadata["child_session_id"])
	if err != nil {
		t.Fatalf("child_session_id metadata: %v", err)
	}
	if result.Metadata[domain.ToolMetaExternalInputTokens] != "300" ||
		result.Metadata[domain.ToolMetaExternalOutputTokens] != "50" {
		t.Fatalf("external usage metadata = %v", result.Metadata)
	}

	// The child session is independently persisted; its first event is
	// the delegation edge naming the parent session and the spawning call.
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: %v (n=%d)", err, len(events))
	}
	if events[0].Type != domain.EventRunCreated || !strings.Contains(string(events[0].Payload), `"delegated":true`) {
		t.Fatalf("first child event = %s %s, want delegated run.created", events[0].Type, events[0].Payload)
	}

	// The child saw the task as its user message, and its model request
	// carried only the read-only registry.
	calls := model.Calls()
	if len(calls) != 2 {
		t.Fatalf("child model calls = %d, want 2", len(calls))
	}
	last := calls[0].Messages[len(calls[0].Messages)-1]
	if last.Role != domain.RoleUser || !strings.Contains(strings.Join(last.TextParts(), ""), "找到 loom 的入口") {
		t.Fatalf("child first request missing task message: %+v", last)
	}
	for _, def := range calls[0].Tools {
		if def.Name == "delegate_task" {
			t.Fatalf("child registry must not contain delegate_task (recursion)")
		}
	}
}

func TestDelegateObserverHooks(t *testing.T) {
	factory, model, _, models := newTestFactory(t,
		fakes.ScriptEntry{Text: "结论", StopReason: domain.StopEndTurn, UsageIn: 100, UsageOut: 30},
	)
	publishSnapshot(models, model)
	var starts []ChildStart
	var finishes []ChildFinish
	factory.Observer = &Observer{
		Started:  func(s ChildStart) { starts = append(starts, s) },
		Finished: func(f ChildFinish) { finishes = append(finishes, f) },
	}
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}

	prepared := delegateCall(t, tool, "调研 loom 的预算机制")
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s", result.Status)
	}
	if len(starts) != 1 || len(finishes) != 1 {
		t.Fatalf("hooks fired %d/%d times, want 1/1", len(starts), len(finishes))
	}
	start := starts[0]
	if start.CallID != prepared.Call.ID || start.Task != "调研 loom 的预算机制" ||
		start.SessionID.String() != result.Metadata["child_session_id"] {
		t.Fatalf("start = %+v", start)
	}
	finish := finishes[0]
	if finish.CallID != prepared.Call.ID || finish.SessionID != start.SessionID ||
		finish.Outcome != domain.OutcomeSucceeded ||
		finish.Usage.InputTokens != 100 || finish.Usage.OutputTokens != 30 {
		t.Fatalf("finish = %+v", finish)
	}
}

func TestDelegateExecuteModelFailure(t *testing.T) {
	factory, model, _, models := newTestFactory(t,
		fakes.ScriptEntry{Error: "provider exploded"},
	)
	publishSnapshot(models, model)
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}

	result := tool.Execute(context.Background(), delegateCall(t, tool, "research X"))
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error", result.Status)
	}
	if result.Error == nil || result.Error.Retryable {
		t.Fatalf("error = %+v, want non-retryable", result.Error)
	}
	if result.Metadata["child_session_id"] == "" {
		t.Fatalf("failure result must still name the child session for audit")
	}
}

func TestDelegateExecuteWithoutModelSelection(t *testing.T) {
	factory, _, _, _ := newTestFactory(t)
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}
	result := tool.Execute(context.Background(), delegateCall(t, tool, "research X"))
	if result.Status != domain.ToolStatusError || result.Error == nil ||
		!strings.Contains(result.Error.Message, "no model selection") {
		t.Fatalf("result = %+v, want no-model-selection error", result)
	}
}

func TestDelegateExecuteCancelledWithParentTurn(t *testing.T) {
	factory, model, _, models := newTestFactory(t,
		fakes.ScriptEntry{Text: "never reached", StopReason: domain.StopEndTurn},
	)
	publishSnapshot(models, model)
	tool, err := NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	result := tool.Execute(ctx, delegateCall(t, tool, "research X"))
	if result.Status != domain.ToolStatusCancelled {
		t.Fatalf("status = %s, want cancelled", result.Status)
	}
}

func TestChildPolicyAllowsOnlyRegisteredTools(t *testing.T) {
	registry := agent.NewToolRegistry()
	if err := registry.Register(fakes.ReadFileTool()); err != nil {
		t.Fatalf("register: %v", err)
	}
	policy := childPolicyFor(registry)

	allow := policy.Evaluate(domain.PreparedCall{
		Call: domain.ToolCall{Name: "read_file"},
		Risk: domain.R1,
	})
	if allow.Decision != domain.DecisionAllow {
		t.Fatalf("read_file decision = %s, want allow", allow.Decision)
	}

	for _, name := range []string{"edit", "run_cmd", "delegate_task"} {
		verdict := policy.Evaluate(domain.PreparedCall{
			Call: domain.ToolCall{Name: name},
			Risk: domain.R2,
		})
		if verdict.Decision != domain.DecisionDeny {
			t.Fatalf("%s decision = %s, want deny (child policy must never ask)", name, verdict.Decision)
		}
	}
}
