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
	"fmt"
	"reflect"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/mcp"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// ConfigApplyReport classifies one hot-applied configuration change by
// when each section takes effect, so the UI can tell the user precisely
// what "saved" means (docs: PUT /v1/config).
type ConfigApplyReport struct {
	// Immediate sections were actively re-applied: policy rebuilt, MCP
	// servers reconciled, prompt builder swapped.
	Immediate []string `json:"immediate"`
	// NextTurn sections ride the atomic Resolved() swap: every component
	// on the turn-construction path reads them fresh next turn.
	NextTurn []string `json:"next_turn"`
	// Restart sections are frozen into process-level resources (stores,
	// tracing provider, pipelines, tool registrations) and require a
	// process restart.
	Restart []string `json:"restart"`
}

// classifyConfigChanges diffs two resolved configs section by section.
// Only changed sections appear in the report.
func classifyConfigChanges(prev, next *config.ResolvedConfig) ConfigApplyReport {
	var r ConfigApplyReport
	if prev == nil || next == nil {
		r.Restart = []string{"all"}
		return r
	}
	// Actively re-applied below (ApplyConfig).
	if prev.Approval.Mode != next.Approval.Mode {
		r.Immediate = append(r.Immediate, "approval.mode")
	}
	if !reflect.DeepEqual(prev.Tools.PathExtra, next.Tools.PathExtra) {
		r.Immediate = append(r.Immediate, "tools.path_extra")
	}
	// The remembered-store switch opens/closes a process-level store;
	// the other rule toggles ride ReloadPolicy.
	if prev.Rules.PersistRemembered != next.Rules.PersistRemembered {
		r.Restart = append(r.Restart, "rules.persist_remembered")
	}
	if prev.Rules.Enabled != next.Rules.Enabled ||
		prev.Rules.Builtin != next.Rules.Builtin ||
		prev.Rules.Project != next.Rules.Project ||
		prev.Rules.ProjectAllow != next.Rules.ProjectAllow {
		r.Immediate = append(r.Immediate, "rules")
	}
	if !reflect.DeepEqual(prev.MCP.Servers, next.MCP.Servers) {
		r.Immediate = append(r.Immediate, "mcp_servers")
	}
	if prev.Prompt != next.Prompt {
		r.Immediate = append(r.Immediate, "prompt")
	}
	// Read fresh at every turn construction.
	if prev.Default != next.Default {
		r.NextTurn = append(r.NextTurn, "default")
	}
	if !reflect.DeepEqual(prev.Providers, next.Providers) {
		r.NextTurn = append(r.NextTurn, "providers")
	}
	// limits is read per turn, except the two byte caps frozen into the
	// run_cmd tool and the artifact store at assembly.
	if prev.Limits.MaxInputTokens != next.Limits.MaxInputTokens ||
		prev.Limits.MaxOutputTokens != next.Limits.MaxOutputTokens ||
		prev.Limits.MaxEstimatedCostUSD != next.Limits.MaxEstimatedCostUSD ||
		prev.Limits.MaxTokens != next.Limits.MaxTokens {
		r.NextTurn = append(r.NextTurn, "limits")
	}
	if prev.Limits.MaxToolOutputBytes != next.Limits.MaxToolOutputBytes ||
		prev.Limits.MaxArtifactBytes != next.Limits.MaxArtifactBytes {
		r.Restart = append(r.Restart, "limits byte caps")
	}
	if !reflect.DeepEqual(prev.Context, next.Context) {
		r.NextTurn = append(r.NextTurn, "context")
	}
	// The main loop reads runaway per turn; the sub-agent factory captured
	// it at assembly.
	if !reflect.DeepEqual(prev.Runaway, next.Runaway) {
		r.NextTurn = append(r.NextTurn, "runaway")
		r.Restart = append(r.Restart, "runaway (subagent)")
	}
	// subagent.model is published per turn; the token caps are frozen
	// into the child-run factory at assembly.
	if !reflect.DeepEqual(prev.Subagent.Model, next.Subagent.Model) {
		r.NextTurn = append(r.NextTurn, "subagent.model")
	}
	if prev.Subagent.Enabled != next.Subagent.Enabled {
		r.Restart = append(r.Restart, "subagent.enabled")
	}
	if prev.Subagent.MaxTokens != next.Subagent.MaxTokens ||
		prev.Subagent.MaxOutputTokens != next.Subagent.MaxOutputTokens {
		r.Restart = append(r.Restart, "subagent budgets")
	}
	// Tracing cost rates feed the per-turn budget loop; the provider
	// connection itself is process-level.
	if prev.Tracing.CostInputPerMTok != next.Tracing.CostInputPerMTok ||
		prev.Tracing.CostOutputPerMTok != next.Tracing.CostOutputPerMTok {
		r.NextTurn = append(r.NextTurn, "tracing cost rates")
	}
	if prev.Tracing.Host != next.Tracing.Host ||
		prev.Tracing.PublicKey != next.Tracing.PublicKey ||
		prev.Tracing.SecretKey != next.Tracing.SecretKey ||
		prev.Tracing.Environment != next.Tracing.Environment ||
		prev.Tracing.IncludeContent != next.Tracing.IncludeContent ||
		prev.Tracing.UserID != next.Tracing.UserID ||
		prev.Tracing.Enabled != next.Tracing.Enabled {
		r.Restart = append(r.Restart, "tracing")
	}
	// Frozen into process-level resources at startup.
	if !reflect.DeepEqual(prev.Memory, next.Memory) {
		r.Restart = append(r.Restart, "memory")
	}
	// The disabled-name set is pushed into every assembled loader by
	// ApplyConfig (Immediate); enabled/extra_roots stay frozen into the
	// per-workspace assembly and still require a restart.
	if !reflect.DeepEqual(prev.Skills.Disabled, next.Skills.Disabled) {
		r.Immediate = append(r.Immediate, "skills.disabled")
	}
	if prev.Skills.Enabled != next.Skills.Enabled ||
		!reflect.DeepEqual(prev.Skills.ExtraRoots, next.Skills.ExtraRoots) {
		r.Restart = append(r.Restart, "skills")
	}
	if !reflect.DeepEqual(prev.Image, next.Image) {
		r.Restart = append(r.Restart, "image")
	}
	// Browser tool registration and Chrome path are frozen at assembly;
	// all knobs (idle TTL, viewport, timeout, quality) are baked into the
	// tool instance. Changing any of them requires a restart.
	if !reflect.DeepEqual(prev.Browser, next.Browser) {
		r.Restart = append(r.Restart, "browser")
	}
	// The share listener is runtime-managed: ApplyConfig reconciles it
	// immediately (start/stop/rebind), no restart required. The runtime
	// toggle writes through to share.enabled, so this reconcile is the
	// single path that starts or stops the listener.
	if prev.Share != next.Share {
		r.Immediate = append(r.Immediate, "share")
	}
	// Storage is not diffed: the loom home derives from the config file
	// location (fixed per process), so it can never change via hot-apply.
	if prev.Logging != next.Logging {
		r.Restart = append(r.Restart, "logging")
	}
	if !reflect.DeepEqual(prev.UI, next.UI) {
		r.Restart = append(r.Restart, "ui")
	}
	if !reflect.DeepEqual(prev.Workspaces, next.Workspaces) {
		r.Restart = append(r.Restart, "workspaces")
	}
	return r
}

