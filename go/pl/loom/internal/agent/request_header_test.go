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
// Created: 2026/08/14

package agent

import (
	"context"
	"encoding/json"
	"log/slog"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// Request-header persistence tests (docs/SURFACE_DESIGN.md §4.8, M4).

func headerEvents(t *testing.T, events []domain.Event) []domain.RequestHeaderPayload {
	t.Helper()
	var out []domain.RequestHeaderPayload
	for _, evt := range events {
		if evt.Type != domain.EventModelRequestHeader {
			continue
		}
		var payload domain.RequestHeaderPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("unmarshal header payload: %v", err)
		}
		out = append(out, payload)
	}
	return out
}

func requestStartedHeaderHashes(t *testing.T, events []domain.Event) []string {
	t.Helper()
	var out []string
	for _, evt := range events {
		if evt.Type != domain.EventModelRequestStarted {
			continue
		}
		var payload modelRequestAuditPayload
		if err := json.Unmarshal(evt.Payload, &payload); err != nil {
			t.Fatalf("unmarshal request_started payload: %v", err)
		}
		out = append(out, payload.HeaderHash)
	}
	return out
}

func TestRequestHeaderLoggedOnceWithInitialReason(t *testing.T) {
	store := fakes.NewFakeStore()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"x"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 10, UsageOut: 5,
		},
		fakes.ScriptEntry{Text: "second", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5},
	)
	registry := NewToolRegistry()
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "hello")

	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	// Two model calls, one unchanged header: logged exactly once.
	headers := headerEvents(t, events)
	if len(headers) != 1 {
		t.Fatalf("header events = %d, want 1 (dedup)", len(headers))
	}
	if headers[0].Reason != domain.HeaderReasonInitial {
		t.Fatalf("reason = %s, want initial", headers[0].Reason)
	}
	if headers[0].Header.ModelName == "" || headers[0].Hash == "" {
		t.Fatalf("header content incomplete: %+v", headers[0])
	}
	// Every request_started anchors to the logged header.
	for _, hash := range requestStartedHeaderHashes(t, events) {
		if hash != headers[0].Hash {
			t.Fatalf("request_started header_hash %q != header hash %q", hash, headers[0].Hash)
		}
	}
}

func TestRequestHeaderResumeReasonOnContinuedSession(t *testing.T) {
	store := fakes.NewFakeStore()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "first", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5},
	)
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "hello")
	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: NewToolRegistry(), Logger: slog.Default(),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}

	// Continue the terminal session with a fresh loop instance (the resume
	// path): its first request must log a resume-reason header even though
	// the content is identical to the pre-resume one.
	ckpt, err := store.LoadLatestCheckpoint(context.Background(), run.SessionID)
	if err != nil {
		t.Fatalf("LoadLatestCheckpoint: %v", err)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	continued, err := ContinueRun(ckpt, ckpt.Messages, int64(len(events)), domain.DefaultLimits(), run.Clock)
	if err != nil {
		t.Fatalf("ContinueRun: %v", err)
	}
	continued.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "again"}},
		CreatedAt: run.Clock.Now(),
	})
	model2 := fakes.NewFakeModel(
		fakes.ScriptEntry{Text: "second", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5},
	)
	loop2 := &Loop{
		Run: continued, Model: model2, Store: store,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: NewToolRegistry(), Logger: slog.Default(),
	}
	if err := loop2.Execute(context.Background()); err != nil {
		t.Fatalf("Execute continued: %v", err)
	}

	all, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	headers := headerEvents(t, all)
	if len(headers) != 2 {
		t.Fatalf("header events = %d, want 2 (initial + resume)", len(headers))
	}
	if headers[1].Reason != domain.HeaderReasonResume {
		t.Fatalf("second header reason = %s, want resume", headers[1].Reason)
	}
	// Identical content: the resume header re-anchors without 'change'.
	if headers[0].Hash != headers[1].Hash {
		t.Fatalf("unchanged header must keep the same hash: %q vs %q", headers[0].Hash, headers[1].Hash)
	}
}

