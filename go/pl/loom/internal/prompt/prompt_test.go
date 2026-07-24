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
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

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

	titles := []string{"身份与角色", "核心工作方式", "代码修改规范", "沟通规范", "终端与 Git 安全约束", "环境与上下文"}
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

func TestBuildRendersEnvironmentSnapshot(t *testing.T) {
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: testEnvironment()}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "工作区根目录: /ws")
	assert.Contains(t, text, "当前分支 main，HEAD 为 abc1234")
	assert.Contains(t, text, "darwin/arm64")
	assert.Contains(t, text, "/bin/zsh")
	assert.Contains(t, text, "2026-07-24 12:00:00 UTC")
}

func TestBuildRendersNonGitWorkspace(t *testing.T) {
	env := testEnvironment()
	env.IsGitRepo = false
	b := NewBuilder("/ws", WithEnvProvider(staticEnvProvider{env: env}), noRules)
	text, _, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "非 Git 仓库")
}

func TestBuildAppendsExtraInstructions(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{env: testEnvironment()}),
		noRules,
		WithExtraInstructions("始终使用 Bazel 构建。"))
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "# 附加指令\n始终使用 Bazel 构建。")
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
	assert.Contains(t, text, "# 工作区规则（/ws/LOOM.md）")
	assert.Contains(t, text, "一律使用 Bazel 构建。")
	assert.Contains(t, text, "不能提升权限")
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
	extraIdx := strings.Index(text, "# 附加指令")
	rulesIdx := strings.Index(text, "# 工作区规则")
	envIdx := strings.Index(text, "# 环境与上下文")
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
	assert.NotContains(t, text, "工作区规则")
}

func TestBuildDegradesWhenEnvironmentCollectionFails(t *testing.T) {
	b := NewBuilder("/ws",
		WithEnvProvider(staticEnvProvider{err: errors.New("boom")}),
		noRules,
		WithClock(domain.NewFakeClock(time.Date(2026, 7, 24, 0, 0, 0, 0, time.UTC))))
	text, rules, err := b.Build(context.Background())
	require.NoError(t, err)
	assert.Contains(t, text, "工作区根目录: /ws")
	assert.Contains(t, text, "环境信息采集不完整")
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
	home := t.TempDir()
	ws := t.TempDir()
	require.NoError(t, os.MkdirAll(filepath.Join(home, ".loom"), 0o700))
	require.NoError(t, os.WriteFile(filepath.Join(home, ".loom", "LOOM.md"), []byte("全局规则"), 0o600))
	require.NoError(t, os.WriteFile(filepath.Join(ws, "LOOM.md"), []byte("项目规则"), 0o600))
	require.NoError(t, os.WriteFile(filepath.Join(ws, "CLAUDE.md"), []byte("兼容规则"), 0o600))

	p := &FileRulesProvider{workspaceRoot: ws, homeDir: home}
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
	p := &FileRulesProvider{workspaceRoot: ws, homeDir: t.TempDir()}
	files, err := p.Discover(context.Background())
	require.NoError(t, err)
	assert.Empty(t, files)
}

func TestFileRulesProviderTruncatesOversizedFile(t *testing.T) {
	ws := t.TempDir()
	oversized := strings.Repeat("甲", maxRuleFileBytes*2)
	require.NoError(t, os.WriteFile(filepath.Join(ws, "LOOM.md"), []byte(oversized), 0o600))
	p := &FileRulesProvider{workspaceRoot: ws, homeDir: ""}
	files, err := p.Discover(context.Background())
	require.NoError(t, err)
	require.Len(t, files, 1)
	assert.Contains(t, files[0].Content, "已截断")
	assert.LessOrEqual(t, len(files[0].Content), maxRuleFileBytes+len("\n（规则文件超过 32KB，已截断）"))
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
