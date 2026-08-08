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
// Created: 2026/07/23

// Package app provides the application-level composition root and session
// controller for the Loom runtime. It wires together the agent loop,
// tool registry, session store, artifact store, and runtime event broker.
package app

import (
	"context"
	"fmt"
	"log/slog"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/exsession"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/tool/imagegen"
	"github.com/liubang/playground/go/pl/loom/internal/tool/lint"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
	"github.com/liubang/playground/go/pl/loom/internal/tool/webfetch"
	"github.com/liubang/playground/go/pl/loom/internal/tool/websearch"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// bootstrapSubagentFailpoint is a test-only seam (review M22 regression):
// when non-nil, NewWorkspaceBootstrap invokes it at the top of the
// sub-agent assembly and aborts with its error. The real assembly
// failures (buildSubagentRegistry & co.) are unreachable through the
// public API — every constructor involved only rejects nil arguments,
// which bootstrap never passes — so the cleanup path needs an injected
// fault to be regression-tested.
var bootstrapSubagentFailpoint func() error

// BootstrapConfig carries the workspace-specific inputs for assembling one
// workspace-scoped Bootstrap on top of a shared ProcessRuntime
// (docs/WORKSPACE_DESIGN.md §5.1).
type BootstrapConfig struct {
	// WorkspaceRoot is the absolute path to the workspace directory.
	WorkspaceRoot string
	// WorkspaceID is the owning workspace entity's ID (docs/WORKSPACE_DESIGN
	// W1); zero only in hand-assembled test bootstraps.
	WorkspaceID domain.WorkspaceID
}

// Bootstrap assembles the workspace-scoped runtime components for one
// workspace (docs/WORKSPACE_DESIGN.md §5.1): the path validator, process
// runner, base tool registry, approval policy, prompt builder, skills, and
// sub-agent runtime — all bound to WorkspaceRoot. It embeds the shared
// *ProcessRuntime, so process-level resources (Store/Artifact/Resolved/
// Recorder/MCPManager/Memory*/...) stay reachable through the workspace
// handle with no call-site churn. One Bootstrap exists per workspace; the
// WorkspaceRegistry manages them.
type Bootstrap struct {
	*ProcessRuntime

	// WorkspaceID is the owning workspace entity's ID (W1).
	WorkspaceID   domain.WorkspaceID
	WorkspaceRoot string
	Registry      *agent.ToolRegistry
	// Policy is the assembled decider chain (docs/PERMISSION_DESIGN.md
	// §4.4): rules → danger → session → mode-aware baseline. Read it via
	// CurrentPolicy: ReloadPolicy swaps it when rules change at runtime.
	Policy           agent.Policy
	permissionPolicy *permission.Policy
	approvalMode     permission.ApprovalMode
	// policyMu guards Policy/permissionPolicy/approvalMode against
	// concurrent ReloadPolicy/SetApprovalMode writes and
	// run-construction/ListRules reads.
	policyMu sync.RWMutex
	// PromptBuilder is set at assembly; reads go through CurrentPrompt,
	// hot-reload rebuilds through RebuildPrompt — both under promptMu.
	PromptBuilder agent.PromptBuilder
	// promptMu guards PromptBuilder swaps.
	promptMu sync.RWMutex
	// skillsPromptOpt caches the skills catalog prompt option captured at
	// assembly (WireSkills also registers read_skill, which stays fixed);
	// prompt rebuilds reuse it.
	skillsPromptOpt prompt.Option
	Validator       *workspace.PathValidator
	Runner          *process.Runner
	// FileStateBook is the shared read/write hash tracker used by
	// read_file and edit for drift detection; rewind restoration updates
	// it so a post-rewind edit measures drift from the restored content.
	FileStateBook *workspace.FileStateBook
	// GoalCell ferries update_goal mutations from the tool to each turn's
	// agent loop.
	GoalCell *agent.GoalCell
	// PlanCell ferries update_plan snapshots from the tool to each turn's
	// agent loop.
	PlanCell *agent.PlanCell
	// SteerCell ferries user messages submitted while a turn is busy to the
	// loop's next model call (docs/STEER_DESIGN.md). Shared across turns so
	// leftovers relay into the next turn's prompt.
	SteerCell *agent.SteerCell
	// SubagentModels is the delegate_task model mailbox: the controller
	// publishes each turn's model selection, the tool reads it at
	// execution time (docs/SUBAGENT_DESIGN.md D7). Nil when the
	// sub-agent is disabled (subagent.enabled=false).
	SubagentModels *subagent.ModelSource
	// SubagentFactory is the delegate_task child-loop factory, exposed
	// so the frontend wiring can attach a lifecycle observer
	// (WireSubagentObserver). Nil when the sub-agent is disabled.
	SubagentFactory *subagent.Factory
	// SubagentManager is the V2 async delegation runtime; Close
	// drains its in-flight children before the store shuts down. Nil
	// when the sub-agent is disabled.
	SubagentManager *subagent.Manager
	// SessionManager owns every exec_session/write_stdin background
	// process; Close reclaims surviving process groups before the store
	// shuts down.
	SessionManager *exsession.Manager
	// Skills exposes the skills loader/catalog shared by the prompt
	// provider and the read_skill tool; nil when skills are disabled
	// (skills.enabled=false or the built-in system prompt is off).
	Skills *SkillsHandle
}

// NewWorkspaceBootstrap assembles the workspace-scoped runtime components
// for one workspace on top of the shared ProcessRuntime
// (docs/WORKSPACE_DESIGN.md §5.1). The ProcessRuntime owns the session/
// artifact stores and tracing; a workspace-assembly failure never closes
// them. The caller closes the returned Bootstrap on workspace teardown.
func NewWorkspaceBootstrap(ctx context.Context, proc *ProcessRuntime, cfg BootstrapConfig) (_ *Bootstrap, retErr error) {
	if proc == nil || proc.Resolved() == nil {
		return nil, fmt.Errorf("process runtime is required")
	}
	resolved := proc.Resolved()
	logger := proc.Logger
	if logger == nil {
		logger = slog.Default()
	}

	// Create workspace validator
	validator, err := workspace.NewPathValidator(cfg.WorkspaceRoot)
	if err != nil {
		return nil, fmt.Errorf("create path validator: %w", err)
	}

	// Create process runner. The env allowlist additionally permits the Go
	// toolchain variables the lint tool overrides (the sandbox only allows
	// writes inside the workspace and the temp dir, so GOCACHE must be
	// redirectable for go vet to work at all). SessionEnv injects the loom
	// attribution variables (see process.LoomSessionEnv) into every spawned
	// command so downstream CLIs can attribute traffic to this session.
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
		EnvAllowlist: []string{
			"PATH", "LANG", "LC_ALL", "TMPDIR", "HOME",
			"GOCACHE", "GOPATH", "GOMODCACHE", "GOPROXY", "GOSUMDB", "GOFLAGS",
		},
		// Per-session attribution rides the turn context (Controller
		// path, docs/SERVE_DESIGN.md §4.3); the process-level atomic
		// value (ProcessRuntime.SessionEnv) remains the fallback for
		// context-less paths (headless runAgent, runner tests).
		SessionEnv: func(ctx context.Context) map[string]string {
			if env := process.SessionEnvFromContext(ctx); len(env) > 0 {
				return env
			}
			return proc.SessionEnv.Get()
		},
	})
	if err != nil {
		return nil, fmt.Errorf("create process runner: %w", err)
	}

	// Create tool registry and register built-in tools. The file-state
	// book is shared by read_file (records hashes) and edit (checks
	// drift) — including the sub-agent's read-only registry.
	book := workspace.NewFileStateBook()
	registry := agent.NewToolRegistry()
	goalCell := agent.NewGoalCell()
	planCell := agent.NewPlanCell()
	steerCell := agent.NewSteerCell()
	sessionManager, err := exsession.NewManager(runner, proc.Artifact, exsession.DefaultIdleTTL)
	if err != nil {
		return nil, fmt.Errorf("create session manager: %w", err)
	}
	// Review M22: EVERY failure past this point must release the session
	// manager — otherwise its reaper goroutine (and any staged sessions)
	// leak. A single defer keyed on the named return replaces per-path
	// Close calls, which had already drifted (the sub-agent assembly
	// failures returned without closing). On success ownership passes to
	// the returned Bootstrap.
	defer func() {
		if retErr != nil {
			sessionManager.Close()
		}
	}()
	if err := registerBuiltinTools(registry, validator, runner, proc.Artifact, resolved.Limits.MaxToolOutputBytes, goalCell, planCell, proc.Questioner, book, sessionManager, resolved.Image); err != nil {
		return nil, fmt.Errorf("register tools: %w", err)
	}

	// Approval policy: the decider chain (rules → danger → session →
	// mode-aware baseline). Session/user-layer rules and the remembered
	// store are process-shared (held by the ProcessRuntime, WORKSPACE_DESIGN
	// D4); the project layer is loaded per workspace from cfg.WorkspaceRoot.
	policy := permission.DefaultPolicy()
	policy.Session = proc.SessionRules
	policy = permission.AttachRules(ctx, policy, cfg.WorkspaceRoot, resolved.Storage.RulesDir(), permission.RuleLoadOptions{
		Enabled:      resolved.Rules.Enabled,
		Builtin:      resolved.Rules.Builtin,
		Project:      resolved.Rules.Project,
		ProjectAllow: resolved.Rules.ProjectAllow,
	}, logger)
	decider := policy.Decider(resolved.Approval.Mode)

	// Skills assembly (read_skill tool + catalog prompt option) happens
	// once here; the option is cached on the Bootstrap so prompt rebuilds
	// (config hot-reload) reuse it without re-scanning or re-registering.
	// Inside the system-prompt guard so a catalog-less read_skill is
	// never registered. The catalog budget tracks the startup model's
	// window; switching models does not rebuild it (catalogs are far
	// smaller than any window).
	var (
		skills    *SkillsHandle
		skillsOpt prompt.Option
	)
	if !resolved.Prompt.DisableBuiltin {
		opt, skillsHandle, err := WireSkills(registry, cfg.WorkspaceRoot, defaultContextWindow(resolved), resolved.Skills, resolved.Storage.SkillsDir(), resolved.Prompt.DisableBuiltin, logger)
		if err != nil {
			return nil, fmt.Errorf("wire skills: %w", err)
		}
		skillsOpt = opt
		skills = skillsHandle
	}

	// MCP tools: the shared process-level manager (ProcessRuntime) owns the
	// server subprocesses (WORKSPACE_DESIGN D2); each workspace registers the
	// adapted tools into its own registry. Tools are stateless adapters over
	// the shared manager, so registering them per workspace is safe. Later
	// changes (config hot-reload) flow through SyncMCPTools.
	if mcpMgr := proc.MCP(); mcpMgr != nil {
		for _, tool := range mcpMgr.Tools() {
			if err := registry.Register(tool); err != nil {
				logger.Warn("mcp: failed to register tool", "tool", tool.Definition().Name, "error", err)
			}
		}
	}

	// Sub-agent delegation (docs/SUBAGENT_DESIGN.md): register
	// delegate_task against a read-only child registry. The child's
	// system prompt is the built-in prompt plus the researcher
	// instructions — no skills catalog, no managed prompt.
	var (
		subagentModels  *subagent.ModelSource
		subagentFactory *subagent.Factory
		subagentManager *subagent.Manager
	)
	if resolved.Subagent.Enabled {
		// Test-only seam (M22 regression): exercises the failure cleanup
		// path — the real assembly failures below are unreachable through
		// the public API because every constructor involved only rejects
		// nil arguments, which bootstrap never passes.
		if bootstrapSubagentFailpoint != nil {
			if err := bootstrapSubagentFailpoint(); err != nil {
				return nil, fmt.Errorf("sub-agent assembly: %w", err)
			}
		}
		researcherRegistry, err := buildSubagentRegistry(validator, runner, proc.Artifact, book)
		if err != nil {
			return nil, fmt.Errorf("build sub-agent registry: %w", err)
		}
		childLimits := resolved.Limits
		if resolved.Subagent.MaxTokens > 0 {
			childLimits.MaxTokens = resolved.Subagent.MaxTokens
		}
		if resolved.Subagent.MaxOutputTokens > 0 {
			childLimits.MaxOutputTokens = resolved.Subagent.MaxOutputTokens
		}
		subagentModels = &subagent.ModelSource{}

		// Assemble the role specs: researcher (read-only, R1) and coder
		// (read-write, R3). The coder registry adds edit/write/run_cmd/lint.
		researcherPrompt := prompt.NewBuilder(cfg.WorkspaceRoot,
			prompt.WithExtraInstructions(subagent.ResearcherInstructions),
			prompt.WithRulesProvider(prompt.NewFileRulesProvider(cfg.WorkspaceRoot, resolved.Storage.LoomMDPath())))
		researcherSpec := &subagent.RoleSpec{
			Registry: researcherRegistry,
			Prompt:   researcherPrompt,
			Risk:     domain.R1,
		}

		coderRegistry, err := buildCoderRegistry(validator, runner, proc.Artifact, book, int(resolved.Limits.MaxToolOutputBytes))
		if err != nil {
			return nil, fmt.Errorf("build coder registry: %w", err)
		}
		coderPrompt := prompt.NewBuilder(cfg.WorkspaceRoot,
			prompt.WithExtraInstructions(subagent.CoderInstructions),
			prompt.WithRulesProvider(prompt.NewFileRulesProvider(cfg.WorkspaceRoot, resolved.Storage.LoomMDPath())))
		coderSpec := &subagent.RoleSpec{
			Registry: coderRegistry,
			Prompt:   coderPrompt,
			Risk:     domain.R3,
		}

		roles := map[subagent.Role]*subagent.RoleSpec{
			subagent.RoleResearcher: researcherSpec,
			subagent.RoleCoder:      coderSpec,
		}

		factory := &subagent.Factory{
			Store:                proc.Store,
			Artifacts:            proc.Artifact,
			Recorder:             proc.Recorder,
			Logger:               logger,
			Registry:             researcherRegistry,
			Prompt:               researcherPrompt,
			Workspace:            cfg.WorkspaceRoot,
			WorkspaceID:          cfg.WorkspaceID,
			Limits:               childLimits,
			Runaway:              resolved.Runaway,
			Models:               subagentModels,
			CostInputUSDPerMTok:  resolved.Tracing.CostInputPerMTok,
			CostOutputUSDPerMTok: resolved.Tracing.CostOutputPerMTok,
		}

		// V2 Manager: async spawn/wait/resume for both roles.
		// validator implements agent.FileStateReader for RecoverRun.
		manager, err := subagent.NewManager(factory, roles, validator)
		if err != nil {
			return nil, fmt.Errorf("create sub-agent manager: %w", err)
		}
		factory.Manager = manager
		subagentManager = manager

		delegateTool, err := subagent.NewDelegateTaskTool(factory)
		if err != nil {
			return nil, fmt.Errorf("delegate_task: %w", err)
		}
		if err := registry.Register(delegateTool); err != nil {
			return nil, fmt.Errorf("register delegate_task: %w", err)
		}

		// V2 companion tools: wait_subagent and resume_subagent.
		waitTool, err := subagent.NewWaitSubagentTool(manager)
		if err != nil {
			return nil, fmt.Errorf("wait_subagent: %w", err)
		}
		if err := registry.Register(waitTool); err != nil {
			return nil, fmt.Errorf("register wait_subagent: %w", err)
		}
		resumeTool, err := subagent.NewResumeSubagentTool(manager)
		if err != nil {
			return nil, fmt.Errorf("resume_subagent: %w", err)
		}
		if err := registry.Register(resumeTool); err != nil {
			return nil, fmt.Errorf("register resume_subagent: %w", err)
		}

		subagentFactory = factory
		logger.Info("sub-agent delegation enabled", "roles", "researcher+coder", "async", true)
	}

	// Memory tools: the store is process-shared (ProcessRuntime,
	// WORKSPACE_DESIGN D5); register the tools into each workspace's
	// registry. The prompt-side injection is part of buildPrompt (the
	// memoryPromptWrapper), so prompt rebuilds keep it.
	if proc.MemoryStore != nil {
		if err := registerMemoryTools(registry, proc.MemoryStore); err != nil {
			logger.Warn("memory tools registration failed", "error", err)
		}
	}

	b := &Bootstrap{
		ProcessRuntime:   proc,
		WorkspaceID:      cfg.WorkspaceID,
		WorkspaceRoot:    cfg.WorkspaceRoot,
		Registry:         registry,
		Policy:           decider,
		permissionPolicy: &policy,
		approvalMode:     resolved.Approval.Mode,
		skillsPromptOpt:  skillsOpt,
		Validator:        validator,
		Runner:           runner,
		FileStateBook:    book,
		GoalCell:         goalCell,
		PlanCell:         planCell,
		SteerCell:        steerCell,
		SubagentModels:   subagentModels,
		SubagentFactory:  subagentFactory,
		SubagentManager:  subagentManager,
		SessionManager:   sessionManager,
		Skills:           skills,
	}
	b.PromptBuilder = b.buildPrompt(ctx, resolved)
	return b, nil
}

