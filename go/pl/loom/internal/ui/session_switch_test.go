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

package ui

import (
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Regression: switching sessions must reset every per-session UI state
// synchronously. A leftover busy phase kept the spinner ticking and the
// activity timer redrawing every frame (screen flicker), and a stale
// scroll/follow position stranded the fresh transcript above its tail so
// it could never be scrolled back to the bottom.
func TestSessionSwitchResetsPerSessionUIState(t *testing.T) {
	m := NewModel(newTestController(t), "test-model", "/ws")
	m.width, m.height = 80, 24
	m.layout()

	// Simulate a busy session with leftover view state.
	m.phase = "model"
	m.activityLabel = "Streaming response"
	m.lastActivityAt = time.Now()
	m.spinning = true
	m.plan = domain.Plan{Items: []domain.PlanItem{
		{Index: 0, Goal: "read code", Status: domain.PlanItemDone},
		{Index: 1, Goal: "implement", Status: domain.PlanItemInProgress},
	}}
	m.planHidden = true
	m.compactions = 3
	m.contextOccupancy = 12345
	m.pauseFollowTail()
	m.newEvents = 7

	updated, cmd := m.handleSessionSwitched(sessionSwitchedMsg{
		action: sessionAction{name: "Resume", success: "Session resumed"},
	})
	m = updated.(Model)

	if cmd == nil {
		t.Fatal("session switch must reattach the event stream")
	}
	if m.phase != "idle" {
		t.Fatalf("phase = %q, want idle", m.phase)
	}
	if m.isBusy() {
		t.Fatalf("switched session must not be busy (phase=%q)", m.phase)
	}
	if m.activityLabel != "" {
		t.Fatalf("activity label leaked: %q", m.activityLabel)
	}
	if !m.lastActivityAt.IsZero() {
		t.Fatalf("last activity time leaked: %v", m.lastActivityAt)
	}
	if m.spinning {
		t.Fatal("spinner still ticking after switch")
	}
	if len(m.plan.Items) != 0 {
		t.Fatalf("plan leaked: %+v", m.plan)
	}
	if m.planHidden {
		t.Fatal("plan-hidden toggle leaked")
	}
	if m.compactions != 0 {
		t.Fatalf("compaction count leaked: %d", m.compactions)
	}
	if m.contextOccupancy != 0 {
		t.Fatalf("context occupancy leaked: %d", m.contextOccupancy)
	}
	if !m.followTail {
		t.Fatal("follow-tail must re-engage on switch")
	}
	if m.newEvents != 0 {
		t.Fatalf("new-events count leaked: %d", m.newEvents)
	}
}
