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

func makeCatalog(skills ...*Skill) *Catalog {
	return newCatalog(skills, nil)
}

func testSkill(name, desc string, scope Scope) *Skill {
	return &Skill{Name: name, Description: desc, Path: "/skills/" + name + "/SKILL.md", Dir: "/skills/" + name, Scope: scope}
}

func TestBudgetTokens(t *testing.T) {
	if got := BudgetTokens(200_000); got != 4_000 {
		t.Fatalf("BudgetTokens(200000) = %d, want 4000", got)
	}
	if got := BudgetTokens(99); got != 1 {
		t.Fatalf("BudgetTokens(99) = %d, want 1", got)
	}
	if got := BudgetTokens(0); got != FallbackBudgetTokens {
		t.Fatalf("BudgetTokens(0) = %d, want fallback %d", got, FallbackBudgetTokens)
	}
	if got := BudgetTokens(-1); got != FallbackBudgetTokens {
		t.Fatalf("BudgetTokens(-1) = %d, want fallback %d", got, FallbackBudgetTokens)
	}
}

func TestRenderEmptyCatalog(t *testing.T) {
	if got := Render(makeCatalog(), 0); got != "" {
		t.Fatalf("Render(empty) = %q, want empty", got)
	}
	if got := Render(nil, 0); got != "" {
		t.Fatalf("Render(nil) = %q, want empty", got)
	}
}

func TestRenderFullListWithinBudget(t *testing.T) {
	cat := makeCatalog(
		testSkill("pandora", "查询 appkey 归属与监控指标", ScopeUser),
		testSkill("review", "code review helper", ScopeRepo),
	)
	body := Render(cat, 200_000)
	if !strings.Contains(body, "- review: code review helper (file: /skills/review/SKILL.md)") {
		t.Fatalf("body missing repo skill line:\n%s", body)
	}
	if !strings.Contains(body, "- pandora: 查询 appkey 归属与监控指标 (file: /skills/pandora/SKILL.md)") {
		t.Fatalf("body missing user skill line:\n%s", body)
	}
	// repo sorts before user.
	if strings.Index(body, "- review:") > strings.Index(body, "- pandora:") {
		t.Fatalf("repo skill must render before user skill:\n%s", body)
	}
	if !strings.Contains(body, "read_skill") || !strings.Contains(body, "untrusted content") {
		t.Fatalf("body missing usage instructions or untrusted-content notice:\n%s", body)
	}
}

func TestRenderDescriptionRoundRobinTruncation(t *testing.T) {
	// Two skills with very different description lengths; the short one must
	// render in full and yield its unused share to the long one.
	lines := []skillLine{
		{name: "short", desc: "xy", path: "/skills/short/SKILL.md"},
		{name: "long", desc: strings.Repeat("a", 40), path: "/skills/long/SKILL.md"},
	}
	budget := totalCost(renderMinimalLines(lines)) + 3 // minimal + 12 bytes of description room
	out := allocateDescriptions(lines, budget)
	if !strings.Contains(out[0], "xy") {
		t.Fatalf("short description must render in full: %q", out[0])
	}
	if strings.Contains(out[1], strings.Repeat("a", 40)) {
		t.Fatalf("long description must be truncated to fit budget: %q", out[1])
	}
	if !strings.Contains(out[1], "aaa") {
		t.Fatalf("long description must receive the yielded share: %q", out[1])
	}
	if totalCost(out) > budget {
		t.Fatalf("totalCost = %d > budget %d", totalCost(out), budget)
	}
}

func TestRenderSingleDescriptionCap(t *testing.T) {
	long := strings.Repeat("字", MaxDescriptionChars+10)
	got := capDescription(long)
	runes := []rune(got)
	if len(runes) != MaxDescriptionChars || !strings.HasSuffix(got, descriptionCapSuffix) {
		t.Fatalf("capDescription length = %d, want %d with suffix", len(runes), MaxDescriptionChars)
	}
}

func TestRenderMinimalAndSkipOverflow(t *testing.T) {
	// Budget fits only the cheaper second entry: the expensive first entry is
	// SKIPPED and scanning continues (not truncated at the tail).
	expensive := testSkill("a-expensive", "desc", ScopeRepo)
	expensive.Path = "/" + strings.Repeat("p", 200) + "/SKILL.md"
	cheap := testSkill("z-cheap", "desc", ScopeUser)
	cat := makeCatalog(expensive, cheap)

	budget := tokenCost(cheapLine(cheap))
	body := renderBodyFromRender(cat, budget)
	if strings.Contains(body, "a-expensive") {
		t.Fatalf("expensive entry must be skipped:\n%s", body)
	}
	if !strings.Contains(body, "z-cheap") {
		t.Fatalf("cheap entry must be kept:\n%s", body)
	}
	if !strings.Contains(body, "1 more skills omitted due to the prompt budget") {
		t.Fatalf("omission must be noted:\n%s", body)
	}
	// Descriptions are gone at this degradation level.
	if strings.Contains(body, ": desc (file:") {
		t.Fatalf("descriptions must be stripped before omission:\n%s", body)
	}
}

func cheapLine(s *Skill) string {
	return skillLine{name: s.Name, desc: "", path: s.Path}.minimal()
}

// renderBodyFromRender renders with an explicit token budget by calling the
// same degradation chain Render uses, bypassing BudgetTokens.
func renderBodyFromRender(cat *Catalog, budget int) string {
	skills := cat.Skills()
	lines := make([]skillLine, 0, len(skills))
	for _, s := range skills {
		lines = append(lines, skillLine{name: s.Name, desc: capDescription(s.Description), path: s.Path})
	}
	if totalCost(renderFullLines(lines)) <= budget {
		return renderBody(renderFullLines(lines), 0, len(cat.Issues()))
	}
	truncated := allocateDescriptions(lines, budget)
	if totalCost(truncated) <= budget {
		return renderBody(truncated, 0, len(cat.Issues()))
	}
	included, omitted := fitWithinBudget(renderMinimalLines(lines), budget)
	return renderBody(included, omitted, len(cat.Issues()))
}

func TestRenderCJKCostIsByteBased(t *testing.T) {
	// One CJK rune is 3 bytes ≈ 1 token by the bytes/4 rule; a char-based
	// budget would undercount 3x. 8 CJK chars = 24 bytes → 6+ tokens.
	line := "- x: 查询监控指标数据 (file: /p)"
	want := (len(line) + 1 + approxBytesPerToken - 1) / approxBytesPerToken
	if got := tokenCost(line); got != want {
		t.Fatalf("tokenCost = %d, want %d", got, want)
	}
	if tokenCost(line) < 8 {
		t.Fatalf("CJK line undercharged: %d tokens", tokenCost(line))
	}
}

func TestRenderIssueNote(t *testing.T) {
	cat := newCatalog(nil, []LoadIssue{{Path: "/x/SKILL.md", Message: "bad yaml"}})
	body := Render(cat, 0)
	if !strings.Contains(body, "1 skills failed to load") {
		t.Fatalf("issue note missing:\n%s", body)
	}
}
