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
// Created: 2026/08/08

package app

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
)

func TestApplyConfigWebSearchKeepsAllWorkspacesAtStartupValue(t *testing.T) {
	for _, enabled := range []bool{false, true} {
		t.Run(fmt.Sprintf("startup_enabled=%v", enabled), func(t *testing.T) {
			t.Setenv("LOOM_WEB_SEARCH_PROVIDER", "duckduckgo")
			proc := newWebSearchTestProcess(t, enabled, true)
			reg, err := NewWorkspaceRegistry(proc)
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(reg.Close)
			first, err := reg.RegisterDefault(t.Context(), t.TempDir())
			if err != nil {
				t.Fatal(err)
			}
			broker := runtimeevent.NewBroker()
			t.Cleanup(broker.Close)
			svc := NewSessionService(proc, reg, broker, SessionServiceConfig{})
			t.Cleanup(func() { _ = svc.Shutdown(t.Context()) })

			check := func(b *Bootstrap, want bool, extra string) {
				t.Helper()
				checkRole := func(registry *agent.ToolRegistry, builder agent.PromptBuilder) {
					t.Helper()
					tool, ok := registry.Lookup("web_search")
					if ok != want || (ok && tool.Definition().Source != domain.ToolSourceBuiltin) {
						t.Fatalf("workspace %s builtin web_search = %v, want %v", b.WorkspaceID, ok, want)
					}
					text, _, err := builder.Build(t.Context())
					if err != nil {
						t.Fatal(err)
					}
					if strings.Contains(text, "web_search") != want {
						t.Fatalf("workspace %s prompt does not match builtin availability %v", b.WorkspaceID, want)
					}
				}
				checkRole(b.Registry, b.CurrentPrompt())
				text, _, err := b.CurrentPrompt().Build(t.Context())
				if err != nil || !strings.Contains(text, extra) {
					t.Fatalf("main prompt missing live instructions %q: %v", extra, err)
				}
				if b.SubagentFactory == nil || b.SubagentManager == nil {
					t.Fatal("subagents not assembled")
				}
				checkRole(b.SubagentFactory.Registry, b.SubagentFactory.Prompt)
				for _, role := range []subagent.Role{subagent.RoleResearcher, subagent.RoleCoder} {
					checkRole(b.SubagentManager.RoleRegistry(role), b.SubagentManager.RolePrompt(role))
				}
			}
			check(first, enabled, "")
			next := new(*proc.Resolved())
			next.WebSearch.Enabled = !enabled
			next.Prompt.Extra = "multi-workspace reloaded instructions"
			report := svc.ApplyConfig(t.Context(), next)
			if !slices.Contains(report.Restart, "web_search") || !slices.Contains(report.Immediate, "prompt") {
				t.Fatalf("reload report = %+v, want restart web_search and immediate prompt", report)
			}
			if proc.Resolved() != next || proc.webSearchEnabled != enabled {
				t.Fatal("live config must change without changing the startup-frozen flag")
			}
			check(first, enabled, next.Prompt.Extra)
			second, err := reg.Register(t.Context(), t.TempDir(), "after reload")
			if err != nil {
				t.Fatal(err)
			}
			if second == first || second.ProcessRuntime != proc {
				t.Fatal("expected a distinct workspace sharing the same process runtime")
			}
			check(second, enabled, next.Prompt.Extra)
			resolvedFirst, err := reg.Resolve(t.Context(), first.WorkspaceID)
			if err != nil || resolvedFirst != first {
				t.Fatalf("existing workspace was replaced: %v", err)
			}
			check(resolvedFirst, enabled, next.Prompt.Extra)

			// A newly constructed process captures the changed setting; all
			// its workspaces use it, including researcher and coder registries.
			fresh, err := NewProcessRuntime(t.Context(), next, ProcessRuntimeConfig{
				ArtifactDir: filepath.Join(t.TempDir(), "artifacts"), Logger: proc.Logger,
			})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(fresh.Close)
			if fresh.webSearchEnabled != next.WebSearch.Enabled {
				t.Fatal("new process did not capture the changed web search setting")
			}
			restarted, err := NewWorkspaceBootstrap(t.Context(), fresh, BootstrapConfig{WorkspaceRoot: t.TempDir()})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(restarted.Close)
			check(restarted, !enabled, next.Prompt.Extra)
			check(first, enabled, next.Prompt.Extra)
			check(second, enabled, next.Prompt.Extra)
		})
	}
}

