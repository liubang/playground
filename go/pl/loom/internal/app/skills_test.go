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
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
)

func TestWireSkillsDisabled(t *testing.T) {
	for _, tc := range []struct {
		name                 string
		cfg                  config.ResolvedSkills
		systemPromptDisabled bool
	}{
		{"skills disabled", config.ResolvedSkills{Enabled: false}, false},
		{"system prompt disabled", config.ResolvedSkills{Enabled: true}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			registry := agent.NewToolRegistry()
			opt, err := WireSkills(registry, t.TempDir(), 0, tc.cfg, tc.systemPromptDisabled, nil)
			if err != nil {
				t.Fatalf("WireSkills() error = %v", err)
			}
			if opt != nil {
				t.Fatal("WireSkills() option = non-nil, want nil when disabled")
			}
			if _, ok := registry.Lookup("read_skill"); ok {
				t.Fatal("read_skill registered while skills are disabled")
			}
		})
	}
}

func TestWireSkillsEnabledEndToEnd(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home) // os.UserHomeDir reads $HOME on unix
	ws := t.TempDir()
	skillDir := filepath.Join(ws, ".loom", "skills", "demo-skill")
	if err := os.MkdirAll(skillDir, 0o755); err != nil {
		t.Fatal(err)
	}
	body := "---\nname: demo-skill\ndescription: e2e wiring check\n---\n"
	if err := os.WriteFile(filepath.Join(skillDir, "SKILL.md"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}

	registry := agent.NewToolRegistry()
	opt, err := WireSkills(registry, ws, 0, config.ResolvedSkills{Enabled: true}, false, nil)
	if err != nil {
		t.Fatalf("WireSkills() error = %v", err)
	}
	if opt == nil {
		t.Fatal("WireSkills() option = nil, want skills provider option")
	}
	if _, ok := registry.Lookup("read_skill"); !ok {
		t.Fatal("read_skill not registered")
	}

	// Applying the option must make the catalog visible in the system prompt.
	b := prompt.NewBuilder(ws, opt)
	text, rules, err := b.Build(context.Background())
	if err != nil {
		t.Fatalf("Build() error = %v", err)
	}
	if !strings.Contains(text, "demo-skill") || !strings.Contains(text, "可用技能") {
		t.Fatalf("skills section missing from built prompt")
	}
	found := false
	for _, r := range rules {
		if r.Source == "loom://skills/catalog" {
			found = true
		}
	}
	if !found {
		t.Fatal("loom://skills/catalog audit ref missing")
	}
}