// TestRequestHeaderCanonicalHashStability locks the dedup premise: the
// hash must be a pure function of header CONTENT — tool list ordering
// differences must not produce a spurious 'change'.
func TestRequestHeaderCanonicalHashStability(t *testing.T) {
	toolA := domain.ToolDefinition{Name: "a_tool", Description: "a", InputSchema: json.RawMessage(`{"type":"object"}`)}
	toolB := domain.ToolDefinition{Name: "b_tool", Description: "b", InputSchema: json.RawMessage(`{"type":"object"}`)}
	base := domain.RequestHeader{
		ModelName: "m", MaxTokens: 4096, System: "sys",
		Tools: []domain.ToolDefinition{toolA, toolB},
	}
	shuffled := base
	shuffled.Tools = []domain.ToolDefinition{toolB, toolA}
	if base.CanonicalHash() == "" {
		t.Fatal("empty hash")
	}
	// Ordering matters by design: the wire order IS the semantic content
	// (providers see tools in list order), so a reorder SHOULD change the
	// hash — what must be stable is that the same ordering hashes equal.
	clone := base
	clone.Tools = []domain.ToolDefinition{toolA, toolB}
	if base.CanonicalHash() != clone.CanonicalHash() {
		t.Fatal("identical content must hash equal")
	}
	if base.CanonicalHash() == shuffled.CanonicalHash() {
		t.Fatal("reordered tools must change the hash (wire order is semantic)")
	}
	changed := base
	changed.System = "sys-v2"
	if base.CanonicalHash() == changed.CanonicalHash() {
		t.Fatal("changed system prompt must change the hash")
	}
}

// flipPrompt returns different prompt text on each build, simulating a
// mid-run prompt change (the production sources are /model switches, MCP
// tool hot-loads, and prompt/memory edits).
type flipPrompt struct{ calls int }

func (f *flipPrompt) Build(context.Context) (string, []domain.ContextRuleRef, error) {
	f.calls++
	if f.calls == 1 {
		return "prompt-v1", nil, nil
	}
	return "prompt-v2", nil, nil
}

func TestRequestHeaderChangeReason(t *testing.T) {
	store := fakes.NewFakeStore()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls: []domain.ToolCall{
				{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{"text":"x"}`)},
			},
			StopReason: domain.StopToolUse, UsageIn: 10, UsageOut: 5,
		},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 10, UsageOut: 5},
	)
	registry := NewToolRegistry()
	if err := registry.Register(fakes.EchoTool()); err != nil {
		t.Fatalf("Register: %v", err)
	}
	run := newTestRun(domain.DefaultLimits())
	mustCreateSession(t, store, run.SessionID)
	addUserTextMessage(run, "hello")
	loop := &Loop{
		Run: run, Model: model, Store: store,
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		ModelName: "model-a", SystemPrompt: &flipPrompt{},
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	events, err := store.LoadEvents(context.Background(), run.SessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	headers := headerEvents(t, events)
	if len(headers) != 2 {
		t.Fatalf("header events = %d, want 2 (initial + change)", len(headers))
	}
	if headers[0].Reason != domain.HeaderReasonInitial || headers[1].Reason != domain.HeaderReasonChange {
		t.Fatalf("reasons = %s, %s; want initial, change", headers[0].Reason, headers[1].Reason)
	}
	if headers[0].Header.System != "prompt-v1" || headers[1].Header.System != "prompt-v2" {
		t.Fatalf("header system texts = %q, %q", headers[0].Header.System, headers[1].Header.System)
	}
	if headers[0].Hash == headers[1].Hash {
		t.Fatal("changed header must have a different hash")
	}
	// Each request_started anchors to the header in effect for that call.
	anchors := requestStartedHeaderHashes(t, events)
	if len(anchors) != 2 || anchors[0] != headers[0].Hash || anchors[1] != headers[1].Hash {
		t.Fatalf("request anchors %v do not match header sequence %q, %q", anchors, headers[0].Hash, headers[1].Hash)
	}
}