// buildPrompt assembles the system prompt builder from the given resolved
// config. Called at assembly and by RebuildPrompt on config hot-reload.
func (b *Bootstrap) buildPrompt(ctx context.Context, resolved *config.ResolvedConfig) agent.PromptBuilder {
	if resolved.Prompt.DisableBuiltin {
		return nil
	}
	logger := b.Logger
	if logger == nil {
		logger = slog.Default()
	}
	promptOpts := []prompt.Option{
		prompt.WithExtraInstructions(resolved.Prompt.Extra),
		prompt.WithRulesProvider(prompt.NewFileRulesProvider(b.WorkspaceRoot, resolved.Storage.LoomMDPath())),
	}
	// traceCfg is rebuilt from the resolved config for ResolveManagedPrompt;
	// the recorder/provider themselves are process-level (ProcessRuntime).
	traceCfg := resolved.Tracing
	traceCfg.Logger = logger
	traceCfg.Release = b.Version
	if opt := ResolveManagedPrompt(ctx, resolved.Prompt.Managed, traceCfg, resolved.Storage.SessionsDir(), logger); opt != nil {
		promptOpts = append(promptOpts, opt)
	}
	if b.skillsPromptOpt != nil {
		promptOpts = append(promptOpts, b.skillsPromptOpt)
	}
	var pb agent.PromptBuilder = prompt.NewBuilder(b.WorkspaceRoot, promptOpts...)
	if b.MemoryStore != nil {
		pb = &memoryPromptWrapper{inner: pb, store: b.MemoryStore, logger: logger}
	}
	return pb
}

