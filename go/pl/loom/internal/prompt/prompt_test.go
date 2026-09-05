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
// Created: 2026/07/24

package prompt

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

type staticEnvProvider struct {
	env Environment
	err error
}

func (p staticEnvProvider) Collect(context.Context) (Environment, error) { return p.env, p.err }

type staticRulesProvider struct {
	files []RuleFile
	err   error
}

func (p staticRulesProvider) Discover(context.Context) ([]RuleFile, error) { return p.files, p.err }

// noRules keeps builder tests hermetic: without it the builder would pick up
// real rule files (e.g. ~/.loom/LOOM.md) from the host.
var noRules = WithRulesProvider(staticRulesProvider{})

func testEnvironment() Environment {
	return Environment{
		WorkspaceRoot: "/ws",
		IsGitRepo:     true,
		GitBranch:     "main",
		GitHead:       "abc1234",
		Platform:      "darwin/arm64",
		Shell:         "/bin/zsh",
		Now:           time.Date(2026, 7, 24, 12, 0, 0, 0, time.UTC),
	}
}

func TestBuildContainsAllBuiltinSectionsInOrder(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)

	titles := []string{"Identity & Role", "Core Workflow", "Task Planning", "Code Change Guidelines", "Communication", "Runtime Environment", "Terminal & Git Safety", "Environment & Context"}
	last := -1
	for _, title := range titles {
		idx := strings.Index(text, "# "+title)
		require.Greater(t, idx, last, "section %q missing or out of order", title)
		last = idx
	}

	require.Len(t, rules, len(titles))
	for _, rule := range rules {
		assert.NotEmpty(t, rule.Source)
		assert.True(t, strings.HasPrefix(rule.Hash, "sha256:"), "rule hash should carry sha256 prefix: %q", rule.Hash)
	}
	assert.Equal(t, "loom://builtin/identity", rules[0].Source)
	assert.Equal(t, "loom://builtin/environment", rules[len(rules)-1].Source)
}

func TestBuildDeclaresWebFetchCapabilityAndNoGatekeeping(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	// The model must learn from the prompt — not from a refusal loop — that
	// web_fetch has direct network access outside the run_cmd sandbox.
	assert.Contains(t, text, "web_fetch")
	assert.Contains(t, text, "bypassing the sandbox")
	// Anti-gatekeeping: fulfill any tool-completable request instead of
	// refusing on identity grounds.
	assert.Contains(t, text, "do not self-limit by your \"coding assistant\" role")
}

func TestBuildDeclaresWorkflowAndConciseRules(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	// Preamble narration and validation discipline from the workflow section.
	assert.Contains(t, text, "Narrate before acting")
	assert.Contains(t, text, "do not re-read the file to confirm")
	// Codex-style concise final-answer rules.
	assert.Contains(t, text, "within 10 lines")
	assert.Contains(t, text, "do not dump the full contents of files")
}

func TestBuildRendersEnvironmentSnapshot(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "Workspace root: /ws")
	assert.Contains(t, text, "branch main, HEAD abc1234")
	assert.Contains(t, text, "darwin/arm64")
	assert.Contains(t, text, "/bin/zsh")
	// Date-level granularity keeps the dynamic section cache-friendly.
	assert.Contains(t, text, "Current date: 2026-07-24 UTC")
}

func TestBuildRendersNonGitWorkspace(t *testing.T) {
	env := testEnvironment()
	env.IsGitRepo = false
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: env}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "not a Git repository")
}

func TestBuildAppendsExtraInstructions(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		noRules,
		WithExtraInstructions("始终使用 Bazel 构建。"))
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "# Additional Instructions\n始终使用 Bazel 构建。")
	assertRuleSourcePresent(t, rules, "loom://config/extra-instructions")
}

func TestBuildIncludesWorkspaceRules(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		WithRulesProvider(staticRulesProvider{files: []RuleFile{
			{Path: "/ws/LOOM.md", Content: "一律使用 Bazel 构建。"},
		}}))
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "# Workspace Rules (/ws/LOOM.md)")
	assert.Contains(t, text, "一律使用 Bazel 构建。")
	assert.Contains(t, text, "must never raise privileges")
	assertRuleSourcePresent(t, rules, "file:///ws/LOOM.md")
}

