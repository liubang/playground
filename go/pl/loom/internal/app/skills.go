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

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/skillread"
)

// SkillsHandle exposes the skills loader and shared catalog assembled by
// WireSkills, so frontends can list and refresh the discovered skills
// (the /skill command) against the same catalog the model sees.
type SkillsHandle struct {
	Loader  *skill.Loader
	Catalog *skill.AtomicCatalog
}

// WireSkills builds the skills loader and shared catalog, registers the
// read_skill tool, and returns the prompt option carrying the catalog
// provider plus a handle to the underlying loader/catalog. It is the
// single wiring point shared by the headless (loom run) and TUI entry
// paths so both behave identically.
//
// Skills are disabled (no tool, no prompt section; nil option and nil
// handle returned) when cfg.Enabled is false or the built-in system
// prompt is disabled — a read_skill tool without a visible catalog would
// only mislead the model.
func WireSkills(
	registry *agent.ToolRegistry,
	workspaceRoot string,
	contextWindow int64,
	cfg config.ResolvedSkills,
	systemPromptDisabled bool,
	logger *slog.Logger,
) (prompt.Option, *SkillsHandle, error) {
	if registry == nil {
		return nil, nil, fmt.Errorf("tool registry is required")
	}
	if !cfg.Enabled || systemPromptDisabled {
		return nil, nil, nil
	}
	home, _ := os.UserHomeDir()
	loader := skill.NewLoader(workspaceRoot, home, cfg.ExtraRoots, logger)
	catalog := &skill.AtomicCatalog{}
	readSkill, err := skillread.NewReadSkillTool(catalog)
	if err != nil {
		return nil, nil, fmt.Errorf("read_skill: %w", err)
	}
	if err := registry.Register(readSkill); err != nil {
		return nil, nil, fmt.Errorf("register read_skill: %w", err)
	}
	opt := prompt.WithSkillsProvider(skill.NewPromptProvider(loader, catalog, contextWindow))
	return opt, &SkillsHandle{Loader: loader, Catalog: catalog}, nil
}
