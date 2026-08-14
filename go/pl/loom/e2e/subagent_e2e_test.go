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

package e2e

import (
	"context"
	"encoding/json"
	"net/http"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
)

// subagent_e2e_test.go verifies delegate_task end to end
// (docs/SUBAGENT_DESIGN.md): the real openai provider over HTTP/SSE for
// BOTH the parent and the child loop (one mock server — the parent
// blocks inside the tool while the child runs, so the script order is
// deterministic), the real read_file tool inside the child, the real
// SQLite session store for both sessions, and the budget fold-back into
// the parent run.

// requestBody returns the raw n-th recorded request for tool-list
// assertions (requestText only flattens messages).
func (m *mockOpenAI) requestBody(t *testing.T, n int) string {
	t.Helper()
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.requests) <= n {
		t.Fatalf("mock received %d requests, want at least %d", len(m.requests), n+1)
	}
	return string(m.requests[n])
}

func TestE2EDelegateTask(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "main.go", "package main\n\nfunc main() {}\n")

	// Script order: parent delegates (1) → child explores (2) and
	// concludes (3) → parent consumes the conclusion (4).
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "delegate_task", ToolArgs: `{"task":"找到入口文件并总结其内容","focus":["main.go"]}`, UsageIn: 100, UsageOut: 30},
		{ToolName: "read_file", ToolArgs: `{"path":"main.go"}`, UsageIn: 200, UsageOut: 40},
		{Text: "结论：入口是 main.go，包含一个空的 main 函数。", UsageIn: 300, UsageOut: 50},
		{Text: "子 Agent 已确认：入口是 main.go。", UsageIn: 400, UsageOut: 60},
	})
	model := mock.provider(t)

	childRegistry, artStore := realEnv(t, ws)
	parentRegistry, _ := realEnv(t, ws)

	// Real SQLite store for both the parent and the child sessions.
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	models := &subagent.ModelSource{}
	factory := &subagent.Factory{
		Store:     store,
		Artifacts: artStore,
		Registry:  childRegistry,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}
	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	if err := parentRegistry.Register(delegateTool); err != nil {
		t.Fatalf("Register(delegate_task) error = %v", err)
	}

	run := newBudgetRun(t, domain.DefaultLimits())
	if err := store.CreateSession(context.Background(), run.SessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{
		Model:         model,
		ModelName:     "mock-model",
		ParentSession: run.SessionID,
	})

	loop := newRealLoop(run, model, parentRegistry, artStore, agent.WindowModel{})
	loop.Store = store
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	// 1. The parent consumed the child's conclusion and finished cleanly.
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("parent outcome = %s, want succeeded", run.State.Outcome)
	}
	if got := finalAssistantText(t, run); got != "子 Agent 已确认：入口是 main.go。" {
		t.Fatalf("parent final answer = %q", got)
	}

	// 2. Wire-level isolation: the child's first request carried the task
	//    (self-contained, no parent history) and a read-only tool list
	//    WITHOUT delegate_task (recursion depth 1 by construction); the
	//    parent's request had it.
	childReq := mock.requestBody(t, 1)
	if !strings.Contains(childReq, "找到入口文件并总结其内容") || !strings.Contains(childReq, "main.go") {
		t.Fatalf("child request missing the self-contained task")
	}
	if strings.Contains(childReq, `"delegate_task"`) {
		t.Fatalf("child tool list must not contain delegate_task")
	}
	if !strings.Contains(childReq, `"read_file"`) {
		t.Fatalf("child tool list must contain read_file")
	}
	if !strings.Contains(mock.requestBody(t, 0), `"delegate_task"`) {
		t.Fatalf("parent tool list must contain delegate_task")
	}
	// The child saw NO parent conversation: its request has exactly one
	// user message (the task), not the parent's prompt.
	if strings.Contains(childReq, "do the task") {
		t.Fatalf("child request leaked the parent's prompt — context isolation violated")
	}

	// 3. Budget transparency: parent-metered (100+400)/(30+60) plus the
	//    folded child consumption (200+300)/(40+50).
	if run.Usage.InputTokens != 1000 || run.Usage.OutputTokens != 180 {
		t.Fatalf("parent usage = %d/%d, want 1000/180 (parent + folded child)",
			run.Usage.InputTokens, run.Usage.OutputTokens)
	}

	// 4. The tool result in the parent transcript carries the conclusion
	//    and names the child session; the child session is independently
	//    persisted with the delegation edge as its first event.
	var childSessionID string
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			childSessionID = part.ToolResult.Metadata["child_session_id"]
			if !strings.Contains(part.ToolResult.Content[0].Text, "结论：入口是 main.go") {
				t.Fatalf("tool result missing child conclusion: %s", part.ToolResult.Content[0].Text)
			}
		}
	}
	childID, err := domain.ParseSessionID(childSessionID)
	if err != nil {
		t.Fatalf("child_session_id in tool result metadata: %q (%v)", childSessionID, err)
	}
	events, err := store.LoadEvents(context.Background(), childID, 0)
	if err != nil || len(events) == 0 {
		t.Fatalf("child session events: %v (n=%d)", err, len(events))
	}
	edge := string(events[0].Payload)
	if events[0].Type != domain.EventRunCreated ||
		!strings.Contains(edge, `"delegated":true`) ||
		!strings.Contains(edge, run.SessionID.String()) {
		t.Fatalf("first child event = %s %s, want the delegation edge naming the parent session",
			events[0].Type, edge)
	}
}

