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
// Created: 2026/07/26

package app

import (
	"fmt"
	"log/slog"
	"os"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/skillread"
)

// WireSkills builds the skills loader and shared catalog, registers the
// read_skill tool, and returns the prompt option carrying the catalog
// provider. It is the single wiring point shared by the headless (loom run)
// and TUI entry paths so both behave identically.
//
// Skills are disabled (no tool, no prompt section; nil option returned) when
// LOOM_SKILLS=0, or when LOOM_DISABLE_SYSTEM_PROMPT=1 — a read_skill tool
// without a visible catalog would only mislead the model. getenv is injected
// for testability (pass os.Getenv in production).
func WireSkills(
	registry *agent.ToolRegistry,
	workspaceRoot string,
	contextWindow int64,
	getenv func(string) string,
	logger *slog.Logger,
) (prompt.Option, error) {
	if registry == nil {
		return nil, fmt.Errorf("tool registry is required")
	}
	if getenv == nil {
		getenv = os.Getenv
	}
	if getenv("LOOM_SKILLS") == "0" || getenv("LOOM_DISABLE_SYSTEM_PROMPT") == "1" {
		return nil, nil
	}
	home, _ := os.UserHomeDir()
	loader := skill.NewLoader(workspaceRoot, home, splitExtraRoots(getenv("LOOM_SKILLS_EXTRA_ROOTS")), logger)
	catalog := &skill.AtomicCatalog{}
	readSkill, err := skillread.NewReadSkillTool(catalog)
	if err != nil {
		return nil, fmt.Errorf("read_skill: %w", err)
	}
	if err := registry.Register(readSkill); err != nil {
		return nil, fmt.Errorf("register read_skill: %w", err)
	}
	return prompt.WithSkillsProvider(skill.NewPromptProvider(loader, catalog, contextWindow)), nil
}

// splitExtraRoots parses LOOM_SKILLS_EXTRA_ROOTS (':'-separated directories).
func splitExtraRoots(raw string) []string {
	var roots []string
	for _, part := range strings.Split(raw, ":") {
		if trimmed := strings.TrimSpace(part); trimmed != "" {
			roots = append(roots, trimmed)
		}
	}
	return roots
}