// ApplyConfig hot-applies a freshly loaded configuration: the resolved
// pointer is swapped (next-turn sections take effect by themselves), then
// the immediate sections are actively re-applied — policy chains rebuilt,
// MCP servers reconciled and re-registered per workspace, prompt builders
// reassembled. The returned report classifies every changed section.
func (s *SessionService) ApplyConfig(ctx context.Context, next *config.ResolvedConfig) ConfigApplyReport {
	s.applyMu.Lock()
	defer s.applyMu.Unlock()
	prev := s.proc.Resolved()
	report := classifyConfigChanges(prev, next)

	s.proc.SwapResolved(next)
	s.proc.SetConfiguredDefault(ctx, next.Default)

	// MCP: reconcile the process-level manager, then re-sync every
	// assembled workspace's tool registry.
	if report.hasImmediate("mcp_servers") {
		mgr := s.proc.MCP()
		if mgr == nil && len(next.MCP.Servers) > 0 {
			mgr = mcp.NewManager(s.logger)
			s.proc.SetMCPManager(mgr)
		}
		if mgr != nil {
			mgr.Reconcile(ctx, next.MCP.Servers)
		}
	}

	if report.hasImmediate("share") && s.shareEndpoint != nil {
		if err := s.shareEndpoint.Apply(next.Share.Enabled, next.Share.Listen); err != nil && s.logger != nil {
			s.logger.Warn("hot-apply: share endpoint reconcile failed", "error", err)
		}
	}

	// PATH augmentation is idempotent and rebuilds from the
	// pre-augmentation base, so changed path_extra entries take effect for
	// subsequently spawned commands without a restart.
	if report.hasImmediate("tools.path_extra") {
		added := process.AugmentProcessPATH(next.Tools.PathExtra)
		if s.logger != nil {
			s.logger.Info("hot-apply: re-augmented process PATH", "added", added)
		}
	}

	policyChanged := report.hasImmediate("approval.mode") || report.hasImmediate("rules")
	promptChanged := report.hasImmediate("prompt")
	skillsDisabledChanged := report.hasImmediate("skills.disabled")
	for _, b := range s.registry.Bootstraps() {
		if policyChanged {
			if err := b.SetApprovalMode(ctx, next.Approval.Mode); err != nil && s.logger != nil {
				s.logger.Warn("hot-apply: policy rebuild failed", "workspace", b.WorkspaceRoot, "error", err)
			}
		}
		b.SyncMCPTools()
		if promptChanged {
			b.RebuildPrompt(ctx)
		}
		if skillsDisabledChanged && b.Skills != nil {
			b.Skills.Loader.SetDisabled(next.Skills.Disabled)
			b.Skills.Catalog.Store(b.Skills.Loader.Load(ctx))
		}
	}
	if len(report.Immediate)+len(report.NextTurn)+len(report.Restart) > 0 && s.logger != nil {
		s.logger.Info("config hot-applied",
			"immediate", report.Immediate, "next_turn", report.NextTurn, "restart", report.Restart)
	}
	return report
}