// CurrentPrompt returns the active prompt builder; safe for concurrent
// use with RebuildPrompt.
func (b *Bootstrap) CurrentPrompt() agent.PromptBuilder {
	b.promptMu.RLock()
	defer b.promptMu.RUnlock()
	return b.PromptBuilder
}

// RebuildPrompt reassembles the prompt builder from the live resolved
// config (extra instructions, managed prompt, memory wrapper) and swaps
// it in; subsequent turns build their system prompt from the new values.
func (b *Bootstrap) RebuildPrompt(ctx context.Context) {
	pb := b.buildPrompt(ctx, b.Resolved())
	b.promptMu.Lock()
	b.PromptBuilder = pb
	b.promptMu.Unlock()
}

// CurrentPolicy returns the active decider chain; safe for concurrent
// use with ReloadPolicy.
func (b *Bootstrap) CurrentPolicy() agent.Policy {
	b.policyMu.RLock()
	defer b.policyMu.RUnlock()
	return b.Policy
}

// CurrentRules returns the active declarative rule set (nil when rules
// are disabled); safe for concurrent use with ReloadPolicy.
func (b *Bootstrap) CurrentRules() *permission.RuleSet {
	b.policyMu.RLock()
	defer b.policyMu.RUnlock()
	if b.permissionPolicy == nil {
		return nil
	}
	return b.permissionPolicy.Rules
}