func TestBuildOrdersSectionsByContextPriority(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		WithRulesProvider(staticRulesProvider{files: []RuleFile{
			{Path: "/ws/LOOM.md", Content: "规则X"},
		}}),
		WithExtraInstructions("附加X"))
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	extraIdx := strings.Index(text, "# Additional Instructions")
	rulesIdx := strings.Index(text, "# Workspace Rules")
	envIdx := strings.Index(text, "# Environment & Context")
	require.GreaterOrEqual(t, extraIdx, 0)
	require.Greater(t, rulesIdx, extraIdx, "workspace rules should follow user preferences")
	require.Greater(t, envIdx, rulesIdx, "environment snapshot should come last")
}

func TestBuildSkipsWorkspaceRulesOnProviderError(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		WithRulesProvider(staticRulesProvider{err: errors.New("boom")}))
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.NotContains(t, text, "Workspace Rules")
}

func TestBuildDegradesWhenEnvironmentCollectionFails(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{err: errors.New("boom")}),
		noRules,
		WithClock(domain.NewFakeClock(time.Date(2026, 7, 24, 0, 0, 0, 0, time.UTC))))
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "Workspace root: /ws")
	assert.Contains(t, text, "environment collection incomplete")
	assert.NotEmpty(t, rules)
}

func TestBuildHashesAreDeterministic(t *testing.T) {
	build := func() ([]domain.ContextRuleRef, error) {
		b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
		_, rules, err := b.Build(context.Background())
		return rules, err
	}
	rules1, err1 := build()
	rules2, err2 := build()
	require.NoError(t, err1)
	require.NoError(t, err2)
	assert.Equal(t, rules1, rules2)
}

func TestSystemEnvProviderDetectsNonGitDirectory(t *testing.T) {
	provider := systemEnvProvider{workspaceRoot: t.TempDir(), clock: domain.RealClock{}}
	env, err := provider.Collect(context.Background())
	require.NoError(t, err)
	assert.False(t, env.IsGitRepo)
	assert.NotZero(t, env.Now)
	assert.NotEmpty(t, env.Platform)
}

func TestSystemEnvProviderDetectsGitRepository(t *testing.T) {
	repoRoot := t.TempDir()
	gitRun(t, repoRoot, "init")
	gitRun(t, repoRoot, "config", "user.email", "loom@example.com")
	gitRun(t, repoRoot, "config", "user.name", "Loom Test")
	require.NoError(t, os.WriteFile(filepath.Join(repoRoot, "README"), []byte("test"), 0o600))
	gitRun(t, repoRoot, "add", "README")
	gitRun(t, repoRoot, "commit", "-m", "init")

	provider := systemEnvProvider{workspaceRoot: repoRoot, clock: domain.RealClock{}}
	env, err := provider.Collect(context.Background())
	require.NoError(t, err)
	require.True(t, env.IsGitRepo)
	assert.NotEmpty(t, env.GitBranch)
	assert.Len(t, env.GitHead, 7)
}

func TestFileRulesProviderDiscoversLayeredFiles(t *testing.T) {
	base := t.TempDir()
	ws := t.TempDir()
	globalFile := filepath.Join(base, "LOOM.md")
	require.NoError(t, os.WriteFile(globalFile, []byte("全局规则"), 0o600))
	require.NoError(t, os.WriteFile(filepath.Join(ws, "LOOM.md"), []byte("项目规则"), 0o600))
	require.NoError(t, os.WriteFile(filepath.Join(ws, "CLAUDE.md"), []byte("兼容规则"), 0o600))

	p := &FileRulesProvider{workspaceRoot: ws, globalFile: globalFile}
	files, err := p.Discover(context.Background())
	require.NoError(t, err)
	require.Len(t, files, 3)
	assert.Equal(t, "全局规则", files[0].Content, "user-global rules come first")
	assert.Equal(t, "项目规则", files[1].Content)
	assert.Equal(t, "兼容规则", files[2].Content)
}

func TestFileRulesProviderSkipsMissingAndEmptyFiles(t *testing.T) {
	ws := t.TempDir()
	require.NoError(t, os.WriteFile(filepath.Join(ws, "LOOM.md"), []byte("  \n"), 0o600))
	p := &FileRulesProvider{workspaceRoot: ws, globalFile: filepath.Join(t.TempDir(), "LOOM.md")}
	files, err := p.Discover(context.Background())
	require.NoError(t, err)
	assert.Empty(t, files)
}