func (r ConfigApplyReport) hasImmediate(name string) bool {
	for _, n := range r.Immediate {
		if n == name {
			return true
		}
	}
	return false
}

// ToolchainReport aliases the process-level PATH-augmentation snapshot so
// client frontends reference the canonical shape through the app layer
// (docs/SERVE_DESIGN.md §10 type-identity rule).
type ToolchainReport = process.ToolchainReport

// ToolchainEnvironment returns the cached PATH-augmentation report behind
// the settings "development environment" card; nil when the runtime never
// augmented the PATH (bare test runners).
func (s *SessionService) ToolchainEnvironment() *process.ToolchainReport {
	return process.CurrentToolchainReport()
}

// MCPServers returns the live status of every configured MCP server
// (process level; empty when none are configured).
func (s *SessionService) MCPServers() []mcp.ServerStatus {
	if mgr := s.proc.MCP(); mgr != nil {
		return mgr.Servers()
	}
	return nil
}

// ReconnectMCPServer drops and re-establishes one server's connection
// from the live config, then re-syncs every workspace's tool registry.
// The returned status carries the failure reason when the reconnect did
// not succeed.
func (s *SessionService) ReconnectMCPServer(ctx context.Context, name string) (mcp.ServerStatus, error) {
	s.applyMu.Lock()
	defer s.applyMu.Unlock()
	resolved := s.proc.Resolved()
	if resolved == nil {
		return mcp.ServerStatus{}, fmt.Errorf("no configuration loaded")
	}
	cfg, ok := resolved.MCP.Servers[name]
	if !ok {
		return mcp.ServerStatus{}, fmt.Errorf("unknown mcp server %q", name)
	}
	mgr := s.proc.MCP()
	if mgr == nil {
		mgr = mcp.NewManager(s.logger)
		s.proc.SetMCPManager(mgr)
	}
	status := mgr.Add(ctx, name, cfg)
	for _, b := range s.registry.Bootstraps() {
		b.SyncMCPTools()
	}
	return status, nil
}