// ReloadPolicy re-reads rules from files and the remembered store,
// rebuilds the decider chain, and replaces b.Policy so subsequent
// evaluations reflect the updated rule set. Runs already in flight keep
// the decider they captured at construction. Rule-load switches and the
// approval mode read from the live resolved config, so a hot-applied
// config takes effect here too.
func (b *Bootstrap) ReloadPolicy(ctx context.Context) error {
	resolved := b.Resolved()
	b.policyMu.RLock()
	if b.permissionPolicy == nil || resolved == nil {
		b.policyMu.RUnlock()
		return nil
	}
	session := b.permissionPolicy.Session
	b.policyMu.RUnlock()
	policy := permission.DefaultPolicy()
	policy.Session = session
	// File and store I/O happens outside the lock; only the swap is
	// serialized against readers.
	policy = permission.AttachRules(ctx, policy, b.WorkspaceRoot, resolved.Storage.RulesDir(), permission.RuleLoadOptions{
		Enabled:      resolved.Rules.Enabled,
		Builtin:      resolved.Rules.Builtin,
		Project:      resolved.Rules.Project,
		ProjectAllow: resolved.Rules.ProjectAllow,
	}, b.Logger)
	b.policyMu.Lock()
	defer b.policyMu.Unlock()
	*b.permissionPolicy = policy
	b.Policy = policy.Decider(b.approvalMode)
	return nil
}