func TestFileRulesProviderTruncatesOversizedFile(t *testing.T) {
	ws := t.TempDir()
	oversized := strings.Repeat("甲", maxRuleFileBytes*2)
	require.NoError(t, os.WriteFile(filepath.Join(ws, "LOOM.md"), []byte(oversized), 0o600))
	p := &FileRulesProvider{workspaceRoot: ws, globalFile: ""}
	files, err := p.Discover(context.Background())
	require.NoError(t, err)
	require.Len(t, files, 1)
	assert.Contains(t, files[0].Content, "已截断")
	assert.LessOrEqual(t, len(files[0].Content), maxRuleFileBytes+len("\n（规则文件超过 32KB，已截断）"))
	// Regression (REVIEW R10): 甲 is 3 bytes and 32768 % 3 = 2, so the old
	// byte-level cut split a rune and produced invalid UTF-8.
	assert.True(t, utf8.ValidString(files[0].Content), "truncated rule file must stay valid UTF-8")
}

// TestBuilderManagedBaseReplacesBuiltinSections verifies that a
// Langfuse-managed prompt supersedes the built-in normative sections while
// dynamic sections (extra instructions) still apply, and that the managed
// identity is reported for generation linking.
func TestBuilderManagedBaseReplacesBuiltinSections(t *testing.T) {
	builder := NewBuilder(
		t.TempDir(),
		WithExtraInstructions("no emoji"),
		WithManagedBase("loom-system", 3, "managed identity"),
	)
	text, rules, err := builder.Build(context.Background())
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	if !strings.Contains(text, "managed identity") {
		t.Fatalf("managed content missing:\n%s", text)
	}
	if strings.Contains(text, "Identity & Role") {
		t.Fatalf("builtin sections must be replaced:\n%s", text)
	}
	if !strings.Contains(text, "no emoji") {
		t.Fatalf("extra instructions must still apply:\n%s", text)
	}
	assertRuleSourcePresent(t, rules, "langfuse://prompts/loom-system?v=3")

	name, version, ok := builder.ManagedPromptInfo()
	if !ok || name != "loom-system" || version != 3 {
		t.Fatalf("ManagedPromptInfo = %q, %d, %v", name, version, ok)
	}

	plain := NewBuilder(t.TempDir())
	if _, _, ok := plain.ManagedPromptInfo(); ok {
		t.Fatal("builder without managed base must report not-managed")
	}
}

func assertRuleSourcePresent(t *testing.T, rules []domain.ContextRuleRef, source string) {
	t.Helper()
	for _, r := range rules {
		if r.Source == source {
			assert.True(t, strings.HasPrefix(r.Hash, "sha256:"), "rule %q missing content hash", source)
			return
		}
	}
	t.Fatalf("rule source %q not found in %+v", source, rules)
}

func gitRun(t *testing.T, repoRoot string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", repoRoot}, args...)...)
	cmd.Env = append([]string{"LANG=C", "LC_ALL=C"}, os.Environ()...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v failed: %v\n%s", args, err, output)
	}
}

type staticSkillsProvider struct {
	body string
	err  error
}

func (p staticSkillsProvider) Skills(context.Context) (string, error) { return p.body, p.err }

func TestBuildInjectsSkillsSection(t *testing.T) {
	b := NewBuilder(
		"/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		WithRulesProvider(staticRulesProvider{files: []RuleFile{{Path: "/ws/LOOM.md", Content: "rule"}}}),
		WithSkillsProvider(staticSkillsProvider{body: "skill catalog body"}),
	)
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)

	skillsIdx := strings.Index(text, "# Available Skills")
	require.Greater(t, skillsIdx, -1, "skills section missing")
	assert.True(t, strings.Index(text, "rule") < skillsIdx, "skills section must follow workspace rules")
	assert.True(t, skillsIdx < strings.Index(text, "# Environment & Context"), "skills section must precede the environment section")
	assert.Contains(t, text, "skill catalog body")

	var sources []string
	for _, r := range rules {
		sources = append(sources, r.Source)
	}
	assert.Contains(t, sources, "loom://skills/catalog")
}

