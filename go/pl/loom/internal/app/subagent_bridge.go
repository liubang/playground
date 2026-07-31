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
	"log/slog"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
)

// subagentProgressInterval is how often the bridge pulls the running
// child's checkpoint and republishes its counters.
const subagentProgressInterval = time.Second

// WireSubagentObserver bridges delegate_task child-run lifecycle onto the
// runtime event stream (docs/SUBAGENT_DESIGN.md §10): started/finished
// events fire from the observer hooks, and a per-child ticker republishes
// progress counters pulled from the child checkpoint — the pull model
// that keeps the child loop itself free of any event-stream coupling
// (design D6). Events ride the PARENT session's envelope so frontends
// receive them through their existing subscription filter.
//
// Nil factory or broker is a no-op (headless runs have no broker).
// Progress polling runs on the bridge's own goroutines: observer hooks
// fire synchronously inside the delegate tool's Execute and must stay
// cheap and non-blocking.
func WireSubagentObserver(factory *subagent.Factory, broker *runtimeevent.Broker, store domain.SessionStore, logger *slog.Logger) {
	if factory == nil || broker == nil || store == nil {
		return
	}
	b := &subagentBridge{
		broker:   broker,
		store:    store,
		logger:   logger,
		interval: subagentProgressInterval,
		stops:    make(map[domain.ToolCallID]chan struct{}),
	}
	factory.Observer = &subagent.Observer{Started: b.started, Finished: b.finished}
}

// subagentBridge tracks one progress ticker per in-flight delegation.
type subagentBridge struct {
	broker   *runtimeevent.Broker
	store    domain.SessionStore
	logger   *slog.Logger
	interval time.Duration

	mu    sync.Mutex
	stops map[domain.ToolCallID]chan struct{}
}

func (b *subagentBridge) started(info subagent.ChildStart) {
	b.publish(info.ParentSession, runtimeevent.KindSubagentStarted, runtimeevent.SubagentStartedPayload{
		CallID:         info.CallID,
		ChildSessionID: info.SessionID,
		Task:           info.Task,
	})
	stop := make(chan struct{})
	b.mu.Lock()
	b.stops[info.CallID] = stop
	b.mu.Unlock()
	go b.pollProgress(info, stop)
}

func (b *subagentBridge) finished(info subagent.ChildFinish) {
	b.mu.Lock()
	if stop, ok := b.stops[info.CallID]; ok {
		close(stop)
		delete(b.stops, info.CallID)
	}
	b.mu.Unlock()
	b.publish(info.ParentSession, runtimeevent.KindSubagentFinished, runtimeevent.SubagentFinishedPayload{
		CallID:         info.CallID,
		ChildSessionID: info.SessionID,
		Outcome:        string(info.Outcome),
		ToolCalls:      info.Usage.ToolCalls,
		InputTokens:    info.Usage.InputTokens,
		OutputTokens:   info.Usage.OutputTokens,
	})
}

// pollProgress republishes the child's checkpoint counters until stopped.
// Early ticks routinely find no checkpoint yet (the child flushes after
// its first model call); those are skipped silently.
func (b *subagentBridge) pollProgress(info subagent.ChildStart, stop chan struct{}) {
	ticker := time.NewTicker(b.interval)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
		}
		checkpoint, err := b.store.LoadLatestCheckpoint(context.Background(), info.SessionID)
		if err != nil {
			continue
		}
		b.publish(info.ParentSession, runtimeevent.KindSubagentProgress, runtimeevent.SubagentProgressPayload{
			CallID:         info.CallID,
			ChildSessionID: info.SessionID,
			ToolCalls:      checkpoint.Usage.ToolCalls,
			InputTokens:    checkpoint.Usage.InputTokens,
			OutputTokens:   checkpoint.Usage.OutputTokens,
			ElapsedMs:      time.Since(info.StartedAt).Milliseconds(),
		})
	}
}

func (b *subagentBridge) publish(parentSession domain.SessionID, kind runtimeevent.RuntimeEventKind, payload any) {
	if parentSession.IsZero() {
		return
	}
	if err := b.broker.PublishEphemeral(parentSession, domain.RunID{}, 0, kind, payload); err != nil && b.logger != nil {
		b.logger.Debug("subagent bridge publish failed", "kind", kind, "error", err)
	}
}