// SetApprovalMode updates the baseline approval mode and rebuilds the
// decider chain so subsequent evaluations use it (config hot-reload).
func (b *Bootstrap) SetApprovalMode(ctx context.Context, mode permission.ApprovalMode) error {
	b.policyMu.Lock()
	b.approvalMode = mode
	b.policyMu.Unlock()
	return b.ReloadPolicy(ctx)
}

// SyncMCPTools reconciles the workspace registry with the shared MCP
// manager's live tool set: tools of removed/failed servers are
// unregistered, tools of newly connected servers registered. MCP tool
// names carry the mcp__{server}__ prefix, so registry diffing never
// touches built-in tools. Note: a turn that already captured a removed
// server's tool keeps the adapter, but its client is closed — an
// in-flight call to a removed server fails; the tool simply disappears
// from subsequent turns.
func (b *Bootstrap) SyncMCPTools() {
	want := make(map[string]domain.Tool)
	if mcpMgr := b.MCP(); mcpMgr != nil {
		for _, t := range mcpMgr.Tools() {
			want[t.Definition().Name] = t
		}
	}
	have := make(map[string]bool)
	for _, def := range b.Registry.List() {
		if strings.HasPrefix(def.Name, "mcp__") {
			have[def.Name] = true
		}
	}
	for name := range have {
		if _, ok := want[name]; !ok {
			b.Registry.Unregister(name)
		}
	}
	for name, t := range want {
		if !have[name] {
			if err := b.Registry.Register(t); err != nil && b.Logger != nil {
				b.Logger.Warn("mcp: failed to register tool", "tool", name, "error", err)
			}
		}
	}
}