func TestBuildSkillsProviderFailureDegradesToNoSection(t *testing.T) {
	b := NewBuilder(
		"/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		noRules,
		WithSkillsProvider(staticSkillsProvider{err: errors.New("boom")}),
	)
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.NotContains(t, text, "Available Skills")
	// Every other section keeps its audit ref.
	for _, r := range rules {
		assert.NotEqual(t, "loom://skills/catalog", r.Source)
	}
	assert.Equal(t, "loom://builtin/environment", rules[len(rules)-1].Source)
}

func TestBuildSkillsEmptyBodyOmitsSection(t *testing.T) {
	b := NewBuilder(
		"/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		noRules,
		WithSkillsProvider(staticSkillsProvider{body: "  \n"}),
	)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.NotContains(t, text, "Available Skills")
}

func TestBuildManagedPromptKeepsSkillsSection(t *testing.T) {
	b := NewBuilder(
		"/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		noRules,
		WithManagedBase("managed", 3, "managed body"),
		WithSkillsProvider(staticSkillsProvider{body: "skill catalog body"}),
	)
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	// Managed base replaces builtin sections but dynamic sections survive.
	assert.NotContains(t, text, "Identity & Role")
	assert.Contains(t, text, "managed body")
	assert.Contains(t, text, "# Available Skills")
	var sources []string
	for _, r := range rules {
		sources = append(sources, r.Source)
	}
	assert.Contains(t, sources, "loom://skills/catalog")
}

func TestBuildRendersWorkspaceOverview(t *testing.T) {
	env := testEnvironment()
	env.WorkspaceOverview = "go/\n  pl/"
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: env}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "Workspace overview")
	assert.Contains(t, text, "go/\n  pl/")
}

func TestWorkspaceOverviewListsThreeLevelsAndSkipsNoise(t *testing.T) {
	root := t.TempDir()
	require.NoError(t, os.MkdirAll(filepath.Join(root, "go", "pl", "loom"), 0o755))
	require.NoError(t, os.MkdirAll(filepath.Join(root, "cpp"), 0o755))
	require.NoError(t, os.MkdirAll(filepath.Join(root, ".git"), 0o755))
	require.NoError(t, os.MkdirAll(filepath.Join(root, "node_modules"), 0o755))
	require.NoError(t, os.WriteFile(filepath.Join(root, "README.md"), []byte("x"), 0o644))
	require.NoError(t, os.WriteFile(filepath.Join(root, ".hidden"), []byte("x"), 0o644))
	// Level-3 files are omitted (orientation only); level-3 dirs are listed.
	require.NoError(t, os.WriteFile(filepath.Join(root, "go", "pl", "BUILD"), []byte("x"), 0o644))

	overview := workspaceOverview(root)
	assert.Contains(t, overview, "go/")
	assert.Contains(t, overview, "  pl/")
	assert.Contains(t, overview, "    loom/")
	assert.Contains(t, overview, "cpp/")
	assert.Contains(t, overview, "README.md")
	assert.NotContains(t, overview, "BUILD")
	assert.NotContains(t, overview, ".git")
	assert.NotContains(t, overview, "node_modules")
	assert.NotContains(t, overview, ".hidden")
}

func TestWorkspaceOverviewUnreadableRootReturnsEmpty(t *testing.T) {
	assert.Empty(t, workspaceOverview(filepath.Join(t.TempDir(), "missing")))
}

func TestWorkspaceOverviewCapsLongListings(t *testing.T) {
	root := t.TempDir()
	for i := 0; i < overviewMaxLines+20; i++ {
		require.NoError(t, os.WriteFile(filepath.Join(root, fmt.Sprintf("f%04d.txt", i)), []byte("x"), 0o644))
	}
	overview := workspaceOverview(root)
	// 80 content lines + 1 truncation marker line.
	assert.LessOrEqual(t, len(strings.Split(overview, "\n")), overviewMaxLines+1)
	assert.Contains(t, overview, "…")
}

func TestBuildSectionsSplitsStaticAndDynamic(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	secs, err := b.BuildSections(context.Background())
	require.NoError(t, err)

	assert.Contains(t, secs.Static, "# Identity & Role")
	assert.NotContains(t, secs.Static, "Workspace root")
	assert.Contains(t, secs.Dynamic, "# Environment & Context")
	assert.Contains(t, secs.Dynamic, "Workspace root: /ws")

	// Build() stays the concatenation of the two parts, with all refs.
	text, refs, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Equal(t, secs.Static+"\n\n"+secs.Dynamic, text)
	assert.Equal(t, secs.Refs, refs)

	// The static part is byte-stable across builds (prompt-cache prefix).
	again, err := b.BuildSections(context.Background())
	require.NoError(t, err)
	assert.Equal(t, secs.Static, again.Static)
}

