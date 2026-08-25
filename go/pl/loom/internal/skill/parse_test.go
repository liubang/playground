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
	"strings"
	"testing"
)

func parseOK(t *testing.T, contents string) *Skill {
	t.Helper()
	s, err := parseSkill("/skills/demo", "/skills/demo/SKILL.md", []byte(contents), ScopeUser)
	if err != nil {
		t.Fatalf("parseSkill() error = %v", err)
	}
	return s
}

func parseErr(t *testing.T, contents string) string {
	t.Helper()
	_, err := parseSkill("/skills/demo", "/skills/demo/SKILL.md", []byte(contents), ScopeUser)
	if err == nil {
		t.Fatal("parseSkill() error = nil, want error")
	}
	return err.Error()
}

func TestParseSkill(t *testing.T) {
	t.Run("valid frontmatter", func(t *testing.T) {
		s := parseOK(t, "---\nname: weather\ndescription: 查询天气与趋势数据\n---\n\n# body\n")
		if s.Name != "weather" || s.Description != "查询天气与趋势数据" {
			t.Fatalf("unexpected skill: %+v", s)
		}
		if s.Dir != "/skills/demo" || s.Path != "/skills/demo/SKILL.md" || s.Scope != ScopeUser {
			t.Fatalf("unexpected paths/scope: %+v", s)
		}
	})

	t.Run("missing name falls back to directory name", func(t *testing.T) {
		s := parseOK(t, "---\ndescription: does things\n---\n")
		if s.Name != "demo" {
			t.Fatalf("Name = %q, want demo (directory name)", s.Name)
		}
	})

	t.Run("missing description is an error", func(t *testing.T) {
		if msg := parseErr(t, "---\nname: x\n---\n"); !strings.Contains(msg, "description") {
			t.Fatalf("error = %q, want mention of description", msg)
		}
	})

	t.Run("empty description is an error", func(t *testing.T) {
		parseErr(t, "---\nname: x\ndescription: ''\n---\n")
	})

	t.Run("missing frontmatter", func(t *testing.T) {
		if msg := parseErr(t, "# no frontmatter\n"); !strings.Contains(msg, "frontmatter") {
			t.Fatalf("error = %q, want mention of frontmatter", msg)
		}
	})

	t.Run("unclosed frontmatter", func(t *testing.T) {
		parseErr(t, "---\nname: x\ndescription: y\n")
	})

	t.Run("delimiter lines may have trailing whitespace", func(t *testing.T) {
		s := parseOK(t, "--- \nname: x\ndescription: y\n---  \n")
		if s.Name != "x" {
			t.Fatalf("Name = %q, want x", s.Name)
		}
	})

	t.Run("name length boundary", func(t *testing.T) {
		name64 := strings.Repeat("a", MaxNameLen)
		if s := parseOK(t, "---\nname: "+name64+"\ndescription: y\n---\n"); s.Name != name64 {
			t.Fatalf("Name = %q, want %q", s.Name, name64)
		}
		parseErr(t, "---\nname: "+strings.Repeat("a", MaxNameLen+1)+"\ndescription: y\n---\n")
	})

	t.Run("whitespace is collapsed to a single line", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: '  multi   word\t desc  '\n---\n")
		if s.Description != "multi word desc" {
			t.Fatalf("Description = %q, want collapsed", s.Description)
		}
	})
}

func TestParseFrontmatterRepair(t *testing.T) {
	t.Run("bare scalar with colon is repaired", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: Build for AWS: ECS and EKS\n---\n")
		if s.Description != "Build for AWS: ECS and EKS" {
			t.Fatalf("Description = %q", s.Description)
		}
	})

	t.Run("trailing comment survives repair", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: Deploy to: prod # careful\n---\n")
		if s.Description != "Deploy to: prod" {
			t.Fatalf("Description = %q", s.Description)
		}
	})

	t.Run("quoted scalars are not repaired", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: 'already: quoted'\n---\n")
		if s.Description != "already: quoted" {
			t.Fatalf("Description = %q", s.Description)
		}
	})

	t.Run("block scalar body is not repaired", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: |\n  line one: with colon\n  line two: another\n---\n")
		if s.Description != "line one: with colon line two: another" {
			t.Fatalf("Description = %q", s.Description)
		}
	})

	t.Run("unrecoverable YAML reports the original error", func(t *testing.T) {
		// The bare scalar gets quoted by the repair, but the stray sequence
		// entry still fails — and the error must be the ORIGINAL one.
		msg := parseErr(t, "---\nname: x\ndescription: ok: fine\n  - stray\n---\n")
		if !strings.Contains(msg, "invalid YAML") {
			t.Fatalf("error = %q, want invalid YAML", msg)
		}
	})

	t.Run("repairable bare scalar starting with flow char", func(t *testing.T) {
		s := parseOK(t, "---\nname: x\ndescription: [info] see: docs\n---\n")
		if s.Description != "[info] see: docs" {
			t.Fatalf("Description = %q", s.Description)
		}
	})
}

func TestExtractFrontmatter(t *testing.T) {
	if _, ok := extractFrontmatter("no delimiters"); ok {
		t.Fatal("extractFrontmatter() ok = true, want false")
	}
	if _, ok := extractFrontmatter("---\n---\n"); ok {
		t.Fatal("extractFrontmatter() ok = true for empty body, want false")
	}
	fm, ok := extractFrontmatter("---\nname: x\n---\n# body\n")
	if !ok || fm != "name: x" {
		t.Fatalf("extractFrontmatter() = %q, %v", fm, ok)
	}
}