// PublishSubagentSnapshot resolves the effective child-run model
// selection — the current turn's model, or the pinned subagent.model
// override — and posts it to the delegate_task mailbox. Shared by the
// interactive controller path and the headless runner so both behave
// identically (docs/SUBAGENT_DESIGN.md D7). A pinned model uses its own
// configured reasoning: the session reasoning override is the user's
// per-task intent for the MAIN agent and must not leak sideways.
func PublishSubagentSnapshot(src *subagent.ModelSource, resolved *config.ResolvedConfig, current config.ProviderModelRef, reasoning domain.ReasoningSpec, parentSession domain.SessionID) {
	if src == nil || resolved == nil {
		return
	}
	provider := resolved.ProviderByName(current.Provider)
	if provider == nil {
		return
	}
	meta, _ := resolved.ModelMeta(current)
	contextCfg := resolved.Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	snap := subagent.ModelSnapshot{
		Model:         provider.ModelFor(current.Model),
		ModelName:     current.Model,
		Window:        agent.NewWindowModel(meta.ContextWindow, resolved.Limits.MaxInputTokens, contextCfg),
		Reasoning:     reasoning,
		ParentSession: parentSession,
	}
	if pinned := resolved.Subagent.Model; pinned != nil {
		if pinProvider := resolved.ProviderByName(pinned.Provider); pinProvider != nil {
			pinMeta, _ := resolved.ModelMeta(*pinned)
			pinContextCfg := resolved.Context
			if pinMeta.WindowUtilization != nil {
				pinContextCfg.Utilization = *pinMeta.WindowUtilization
			}
			snap.Model = pinProvider.ModelFor(pinned.Model)
			snap.ModelName = pinned.Model
			snap.Window = agent.NewWindowModel(pinMeta.ContextWindow, resolved.Limits.MaxInputTokens, pinContextCfg)
			snap.Reasoning = pinMeta.Reasoning.DomainSpec()
		}
	}
	src.Set(snap)
}

// defaultContextWindow returns the startup model's context window (0 = the
// loop falls back to Limits.MaxInputTokens as the occupancy proxy).
func defaultContextWindow(resolved *config.ResolvedConfig) int64 {
	if meta, ok := resolved.ModelMeta(resolved.Default); ok {
		return meta.ContextWindow
	}
	return 0
}

// toolFactory is one named tool constructor in a registration table.
type toolFactory struct {
	name string
	mk   func() (domain.Tool, error)
}

// registerToolFactories builds and registers every tool in the table.
func registerToolFactories(registry *agent.ToolRegistry, tools []toolFactory) error {
	for _, tt := range tools {
		t, err := tt.mk()
		if err != nil {
			return fmt.Errorf("%s: %w", tt.name, err)
		}
		if err := registry.Register(t); err != nil {
			return fmt.Errorf("register %s: %w", tt.name, err)
		}
	}
	return nil
}

// readOnlyToolFactories is the shared read-only tool set: the main agent's
// baseline and both sub-agent registries start from it, so a new read-only
// tool is added in exactly one place (REVIEW R11).
func readOnlyToolFactories(validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, book *workspace.FileStateBook) []toolFactory {
	return []toolFactory{
		{"read_file", func() (domain.Tool, error) { return builtin.NewReadFileTool(validator, book) }},
		{"list_dir", func() (domain.Tool, error) { return builtin.NewListDirTool(validator) }},
		{"search", func() (domain.Tool, error) { return builtin.NewSearchTool(validator, runner) }},
		{"glob", func() (domain.Tool, error) { return builtin.NewGlobTool(validator, runner) }},
		{"view_image", func() (domain.Tool, error) { return builtin.NewViewImageTool(validator) }},
		{"git_status", func() (domain.Tool, error) { return gittools.NewGitStatusTool(validator) }},
		{"git_diff", func() (domain.Tool, error) { return gittools.NewGitDiffTool(validator) }},
		{"git_log", func() (domain.Tool, error) { return gittools.NewGitLogTool(validator) }},
		{"git_merge_base", func() (domain.Tool, error) { return gittools.NewGitMergeBaseTool(validator) }},
		{"git_blame", func() (domain.Tool, error) { return gittools.NewGitBlameTool(validator) }},
		{"web_fetch", func() (domain.Tool, error) { return webfetch.NewWebFetchTool(artStore) }},
		{"web_search", func() (domain.Tool, error) { return websearch.NewWebSearchTool() }},
	}
}

// registerMemoryTools registers the memory tools (list, read, search,
// add_note) with the tool registry.
func registerMemoryTools(registry *agent.ToolRegistry, store *memory.Store) error {
	return registerToolFactories(registry, []toolFactory{
		{"memory_list", func() (domain.Tool, error) { return memory.NewListTool(store) }},
		{"memory_read", func() (domain.Tool, error) { return memory.NewReadTool(store) }},
		{"memory_search", func() (domain.Tool, error) { return memory.NewSearchTool(store) }},
		{"memory_add_note", func() (domain.Tool, error) { return memory.NewAddNoteTool(store) }},
	})
}

