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

package app

import (
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// SessionRuntime bundles every piece of per-session mutable state that the
// agent loop and its tools touch (docs/SERVE_DESIGN.md §4.2). A serve
// process gives each session its own SessionRuntime so concurrent sessions
// never share goal/plan/steer queues or ask_user answer channels.
//
// Registry is an overlay on the bootstrap's base registry: the three
// session-state tools (update_goal/update_plan/ask_user) are shadowed with
// instances bound to this runtime's cells/questioner, while all stateless
// tools fall through to the shared base registry.
type SessionRuntime struct {
	GoalCell  *agent.GoalCell
	PlanCell  *agent.PlanCell
	SteerCell *agent.SteerCell
	// Questioner is the session's interactive ask_user channel; nil when
	// the session resolves questions non-interactively (e.g. the bootstrap's
	// AutonomousQuestioner on headless paths).
	Questioner *ChannelQuestioner
	Registry   *agent.ToolRegistry
}

// NewSessionRuntime builds a SessionRuntime on top of the bootstrap,
// reusing the bootstrap's cells when it carries them (the legacy
// single-session TUI/headless assembly, so existing behavior is preserved
// exactly). questioner, when non-nil, becomes the session's ask_user
// channel; otherwise questions resolve through the bootstrap's questioner
// (AutonomousQuestioner headless) and Questioner stays nil.
func NewSessionRuntime(b *Bootstrap, questioner *ChannelQuestioner) (*SessionRuntime, error) {
	return newSessionRuntime(b, questioner, true)
}

// NewIsolatedSessionRuntime builds a SessionRuntime with fresh
// cells even when the bootstrap carries its own — the multi-session
// construction used by serve's SessionService, where no session state may
// be shared between sessions (docs/SERVE_DESIGN.md §4.2).
func NewIsolatedSessionRuntime(b *Bootstrap, questioner *ChannelQuestioner) (*SessionRuntime, error) {
	return newSessionRuntime(b, questioner, false)
}

func newSessionRuntime(b *Bootstrap, questioner *ChannelQuestioner, reuseCells bool) (*SessionRuntime, error) {
	rt := &SessionRuntime{Questioner: questioner}
	base := (*agent.ToolRegistry)(nil)
	if b != nil {
		if reuseCells {
			rt.GoalCell = b.GoalCell
			rt.PlanCell = b.PlanCell
			rt.SteerCell = b.SteerCell
		}
		base = b.Registry
	}
	if rt.GoalCell == nil {
		rt.GoalCell = agent.NewGoalCell()
	}
	if rt.PlanCell == nil {
		rt.PlanCell = agent.NewPlanCell()
	}
	if rt.SteerCell == nil {
		rt.SteerCell = agent.NewSteerCell()
	}
	if base == nil {
		base = agent.NewToolRegistry()
	}

	// Resolve the effective ask_user answer source: the explicit session
	// channel wins; otherwise the bootstrap's questioner (AutonomousQuestioner
	// when headless) keeps resolving as before.
	var askSource domain.Questioner
	switch {
	case questioner != nil:
		askSource = questioner
	case b != nil && b.Questioner != nil:
		askSource = b.Questioner
	default:
		askSource = domain.AutonomousQuestioner{}
	}

	overlay := agent.NewOverlayRegistry(base)
	updateGoal, err := agent.NewUpdateGoalTool(rt.GoalCell)
	if err != nil {
		return nil, fmt.Errorf("session runtime update_goal: %w", err)
	}
	if err := overlay.Register(updateGoal); err != nil {
		return nil, fmt.Errorf("session runtime register update_goal: %w", err)
	}
	updatePlan, err := agent.NewUpdatePlanTool(rt.PlanCell)
	if err != nil {
		return nil, fmt.Errorf("session runtime update_plan: %w", err)
	}
	if err := overlay.Register(updatePlan); err != nil {
		return nil, fmt.Errorf("session runtime register update_plan: %w", err)
	}
	askUser, err := agent.NewAskUserTool(askSource)
	if err != nil {
		return nil, fmt.Errorf("session runtime ask_user: %w", err)
	}
	if err := overlay.Register(askUser); err != nil {
		return nil, fmt.Errorf("session runtime register ask_user: %w", err)
	}
	rt.Registry = overlay
	return rt, nil
}