func TestFamilyPatch(t *testing.T) {
	familyPatches["anthropic"] = "Always think step by step."
	defer delete(familyPatches, "anthropic")

	patch, ref := FamilyPatch("anthropic/claude-sonnet-4-5")
	assert.Equal(t, "Always think step by step.", patch)
	require.NotNil(t, ref)
	assert.Equal(t, "loom://builtin/model-family/anthropic", ref.Source)
	assert.True(t, strings.HasPrefix(ref.Hash, "sha256:"))

	patch, ref = FamilyPatch("openai/gpt-5.2")
	assert.Empty(t, patch)
	assert.Nil(t, ref)
}

func TestModelFamilyNormalization(t *testing.T) {
	assert.Equal(t, "anthropic", modelFamily("claude-sonnet-4-5"))
	assert.Equal(t, "openai", modelFamily("GPT-5.2"))
	assert.Equal(t, "openai", modelFamily("openai/o4-mini"))
	assert.Equal(t, "deepseek", modelFamily("deepseek-chat"))
	assert.Equal(t, "custom-model", modelFamily("custom-model"))
}

// mutableEnvProvider returns a different workspace overview on every call,
// simulating a workspace whose tree changes between model requests (the
// agent's own writes are the most common trigger). Concurrent builds must
// be safe: the provider mutates shared state, so it carries its own lock
// (the builder may call Build from concurrent read-only projections).
type mutableEnvProvider struct {
	mu    sync.Mutex
	calls int
	env   Environment
}

func (p *mutableEnvProvider) Collect(context.Context) (Environment, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.calls++
	p.env.WorkspaceOverview = fmt.Sprintf("overview-render-%d", p.calls)
	return p.env, nil
}

func TestBuildFreezesWorkspaceOverviewAcrossBuilds(t *testing.T) {
	provider := &mutableEnvProvider{env: testEnvironment()}
	b := NewBuilder("/ws", WithEnvProvider(provider), noRules)

	first, refs1, err := b.Build(context.Background())
	require.NoError(t, err)
	second, refs2, err := b.Build(context.Background())
	require.NoError(t, err)

	// Every collection ran (the rest of the snapshot stays fresh), but the
	// rendered overview is pinned to the first capture for the session.
	assert.Equal(t, 2, provider.calls)
	assert.Contains(t, first, "overview-render-1")
	assert.Contains(t, second, "overview-render-1")
	assert.NotContains(t, second, "overview-render-2")
	// The dynamic section and its audit hash stay byte-stable between
	// requests, so provider prompt caches survive run-internal file writes.
	assert.Equal(t, refs1, refs2)
	assert.Equal(t, first, second)
}

func TestBuildFrozenOverviewDoesNotLeakAcrossBuilders(t *testing.T) {
	provider := &mutableEnvProvider{env: testEnvironment()}

	b1 := NewBuilder("/ws", WithEnvProvider(provider), noRules)
	text1, _, err := b1.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text1, "overview-render-1")

	// A new builder (a fresh session) re-collects and pins its own value.
	b2 := NewBuilder("/ws", WithEnvProvider(provider), noRules)
	text2, _, err := b2.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text2, "overview-render-2")
}

func TestBuildFrozenOverviewSurvivesConcurrentBuilds(t *testing.T) {
	provider := &mutableEnvProvider{env: testEnvironment()}
	b := NewBuilder("/ws", WithEnvProvider(provider), noRules)
	first, _, err := b.Build(context.Background())
	require.NoError(t, err)

	const workers = 8
	texts := make([]string, workers)
	var wg sync.WaitGroup
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			text, _, err := b.Build(context.Background())
			if err != nil {
				t.Errorf("Build: %v", err)
				return
			}
			texts[i] = text
		}(i)
	}
	wg.Wait()
	for i, text := range texts {
		assert.Equal(t, first, text, "concurrent build %d diverged", i)
	}
}