// TestE2EDelegateTaskParallel verifies that two delegate_task calls in
// ONE assistant response execute concurrently (docs/SUBAGENT_DESIGN.md
// §11): two 400ms scripted child delays must overlap, both children get
// their own isolated session/context, and results land in call order.
func TestE2EDelegateTaskParallel(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "main.go", "package main\n\nfunc main() {}\n")

	mock := newMockOpenAI(t, []mockEntry{
		{Tools: []mockToolCall{
			{Name: "delegate_task", Args: `{"task":"调研 types 目录的设计"}`},
			{Name: "delegate_task", Args: `{"task":"调研 codec 目录的设计"}`},
		}, UsageIn: 100, UsageOut: 30},
		// Serial execution would pay 800ms of scripted delay; parallel
		// pays ~400ms. Content-based matching keeps the mapping stable
		// while the children race.
		{Match: "调研 types", Text: "结论：types 是值类型系统。", UsageIn: 200, UsageOut: 40, Delay: 400 * time.Millisecond},
		{Match: "调研 codec", Text: "结论：codec 是块编解码。", UsageIn: 300, UsageOut: 50, Delay: 400 * time.Millisecond},
		{Match: "do the task", Text: "两个调研都完成了。", UsageIn: 400, UsageOut: 60},
	})
	model := mock.provider(t)

	childRegistry, artStore := realEnv(t, ws)
	parentRegistry, _ := realEnv(t, ws)
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	models := &subagent.ModelSource{}
	factory := &subagent.Factory{
		Store: store, Artifacts: artStore, Registry: childRegistry,
		Limits: domain.DefaultLimits(), Runaway: domain.DefaultRunawayConfig(), Models: models,
	}
	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	if err := parentRegistry.Register(delegateTool); err != nil {
		t.Fatalf("Register(delegate_task) error = %v", err)
	}

	run := newBudgetRun(t, domain.DefaultLimits())
	if err := store.CreateSession(context.Background(), run.SessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{Model: model, ModelName: "mock-model", ParentSession: run.SessionID})
	loop := newRealLoop(run, model, parentRegistry, artStore, agent.WindowModel{})
	loop.Store = store

	start := time.Now()
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	elapsed := time.Since(start)
	if elapsed > 700*time.Millisecond {
		t.Fatalf("two 400ms child delays took %v — executions serialized", elapsed)
	}
	if got := finalAssistantText(t, run); got != "两个调研都完成了。" {
		t.Fatalf("parent final answer = %q", got)
	}

	// Both delegations succeeded with their own conclusions, recorded in
	// call order (types first, codec second).
	var results []*domain.ToolResult
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				results = append(results, part.ToolResult)
			}
		}
	}
	if len(results) != 2 {
		t.Fatalf("delegate results = %d, want 2", len(results))
	}
	if !strings.Contains(results[0].Content[0].Text, "结论：types") ||
		!strings.Contains(results[1].Content[0].Text, "结论：codec") {
		t.Fatalf("results out of call order or wrong conclusions: %q | %q",
			results[0].Content[0].Text, results[1].Content[0].Text)
	}
	if results[0].Metadata["child_session_id"] == results[1].Metadata["child_session_id"] {
		t.Fatal("parallel children must get distinct sessions")
	}

	// Context isolation holds under parallelism: neither child saw the
	// other's task (or the parent's prompt). Child requests are identified
	// by the absence of the parent's prompt — the parent's final request
	// legitimately carries BOTH task strings in its own tool-call history.
	var typesReq, codecReq string
	for i := 1; i < 4; i++ {
		body := mock.requestBody(t, i)
		if strings.Contains(body, "do the task") {
			continue // a parent request, not a child
		}
		if strings.Contains(body, "调研 types") {
			typesReq = body
		}
		if strings.Contains(body, "调研 codec") {
			codecReq = body
		}
	}
	if typesReq == "" || codecReq == "" {
		t.Fatal("both child requests must reach the mock")
	}
	if strings.Contains(typesReq, "调研 codec") || strings.Contains(codecReq, "调研 types") {
		t.Fatal("parallel children saw each other's task — context isolation violated")
	}
	if strings.Contains(typesReq, "do the task") || strings.Contains(codecReq, "do the task") {
		t.Fatal("child request leaked the parent prompt")
	}
}