func TestApplyConfigWebSearchPromptKeepsRegisteredTools(t *testing.T) {
	for _, tc := range []struct {
		name    string
		enabled bool
		mcp     bool
	}{
		{"disabled", false, false},
		{"enabled", true, false},
		{"disabled with same-named MCP", false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("LOOM_WEB_SEARCH_PROVIDER", "duckduckgo")
			proc := newWebSearchTestProcess(t, tc.enabled, false)
			b, err := NewWorkspaceBootstrap(t.Context(), proc, BootstrapConfig{WorkspaceRoot: t.TempDir()})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(b.Close)
			if tc.mcp {
				tool := fakes.NewFakeTool(domain.ToolDefinition{
					Name: "web_search", Source: domain.ToolSourceMCP,
					InputSchema: json.RawMessage(`{"type":"object"}`),
				}, domain.ToolResult{})
				if err := b.Registry.Register(tool); err != nil {
					t.Fatal(err)
				}
			}
			registered := b.Registry.List()
			broker := runtimeevent.NewBroker()
			t.Cleanup(broker.Close)
			svc := NewSingletonWorkspaceService(b, broker, SessionServiceConfig{})
			t.Cleanup(func() { _ = svc.Shutdown(t.Context()) })

			for _, configured := range []bool{!tc.enabled, tc.enabled} {
				previous := b.CurrentPrompt()
				next := new(*proc.Resolved())
				next.WebSearch.Enabled = configured
				next.Prompt.Extra = fmt.Sprintf("reloaded instructions configured=%v", configured)
				report := svc.ApplyConfig(t.Context(), next)
				if !slices.Contains(report.Immediate, "prompt") || previous == b.CurrentPrompt() {
					t.Fatal("config apply did not rebuild the main prompt")
				}
				if !slices.EqualFunc(registered, b.Registry.List(), func(a, b domain.ToolDefinition) bool {
					return a.Name == b.Name && a.Source == b.Source
				}) {
					t.Fatal("config apply changed tool registrations before restart")
				}
				text, _, err := b.CurrentPrompt().Build(t.Context())
				if err != nil {
					t.Fatal(err)
				}
				if !strings.Contains(text, next.Prompt.Extra) {
					t.Fatal("reloaded instructions missing")
				}
				if advertised := strings.Contains(text, "web_search"); advertised != tc.enabled {
					t.Fatalf("prompt advertises web_search = %v, want registered builtin %v (configured %v, MCP %v)", advertised, tc.enabled, configured, tc.mcp)
				}
			}
		})
	}
}

func TestRebuildPromptWebSearchIgnoresNonBuiltinTools(t *testing.T) {
	for _, tc := range []struct {
		name     string
		toolName string
		source   domain.ToolSource
		registry bool
	}{
		{name: "nil registry"},
		{name: "empty registry", registry: true},
		{name: "same-named MCP", toolName: "web_search", source: domain.ToolSourceMCP, registry: true},
		{name: "unattributed tool", toolName: "web_search", registry: true},
		{name: "prefixed MCP", toolName: "mcp__search__web_search", source: domain.ToolSourceMCP, registry: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			proc := &ProcessRuntime{Logger: slog.Default()}
			cfg := testResolvedConfig(nil)
			cfg.Storage.BaseDir = t.TempDir()
			cfg.WebSearch.Enabled = true
			proc.SwapResolved(cfg)
			b := &Bootstrap{ProcessRuntime: proc, WorkspaceRoot: t.TempDir()}
			if tc.registry {
				b.Registry = agent.NewToolRegistry()
			}
			if tc.toolName != "" {
				tool := fakes.NewFakeTool(domain.ToolDefinition{
					Name: tc.toolName, Source: tc.source,
					InputSchema: json.RawMessage(`{"type":"object"}`),
				}, domain.ToolResult{})
				if err := b.Registry.Register(tool); err != nil {
					t.Fatal(err)
				}
			}
			b.RebuildPrompt(t.Context())
			text, _, err := b.CurrentPrompt().Build(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(text, "web_search") {
				t.Fatal("prompt advertises builtin web_search without a registered builtin")
			}
		})
	}
}

