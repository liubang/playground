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

package skill

import (
	"context"
	"testing"
)

func TestCatalogQueries(t *testing.T) {
	cat := newCatalog(
		[]*Skill{
			{Name: "zeta", Description: "d", Path: "/u/zeta/SKILL.md", Dir: "/u/zeta", Scope: ScopeUser},
			{Name: "alpha", Description: "d", Path: "/r/alpha/SKILL.md", Dir: "/r/alpha", Scope: ScopeRepo},
			{Name: "beta", Description: "d", Path: "/u/beta/SKILL.md", Dir: "/u/beta", Scope: ScopeUser},
		},
		nil,
	)
	if got := cat.Names(); len(got) != 3 || got[0] != "alpha" || got[1] != "beta" || got[2] != "zeta" {
		t.Fatalf("Names() = %v, want [alpha beta zeta] (scope then name)", got)
	}
	if cat.Find("alpha") == nil || cat.Find("missing") != nil {
		t.Fatal("Find() mismatch")
	}
}

func TestCatalogBuilderTotalLimit(t *testing.T) {
	b := &catalogBuilder{byName: map[string]*Skill{}}
	for i := 0; i < MaxSkillsTotal+10; i++ {
		b.add(&Skill{Name: string(rune('a'+i%26)) + string(rune('A'+i/26)) + string(rune('0'+i%10)), Description: "d"})
	}
	if len(b.skills) != MaxSkillsTotal {
		t.Fatalf("skills = %d, want capped at %d", len(b.skills), MaxSkillsTotal)
	}
	if len(b.issues) != 10 {
		t.Fatalf("issues = %d, want 10", len(b.issues))
	}
}

func TestAtomicCatalog(t *testing.T) {
	var atomic AtomicCatalog
	// Zero value holds an empty catalog.
	if got := atomic.Get(); got == nil || len(got.Skills()) != 0 || got.Find("x") != nil {
		t.Fatalf("zero-value Get() = %+v, want empty non-nil catalog", got)
	}
	cat := newCatalog([]*Skill{{Name: "a", Description: "d", Scope: ScopeUser}}, nil)
	atomic.Store(cat)
	if got := atomic.Get(); got.Find("a") == nil {
		t.Fatal("Get() after Store() missing skill")
	}
	atomic.Store(nil)
	if got := atomic.Get(); got == nil || len(got.Skills()) != 0 {
		t.Fatal("Store(nil) must reset to an empty catalog")
	}
}

func TestPromptProviderRefreshesSharedCatalog(t *testing.T) {
	ws := t.TempDir()
	writeSkill(t, ws+"/.loom/skills/demo", "demo", "d")
	var atomic AtomicCatalog
	provider := NewPromptProvider(NewLoader(ws, nil, nil), &atomic, 0)

	body, err := provider.Skills(context.Background())
	if err != nil {
		t.Fatalf("Skills() error = %v", err)
	}
	if body == "" {
		t.Fatal("Skills() = empty, want catalog section")
	}
	if atomic.Get().Find("demo") == nil {
		t.Fatal("shared catalog not refreshed by provider")
	}
}

func TestPromptProviderNilLoader(t *testing.T) {
	body, err := NewPromptProvider(nil, nil, 0).Skills(context.Background())
	if err != nil || body != "" {
		t.Fatalf("Skills() = %q, %v, want empty degradation", body, err)
	}
}

func TestScopeString(t *testing.T) {
	if ScopeRepo.String() != "repo" || ScopeUser.String() != "user" {
		t.Fatal("Scope.String() mismatch")
	}
}
