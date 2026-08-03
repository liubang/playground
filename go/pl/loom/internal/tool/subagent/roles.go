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
	"encoding/json"
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Role names the behavioral contract of a delegated sub-agent: its tool
// set, system prompt, and the risk tier a spawn carries. The role travels
// on the child session's delegation edge so a resume after a process
// restart rebuilds the same contract.
type Role string

const (
	// RoleResearcher is the read-only explorer: it cannot modify files,
	// run commands, ask questions, or delegate further (the V1 contract).
	RoleResearcher Role = "researcher"
	// RoleCoder is the writable worker: it edits files and runs sandboxed
	// commands inside its own isolated context. Spawning one is an R3
	// approval because its writes land in the real workspace.
	RoleCoder Role = "coder"
)

// CoderInstructions is the coder role's dedicated prompt section. The
// output contract mirrors the researcher's (conclusion + evidence) but
// adds a summary of the mutations the parent must verify.
const CoderInstructions = `You are a coding sub-agent. A parent agent delegated a self-contained implementation task to you and will verify and build on your work; you see none of its conversation history.

Constraints:
- You CAN read and modify files, and run sandboxed commands (builds, tests) inside the workspace.
- You CANNOT ask questions or delegate further. Do not attempt to: your tool set makes it impossible.
- Work autonomously to completion; never end with a question or a request for clarification. If the task is ambiguous, state your interpretation and implement that.
- Stay strictly within the delegated scope: do not refactor unrelated code, reformat files you were not asked to touch, or commit to git.

Output contract (your final message is the only thing the parent agent sees):
1. Summary: what you implemented or changed, concise.
2. Files: every file you created or modified, with a one-line description of each change.
3. Verification: the commands you ran to validate the change (build, tests) and their outcomes; if you could not verify, say so explicitly.
4. Caveats: anything left incomplete, assumptions made, or risks the parent should review.

Keep the change minimal and verifiable: prefer a small correct edit over a broad clever one, and validate with the fastest relevant build/test command before finishing.`

// RoleSpec binds a role to its runtime contract: the tool registry the
// child loop may use and the prompt builder producing its system prompt.
// Both are assembled once at bootstrap and shared across every spawn.
type RoleSpec struct {
	Registry *agent.ToolRegistry
	Prompt   agent.PromptBuilder
	// Risk is the spawn-time risk tier for this role (R1 researcher,
	// R3 coder): the parent turn approves the delegation once, and the
	// child then runs autonomously inside its sandbox.
	Risk domain.RiskLevel
}

// ParseRole validates a role string from tool arguments; empty maps to
// the researcher default.
func ParseRole(raw string) (Role, error) {
	switch Role(raw) {
	case "":
		return RoleResearcher, nil
	case RoleResearcher:
		return RoleResearcher, nil
	case RoleCoder:
		return RoleCoder, nil
	default:
		return "", domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("unknown sub-agent role %q: expected \"researcher\" or \"coder\"", raw))
	}
}

// Instructions returns the role's appended prompt section.
func (r Role) Instructions() string {
	if r == RoleCoder {
		return CoderInstructions
	}
	return ResearcherInstructions
}

// riskOf returns the spawn risk tier for a role.
func riskOf(role Role) domain.RiskLevel {
	if role == RoleCoder {
		return domain.R3
	}
	return domain.R1
}

// roleOf recovers the sub-agent role from the session's event timeline by
// scanning for the run.created delegation edge. It returns the empty
// string when no delegation event is found (a non-delegated session, or
// a legacy session created before V2).
func roleOf(events []domain.Event) Role {
	for _, e := range events {
		if e.Type != domain.EventRunCreated {
			continue
		}
		var payload struct {
			Delegated bool   `json:"delegated"`
			Role      string `json:"role"`
		}
		if err := json.Unmarshal(e.Payload, &payload); err != nil {
			continue
		}
		if payload.Delegated && payload.Role != "" {
			return Role(payload.Role)
		}
	}
	return ""
}