func TestClassifyConfigChanges(t *testing.T) {
	base := testResolvedConfig(fakes.NewFakeModel())

	if r := classifyConfigChanges(base, base); len(r.Immediate)+len(r.NextTurn)+len(r.Restart) != 0 {
		t.Fatalf("identical configs produced %v", r)
	}

	cases := []struct {
		name                string
		mutate              func(c *config.ResolvedConfig)
		wantI, wantN, wantR string
	}{
		{"approval mode", func(c *config.ResolvedConfig) { c.Approval.Mode = permission.ModeNever }, "approval.mode", "", ""},
		{"approval trust user urls", func(c *config.ResolvedConfig) { c.Approval.TrustUserURLs = !c.Approval.TrustUserURLs }, "approval.trust_user_urls", "", ""},
		{"rules", func(c *config.ResolvedConfig) { c.Rules.Builtin = true }, "rules", "", ""},
		{"prompt", func(c *config.ResolvedConfig) { c.Prompt.Extra = "x" }, "prompt", "", ""},
		{"mcp", func(c *config.ResolvedConfig) {
			c.MCP.Servers = map[string]config.MCPServer{"s": {Command: "x"}}
		}, "mcp_servers", "", ""},
		{"default", func(c *config.ResolvedConfig) { c.Default.Model = "other" }, "", "default", ""},
		{"limits", func(c *config.ResolvedConfig) { c.Limits.MaxTokens = 42 }, "", "limits", ""},
		{"context", func(c *config.ResolvedConfig) { c.Context.Utilization = 0.5 }, "", "context", ""},
		{"runaway", func(c *config.ResolvedConfig) { c.Runaway.MaxRepeatedCalls = 9 }, "", "runaway", ""},
		{"subagent model", func(c *config.ResolvedConfig) {
			c.Subagent.Model = &config.ProviderModelRef{Provider: "test", Model: "model-a"}
		}, "", "subagent.model", ""},
		{"subagent enabled", func(c *config.ResolvedConfig) { c.Subagent.Enabled = true }, "", "", "subagent.enabled"},
		{"tracing cost", func(c *config.ResolvedConfig) { c.Tracing.CostInputPerMTok = 1.5 }, "", "tracing cost rates", ""},
		{"tracing host", func(c *config.ResolvedConfig) { c.Tracing.Host = "https://lf" }, "", "", "tracing"},
		{"memory", func(c *config.ResolvedConfig) { c.Memory.MaxJobsPerRun = 4 }, "", "", "memory"},
		{"sessions", func(c *config.ResolvedConfig) { c.Sessions.AutoArchiveAfter = time.Hour }, "", "sessions", ""},
		{"skills", func(c *config.ResolvedConfig) { c.Skills.Enabled = true }, "", "", "skills"},
		{"skills disabled", func(c *config.ResolvedConfig) { c.Skills.Disabled = []string{"x"} }, "skills.disabled", "", ""},
		{"image", func(c *config.ResolvedConfig) { c.Image.Model = "m" }, "", "", "image"},
		{"share", func(c *config.ResolvedConfig) { c.Share.Enabled = true }, "share", "", ""},
		{"logging", func(c *config.ResolvedConfig) { c.Logging.MaxFileBytes = 1 }, "", "", "logging"},
		{"ui", func(c *config.ResolvedConfig) { c.UI.Icons = "plain" }, "", "", "ui"},
		{"workspaces", func(c *config.ResolvedConfig) {
			c.Workspaces = []config.ResolvedWorkspace{{Name: "w", Root: "/r"}}
		}, "", "", "workspaces"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			next := *base
			tc.mutate(&next)
			r := classifyConfigChanges(base, &next)
			check := func(got []string, want, level string) {
				t.Helper()
				if want == "" {
					return
				}
				if !slices.Contains(got, want) {
					t.Fatalf("%s report = %v, want %q in %s", level, r, want, level)
				}
			}
			check(r.Immediate, tc.wantI, "immediate")
			check(r.NextTurn, tc.wantN, "next_turn")
			check(r.Restart, tc.wantR, "restart")
		})
	}
}

// TestApplyConfigHotSwap verifies the swap itself and the policy rebuild:
// after ApplyConfig the runtime reads the new resolved config and the
// workspace's approval mode without a restart.
func TestApplyConfigHotSwap(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	b := testBootstrap(store, fakes.NewFakeModel())
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(64))
	t.Cleanup(broker.Close)
	svc := NewSingletonWorkspaceService(b, broker, SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })

	next := *b.Resolved()
	next.Approval.Mode = permission.ModeDangerOnly
	next.Limits.MaxTokens = 4242
	report := svc.ApplyConfig(ctx, &next)

	if !slices.Contains(report.Immediate, "approval.mode") || !slices.Contains(report.NextTurn, "limits") {
		t.Fatalf("report = %+v", report)
	}
	if got := svc.proc.Resolved(); got != &next {
		t.Fatalf("resolved not swapped: %p, want %p", got, &next)
	}
	if got := svc.proc.Resolved().Limits.MaxTokens; got != 4242 {
		t.Fatalf("Limits.MaxTokens = %d, want 4242", got)
	}
	if b.approvalMode != permission.ModeDangerOnly {
		t.Fatalf("approvalMode = %q, want danger-only", b.approvalMode)
	}
}

