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

package app

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
)

// recvEvent waits for one event of the given kind (skipping others).
func recvEvent(t *testing.T, ch <-chan runtimeevent.RuntimeEvent, kind runtimeevent.RuntimeEventKind) runtimeevent.RuntimeEvent {
	t.Helper()
	deadline := time.After(3 * time.Second)
	for {
		select {
		case evt := <-ch:
			if evt.Kind == kind {
				return evt
			}
		case <-deadline:
			t.Fatalf("timed out waiting for %s", kind)
		}
	}
}

func TestSubagentBridgePublishesLifecycleAndProgress(t *testing.T) {
	broker := runtimeevent.NewBroker()
	t.Cleanup(broker.Close)
	events, unsubscribe := broker.Subscribe()
	t.Cleanup(unsubscribe)

	store := fakes.NewFakeStore()
	bridge := &subagentBridge{
		broker: broker, store: store, interval: 5 * time.Millisecond,
		stops: make(map[domain.ToolCallID]chan struct{}),
	}

	parent := domain.NewSessionID()
	info := subagent.ChildStart{
		CallID:        domain.NewToolCallID(),
		SessionID:     domain.NewSessionID(),
		ParentSession: parent,
		Task:          "research X",
		StartedAt:     time.Now(),
	}
	bridge.started(info)

	started := recvEvent(t, events, runtimeevent.KindSubagentStarted)
	if started.SessionID != parent {
		t.Fatalf("started envelope session = %s, want the PARENT session (UI filter)", started.SessionID)
	}
	var startedPayload runtimeevent.SubagentStartedPayload
	if err := json.Unmarshal(started.Payload, &startedPayload); err != nil {
		t.Fatalf("started payload: %v", err)
	}
	if startedPayload.ChildSessionID != info.SessionID || startedPayload.CallID != info.CallID ||
		startedPayload.Task != "research X" {
		t.Fatalf("started payload = %+v", startedPayload)
	}

	// Once the child has a checkpoint, the ticker republishes its counters.
	if err := store.CreateSession(context.Background(), info.SessionID); err != nil {
		t.Fatal(err)
	}
	if err := store.SaveCheckpoint(context.Background(), domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: info.SessionID,
		Sequence:  1,
		State:     domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhaseExecutingTools},
		Usage:     domain.Usage{ToolCalls: 4, InputTokens: 9_000, OutputTokens: 1_500},
		CreatedAt: time.Now(),
	}); err != nil {
		t.Fatal(err)
	}
	progress := recvEvent(t, events, runtimeevent.KindSubagentProgress)
	var progressPayload runtimeevent.SubagentProgressPayload
	if err := json.Unmarshal(progress.Payload, &progressPayload); err != nil {
		t.Fatalf("progress payload: %v", err)
	}
	if progressPayload.ToolCalls != 4 || progressPayload.InputTokens != 9_000 || progressPayload.OutputTokens != 1_500 {
		t.Fatalf("progress payload = %+v", progressPayload)
	}

	bridge.finished(subagent.ChildFinish{
		CallID:        info.CallID,
		SessionID:     info.SessionID,
		ParentSession: parent,
		Outcome:       domain.OutcomeSucceeded,
		Usage:         domain.Usage{ToolCalls: 6, InputTokens: 12_000, OutputTokens: 2_000},
	})
	finished := recvEvent(t, events, runtimeevent.KindSubagentFinished)
	var finishedPayload runtimeevent.SubagentFinishedPayload
	if err := json.Unmarshal(finished.Payload, &finishedPayload); err != nil {
		t.Fatalf("finished payload: %v", err)
	}
	if finishedPayload.Outcome != "succeeded" || finishedPayload.ToolCalls != 6 {
		t.Fatalf("finished payload = %+v", finishedPayload)
	}

	// The ticker is stopped: no further progress events arrive.
	bridge.mu.Lock()
	remaining := len(bridge.stops)
	bridge.mu.Unlock()
	if remaining != 0 {
		t.Fatalf("ticker map not drained after finish: %d", remaining)
	}
	select {
	case evt := <-events:
		if evt.Kind == runtimeevent.KindSubagentProgress {
			t.Fatal("progress event after finish — ticker still running")
		}
	case <-time.After(50 * time.Millisecond):
	}
}

func TestWireSubagentObserverNilSafe(t *testing.T) {
	// No factory / no broker: a no-op, not a panic.
	WireSubagentObserver(nil, runtimeevent.NewBroker(), fakes.NewFakeStore(), nil)
	WireSubagentObserver(&subagent.Factory{}, nil, fakes.NewFakeStore(), nil)
	factory := &subagent.Factory{}
	WireSubagentObserver(factory, runtimeevent.NewBroker(), fakes.NewFakeStore(), nil)
	if factory.Observer == nil {
		t.Fatal("observer must be installed when factory, broker and store are present")
	}
}

func TestControllerSubagentView(t *testing.T) {
	store := fakes.NewFakeStore()
	childID := domain.NewSessionID()
	if err := store.CreateSession(context.Background(), childID); err != nil {
		t.Fatal(err)
	}
	if err := store.SaveCheckpoint(context.Background(), domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: childID,
		Sequence:  3,
		State:     domain.RunState{Lifecycle: domain.LifecycleTerminal, Phase: domain.PhasePreparing, Outcome: domain.OutcomeSucceeded},
		Messages: []domain.Message{{
			ID: domain.NewMessageID(), Role: domain.RoleAssistant,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "conclusion"}},
			CreatedAt: time.Now(),
		}},
		Usage:     domain.Usage{ToolCalls: 3, InputTokens: 5_000},
		CreatedAt: time.Now(),
	}); err != nil {
		t.Fatal(err)
	}

	controller := NewController(ControllerConfig{Bootstrap: &Bootstrap{Store: store}})
	view, err := controller.SubagentView(context.Background(), childID)
	if err != nil {
		t.Fatalf("SubagentView: %v", err)
	}
	if view.Active {
		t.Fatal("terminal child must not be active")
	}
	if view.Outcome != domain.OutcomeSucceeded || view.Usage.ToolCalls != 3 || len(view.Messages) != 1 {
		t.Fatalf("view = %+v", view)
	}

	if _, err := controller.SubagentView(context.Background(), domain.SessionID{}); err == nil {
		t.Fatal("zero session ID must be rejected")
	}
	if _, err := controller.SubagentView(context.Background(), domain.NewSessionID()); err == nil {
		t.Fatal("unknown session must surface the checkpoint load error")
	}
}