// TestE2EDelegateTaskObservability verifies the frontend observability
// path (docs/SUBAGENT_DESIGN.md §10): the observer bridge publishes
// started/finished runtime events on the PARENT session's envelope, and
// the controller's SubagentView projects the child checkpoint for the
// read-only drill-in.
func TestE2EDelegateTaskObservability(t *testing.T) {
	ws := t.TempDir()
	writeFile(t, ws, "main.go", "package main\n\nfunc main() {}\n")

	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "delegate_task", ToolArgs: `{"task":"总结 main.go"}`, UsageIn: 100, UsageOut: 30},
		{Text: "结论：main.go 是入口。", UsageIn: 200, UsageOut: 40},
		{Text: "已确认。", UsageIn: 300, UsageOut: 50},
	})
	model := mock.provider(t)

	childRegistry, artStore := realEnv(t, ws)
	parentRegistry, _ := realEnv(t, ws)
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(ws, "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore() error = %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })

	broker := runtimeevent.NewBroker()
	t.Cleanup(broker.Close)
	events, unsubscribe := broker.Subscribe()
	t.Cleanup(unsubscribe)

	models := &subagent.ModelSource{}
	factory := &subagent.Factory{
		Store:     store,
		Artifacts: artStore,
		Registry:  childRegistry,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}
	app.WireSubagentObserver(factory, broker, store, nil)
	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	if err := parentRegistry.Register(delegateTool); err != nil {
		t.Fatalf("Register(delegate_task) error = %v", err)
	}

	run := newBudgetRun(t, domain.DefaultLimits())
	if err := store.CreateSession(context.Background(), run.SessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession() error = %v", err)
	}
	models.Set(subagent.ModelSnapshot{Model: model, ModelName: "mock-model", ParentSession: run.SessionID})
	loop := newRealLoop(run, model, parentRegistry, artStore, agent.WindowModel{})
	loop.Store = store
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}

	// The bridge emitted started + finished on the parent session's
	// envelope, bound to the delegate call via call_id.
	var startedPayload *runtimeevent.SubagentStartedPayload
	var finishedPayload *runtimeevent.SubagentFinishedPayload
	for {
		select {
		case evt := <-events:
			if evt.SessionID != run.SessionID {
				t.Fatalf("event %s leaked onto session %s, want the parent envelope", evt.Kind, evt.SessionID)
			}
			switch evt.Kind {
			case runtimeevent.KindSubagentStarted:
				var p runtimeevent.SubagentStartedPayload
				if err := json.Unmarshal(evt.Payload, &p); err == nil {
					startedPayload = &p
				}
			case runtimeevent.KindSubagentFinished:
				var p runtimeevent.SubagentFinishedPayload
				if err := json.Unmarshal(evt.Payload, &p); err == nil {
					finishedPayload = &p
				}
			}
		default:
			goto drained
		}
	}