// TestRulePackHotReload is the end-to-end proof that enabling a rule pack
// from the WebUI takes effect WITHOUT a restart: InstallRulePack writes
// pack-<id>.json and reloads every assembled workspace's policy, so the
// very next policy evaluation sees the unsandboxed grant. Uninstall
// reverts it immediately.
func TestRulePackHotReload(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })

	// Build a resolved config with rules enabled and a writable rules dir.
	// ResolvedStorage.RulesDir() == BaseDir/rules, and InstallRulePack
	// writes through SessionServiceConfig.RulesDir — point both at the
	// same directory so ReloadPolicy sees the installed pack file.
	baseDir := t.TempDir()
	rulesDir := filepath.Join(baseDir, "rules")
	base := testResolvedConfig(fakes.NewFakeModel())
	base.Rules = config.ResolvedRules{Enabled: true, Builtin: true}
	base.Storage = config.ResolvedStorage{BaseDir: baseDir}
	base.Approval = config.ResolvedApproval{Mode: permission.ModeDangerOnly, TrustUserURLs: true}

	proc := &ProcessRuntime{Current: base.Default, Store: store, Logger: slog.Default()}
	proc.SwapResolved(base)
	// Mirror NewBootstrap's policy wiring (bootstrap.go:434-436): a test
	// Bootstrap assembled by testBootstrap lacks permissionPolicy, which
	// ReloadPolicy treats as "no policy loaded" and skips — the exact gap
	// this test guards.
	packages := permission.NewPackageSet()
	permission.AttachPackages(ctx, packages, t.TempDir(), rulesDir, permission.PackageLoadOptions{Enabled: true, Builtin: true}, slog.Default())
	policy := permission.Policy{
		Packages:   packages,
		Mode:       permission.ModeDangerOnly,
		UserIntent: true,
	}
	b := &Bootstrap{
		ProcessRuntime:   proc,
		Registry:         agent.NewToolRegistry(),
		SteerCell:        agent.NewSteerCell(),
		WorkspaceRoot:    t.TempDir(),
		Policy:           wirePolicy(policy),
		permissionPolicy: &policy,
		approvalMode:     permission.ModeDangerOnly,
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(64))
	t.Cleanup(broker.Close)
	svc := NewSingletonWorkspaceService(b, broker, SessionServiceConfig{RulesDir: rulesDir})
	t.Cleanup(func() { _ = svc.Shutdown(ctx) })

	// Before install: go mod download has no rule (baseline).
	ev := b.Policy.Evaluate(domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: json.RawMessage(`{"command":"go mod download x"}`)},
		Risk: domain.R2,
	})
	if ev.Source == permission.SourceRule {
		t.Fatalf("before install: verdict = %s from %s, want no rule", ev.Decision, ev.Source)
	}

	// Install go-toolchain: policy must see it immediately.
	info, err := svc.InstallRulePack(ctx, "go-toolchain")
	if err != nil {
		t.Fatalf("InstallRulePack: %v", err)
	}
	if !info.Installed {
		t.Fatalf("install info = %+v", info)
	}
	ev = b.Policy.Evaluate(domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: json.RawMessage(`{"command":"go mod download x"}`)},
		Risk: domain.R2,
	})
	if ev.Source != permission.SourceRule || ev.Decision != domain.DecisionAllow || !ev.Grant.Unsandboxed {
		t.Fatalf("after install: verdict = %s (%s) grant=%+v, want rule allow + unsandboxed", ev.Decision, ev.Source, ev.Grant)
	}

	// Uninstall: policy reverts immediately.
	if err := svc.UninstallRulePack(ctx, "go-toolchain"); err != nil {
		t.Fatalf("UninstallRulePack: %v", err)
	}
	ev = b.Policy.Evaluate(domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: json.RawMessage(`{"command":"go mod download x"}`)},
		Risk: domain.R2,
	})
	if ev.Source == permission.SourceRule {
		t.Fatalf("after uninstall: verdict = %s from %s, want no rule", ev.Decision, ev.Source)
	}
}

// TestSyncMCPToolsRemovesStaleTools locks the registry diff: with no MCP
// manager (or no live servers), stale mcp__ tools disappear while
// built-ins stay untouched.
func TestSyncMCPToolsRemovesStaleTools(t *testing.T) {
	store, err := session.OpenSQLiteStore(context.Background(), filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	b := testBootstrap(store, fakes.NewFakeModel())

	stubDef := func(name string) domain.ToolDefinition {
		return domain.ToolDefinition{Name: name, InputSchema: json.RawMessage(`{"type":"object"}`)}
	}
	stale := fakes.NewFakeTool(stubDef("mcp__ghost__echo"), domain.ToolResult{})
	builtin := fakes.NewFakeTool(stubDef("my_custom_tool"), domain.ToolResult{})
	if err := b.Registry.Register(stale); err != nil {
		t.Fatalf("register stale: %v", err)
	}
	if err := b.Registry.Register(builtin); err != nil {
		t.Fatalf("register builtin: %v", err)
	}

	b.SyncMCPTools() // no MCP manager → every mcp__ tool is stale

	if _, ok := b.Registry.Lookup("mcp__ghost__echo"); ok {
		t.Fatal("stale mcp tool survived SyncMCPTools")
	}
	if _, ok := b.Registry.Lookup("my_custom_tool"); !ok {
		t.Fatal("non-mcp tool was unregistered")
	}
}