func registerBuiltinTools(registry *agent.ToolRegistry, validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, maxOutputBytes int64, goalCell *agent.GoalCell, planCell *agent.PlanCell, questioner domain.Questioner, book *workspace.FileStateBook, sessionManager *exsession.Manager, imageCfg config.ResolvedImage) error {
	tools := append(readOnlyToolFactories(validator, runner, artStore, book),
		toolFactory{"edit", func() (domain.Tool, error) { return edit.NewEditTool(validator, book) }},
		toolFactory{"write", func() (domain.Tool, error) { return edit.NewWriteTool(validator) }},
		toolFactory{"lint", func() (domain.Tool, error) { return lint.NewLintTool(validator, runner) }},
	)
	if err := registerToolFactories(registry, tools); err != nil {
		return err
	}
	// run_cmd needs artifact store
	runCmd, err := command.NewRunCmdToolWithArtifacts(validator, runner, artStore, int(maxOutputBytes))
	if err != nil {
		return fmt.Errorf("run_cmd: %w", err)
	}
	if err := registry.Register(runCmd); err != nil {
		return fmt.Errorf("register run_cmd: %w", err)
	}
	// generate_image is registered only when the image section is enabled
	// (config.image): generation costs money per call, so an unconfigured
	// deployment must not advertise the tool to the model. Main-agent only
	// — sub-agent registries stay text-only by design.
	if imageCfg.Enabled && imageCfg.Generator != nil {
		genImage, err := imagegen.NewGenerateImageTool(imageCfg.Generator, artStore, imagegen.Options{
			Model:   imageCfg.Model,
			Size:    imageCfg.Size,
			Quality: imageCfg.Quality,
		})
		if err != nil {
			return fmt.Errorf("generate_image: %w", err)
		}
		if err := registry.Register(genImage); err != nil {
			return fmt.Errorf("register generate_image: %w", err)
		}
	}
	// exec_session/write_stdin share the process-level session manager:
	// interactive background processes (dev servers, REPLs) that the model
	// drives across multiple tool calls.
	execSession, err := exsession.NewExecSessionTool(validator, sessionManager)
	if err != nil {
		return fmt.Errorf("exec_session: %w", err)
	}
	if err := registry.Register(execSession); err != nil {
		return fmt.Errorf("register exec_session: %w", err)
	}
	writeStdin, err := exsession.NewWriteStdinTool(sessionManager)
	if err != nil {
		return fmt.Errorf("write_stdin: %w", err)
	}
	if err := registry.Register(writeStdin); err != nil {
		return fmt.Errorf("register write_stdin: %w", err)
	}
	updateGoal, err := agent.NewUpdateGoalTool(goalCell)
	if err != nil {
		return fmt.Errorf("update_goal: %w", err)
	}
	if err := registry.Register(updateGoal); err != nil {
		return fmt.Errorf("register update_goal: %w", err)
	}
	updatePlan, err := agent.NewUpdatePlanTool(planCell)
	if err != nil {
		return fmt.Errorf("update_plan: %w", err)
	}
	if err := registry.Register(updatePlan); err != nil {
		return fmt.Errorf("register update_plan: %w", err)
	}
	askUser, err := agent.NewAskUserTool(questioner)
	if err != nil {
		return fmt.Errorf("ask_user: %w", err)
	}
	if err := registry.Register(askUser); err != nil {
		return fmt.Errorf("register ask_user: %w", err)
	}
	return nil
}

// buildSubagentRegistry assembles the read-only tool set for
// delegate_task child runs (docs/SUBAGENT_DESIGN.md §4.2). Excluded by
// design: edit/write (writes), run_cmd/lint (process execution),
// update_goal/update_plan (parent-run state), ask_user (no one to
// answer), and delegate_task itself — recursion depth stays 1 by
// construction.
func buildSubagentRegistry(validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, book *workspace.FileStateBook) (*agent.ToolRegistry, error) {
	registry := agent.NewToolRegistry()
	if err := registerToolFactories(registry, readOnlyToolFactories(validator, runner, artStore, book)); err != nil {
		return nil, err
	}
	return registry, nil
}

// buildCoderRegistry assembles the read-write tool set for the coder
// sub-agent role. It carries every researcher tool plus edit, write,
// run_cmd, and lint — the tools that make code changes. Excluded by
// design: update_goal/update_plan (parent-run state), ask_user (no one
// to answer), exec_session/write_stdin (interactive sessions), and
// delegate_task itself (no recursion).
func buildCoderRegistry(validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, book *workspace.FileStateBook, maxOutputBytes int) (*agent.ToolRegistry, error) {
	// Start from the researcher (read-only) set and add the writable tools.
	registry := agent.NewToolRegistry()
	tools := append(readOnlyToolFactories(validator, runner, artStore, book),
		toolFactory{"edit", func() (domain.Tool, error) { return edit.NewEditTool(validator, book) }},
		toolFactory{"write", func() (domain.Tool, error) { return edit.NewWriteTool(validator) }},
		toolFactory{"lint", func() (domain.Tool, error) { return lint.NewLintTool(validator, runner) }},
	)
	if err := registerToolFactories(registry, tools); err != nil {
		return nil, err
	}
	// run_cmd needs the artifact store for output capture.
	runCmd, err := command.NewRunCmdToolWithArtifacts(validator, runner, artStore, maxOutputBytes)
	if err != nil {
		return nil, fmt.Errorf("run_cmd: %w", err)
	}
	if err := registry.Register(runCmd); err != nil {
		return nil, fmt.Errorf("register run_cmd: %w", err)
	}
	return registry, nil
}