drained:
	if startedPayload == nil || finishedPayload == nil {
		t.Fatalf("started = %+v, finished = %+v — bridge events missing", startedPayload, finishedPayload)
	}
	if startedPayload.CallID != finishedPayload.CallID ||
		startedPayload.ChildSessionID != finishedPayload.ChildSessionID {
		t.Fatalf("started/finished not bound to the same delegation: %+v vs %+v", startedPayload, finishedPayload)
	}
	if finishedPayload.Outcome != "succeeded" || finishedPayload.InputTokens != 200 || finishedPayload.OutputTokens != 40 {
		t.Fatalf("finished payload = %+v", finishedPayload)
	}

	// The drill-in projection reads the child checkpoint directly.
	controller := app.NewController(app.ControllerConfig{Bootstrap: &app.Bootstrap{ProcessRuntime: &app.ProcessRuntime{Store: store}}})
	view, err := controller.SubagentView(context.Background(), startedPayload.ChildSessionID)
	if err != nil {
		t.Fatalf("SubagentView() error = %v", err)
	}
	if view.Active || view.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("view = %+v, want terminal succeeded", view)
	}
	if len(view.Messages) == 0 {
		t.Fatal("view must carry the child transcript")
	}
}

// TestE2EDelegateTaskChildFailure verifies the failure path: the
// child's model errors out, the parent receives a non-retryable tool
// error naming the child session, and the parent loop survives to
// finish the turn itself.
func TestE2EDelegateTaskChildFailure(t *testing.T) {
	ws := t.TempDir()
	// Script order: parent delegates (1) → the child's first model call
	// hits the injected provider error (2) → the parent survives and
	// finishes the turn itself (3).
	mock := newMockOpenAI(t, []mockEntry{
		{ToolName: "delegate_task", ToolArgs: `{"task":"研究 X"}`, UsageIn: 100, UsageOut: 30},
		// The child failure must be non-retryable: a retryable 5xx sends the
		// child loop into its bounded wait-and-retry, which would consume
		// the parent's scripted entries below.
		{Fail: true, FailStatus: http.StatusBadRequest},
		{Text: "子 Agent 不可用，我直接自己查。", UsageIn: 50, UsageOut: 20},
	})
	model := mock.provider(t)

	childRegistry, artStore := realEnv(t, ws)
	parentRegistry, _ := realEnv(t, ws)
	models := &subagent.ModelSource{}
	factory := &subagent.Factory{
		Store:     fakes.NewFakeStore(),
		Artifacts: artStore,
		Registry:  childRegistry,
		Limits:    domain.DefaultLimits(),
		Runaway:   domain.DefaultRunawayConfig(),
		Models:    models,
	}
	delegateTool, err := subagent.NewDelegateTaskTool(factory)
	if err != nil {
		t.Fatalf("NewDelegateTaskTool() error = %v", err)
	}
	if err := parentRegistry.Register(delegateTool); err != nil {
		t.Fatalf("Register(delegate_task) error = %v", err)
	}

	run := newBudgetRun(t, domain.DefaultLimits())
	models.Set(subagent.ModelSnapshot{Model: model, ModelName: "mock-model", ParentSession: run.SessionID})
	loop := newRealLoop(run, model, parentRegistry, artStore, agent.WindowModel{})

	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v — the parent must survive a child failure", err)
	}
	if got := finalAssistantText(t, run); got != "子 Agent 不可用，我直接自己查。" {
		t.Fatalf("parent final answer = %q", got)
	}
	// The delegate tool result is an error naming the child session.
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			res := part.ToolResult
			if res.Status != domain.ToolStatusError || res.Error == nil || res.Error.Retryable {
				t.Fatalf("delegate result = %+v, want non-retryable error", res)
			}
			if res.Metadata["child_session_id"] == "" {
				t.Fatalf("failed delegation must still name the child session")
			}
		}
	}
}