// Close releases the workspace-scoped resources held by the Bootstrap: it
// drains in-flight sub-agents (child goroutines hold store references until
// they persist their final checkpoint) and reclaims surviving background
// process groups. The embedded ProcessRuntime is closed separately at
// process teardown (ProcessRuntime.Close), after every workspace Bootstrap.
func (b *Bootstrap) Close() {
	if b.SubagentManager != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		b.SubagentManager.Shutdown(ctx)
		cancel()
	}
	if b.SessionManager != nil {
		b.SessionManager.Close()
	}
}

// ResolveManagedPrompt fetches the Langfuse-managed system prompt when the
// config file names one and tracing is enabled. Any failure degrades to nil
// (built-in prompt) — a prompt-management outage must never block the agent.
// sessionsDir hosts the on-disk prompt cache (prompt_cache/).
func ResolveManagedPrompt(ctx context.Context, managed config.ManagedPrompt, traceCfg trace.Config, sessionsDir string, logger *slog.Logger) prompt.Option {
	if managed.Name == "" || !traceCfg.Enabled {
		return nil
	}
	label := managed.Label
	if label == "" {
		label = "production"
	}
	cacheDir := filepath.Join(sessionsDir, "prompt_cache")
	client := trace.NewPromptClient(traceCfg, cacheDir)
	mp, err := client.Get(ctx, managed.Name, label)
	if err != nil {
		logger.Warn("langfuse managed prompt unavailable, using built-in prompt",
			"name", managed.Name, "label", label, "error", err)
		return nil
	}
	// loom does not substitute Langfuse template variables; surface them
	// loudly so a templated prompt never ships silently unrendered.
	if vars := trace.PromptVariables(mp.Content); len(vars) > 0 {
		logger.Warn("langfuse managed prompt contains unsubstituted variables; they will appear verbatim in the system prompt",
			"name", mp.Name, "version", mp.Version, "variables", vars)
	}
	logger.Info("using langfuse managed prompt",
		"name", mp.Name, "version", mp.Version, "label", label, "fetched_at", mp.FetchedAt)
	return prompt.WithManagedBase(mp.Name, mp.Version, mp.Content)
}

// memoryPromptWrapper wraps a PromptBuilder to inject the memory summary
// and memory instructions into the system prompt. It reads the current
// memory_summary.md on each Build call so updates between turns are
// reflected.
type memoryPromptWrapper struct {
	inner  agent.PromptBuilder
	store  *memory.Store
	logger *slog.Logger
}

func (w *memoryPromptWrapper) Build(ctx context.Context) (string, []domain.ContextRuleRef, error) {
	secs, err := w.BuildSections(ctx)
	if err != nil {
		return "", nil, err
	}
	return joinTexts(secs.Static, secs.Dynamic), secs.Refs, nil
}

// BuildSections splits the wrapped prompt for prompt caching: the memory
// instructions (a constant) join the cacheable static part; the memory
// summary (re-read from disk every turn) joins the per-request dynamic
// part. See agent.SectionedPromptBuilder.
func (w *memoryPromptWrapper) BuildSections(ctx context.Context) (prompt.Sections, error) {
	var secs prompt.Sections
	if sb, ok := w.inner.(agent.SectionedPromptBuilder); ok {
		s, err := sb.BuildSections(ctx)
		if err != nil {
			return prompt.Sections{}, err
		}
		secs = s
	} else {
		base, refs, err := w.inner.Build(ctx)
		if err != nil {
			return prompt.Sections{}, err
		}
		secs = prompt.Sections{Static: base, Refs: refs}
	}

	// Inject memory summary (hot tier).
	provider := memory.NewPromptProvider(w.store)
	summary, err := provider.MemoryPrompt(ctx)
	if err != nil && w.logger != nil {
		w.logger.Warn("memory prompt injection failed", "error", err)
	}

	// Assemble the injected memory sections in one place so the audit rule
	// ref hashes exactly what was appended to the prompt.
	staticSection := memory.MemoryInstructions
	var dynamicBody string
	if summary != "" {
		dynamicBody = "# Memory\n\n" + summary
	}
	secs.Static = strings.TrimRight(secs.Static, "\n") + "\n\n" + staticSection + "\n"
	if dynamicBody != "" {
		secs.Dynamic = strings.TrimRight(secs.Dynamic, "\n") + "\n\n" + dynamicBody + "\n"
	}

	// Add the memory rule ref for audit; the context manifest requires a
	// non-empty hash per rule.
	secs.Refs = append(secs.Refs, provider.RuleRef(staticSection+dynamicBody))

	return secs, nil
}

func joinTexts(static, dynamic string) string {
	switch {
	case strings.TrimSpace(static) == "":
		return dynamic
	case strings.TrimSpace(dynamic) == "":
		return static
	default:
		return static + "\n\n" + dynamic
	}
}
