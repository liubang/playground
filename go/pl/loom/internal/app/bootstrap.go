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
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/mcp"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/exsession"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/tool/lint"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
	"github.com/liubang/playground/go/pl/loom/internal/tool/webfetch"
	"github.com/liubang/playground/go/pl/loom/internal/tool/websearch"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// BootstrapConfig carries the entry-point-specific inputs that do not live
// in the config file: process paths, the build version, and logging.
type BootstrapConfig struct {
	// WorkspaceRoot is the absolute path to the workspace directory.
	WorkspaceRoot string
	// ArtifactDir is the path to the artifact directory.
	ArtifactDir string
	// Version is the build version stamped into traces and the attribution
	// environment of spawned commands.
	Version string
	// Logger is the slog.Logger to use; if nil, a default is created.
	Logger *slog.Logger
	// Questioner resolves ask_user questions; nil selects the autonomous
	// questioner (headless-safe: every question is skipped immediately).
	Questioner domain.Questioner
}

// Bootstrap assembles the runtime components for a Loom session from a
// resolved configuration file. It owns the lifecycle of the session store,
// artifact store, tool registry, and tracing.
type Bootstrap struct {
	Resolved      *config.ResolvedConfig
	Current       config.ProviderModelRef
	WorkspaceRoot string
	Store         domain.SessionStore
	Artifact      domain.ArtifactStore
	Registry      *agent.ToolRegistry
	// Policy is the assembled decider chain (docs/PERMISSION_DESIGN.md
	// §4.4): rules → danger → session → mode-aware baseline. Read it via
	// CurrentPolicy: ReloadPolicy swaps it when rules change at runtime.
	Policy           agent.Policy
	permissionPolicy *permission.Policy
	approvalMode     permission.ApprovalMode
	// policyMu guards Policy and permissionPolicy against concurrent
	// ReloadPolicy writes and run-construction/ListRules reads.
	policyMu      sync.RWMutex
	PromptBuilder agent.PromptBuilder
	Logger        *slog.Logger
	Validator     *workspace.PathValidator
	Runner        *process.Runner
	Version       string
	// FileStateBook is the shared read/write hash tracker used by
	// read_file and edit for drift detection; rewind restoration updates
	// it so a post-rewind edit measures drift from the restored content.
	FileStateBook *workspace.FileStateBook
	// SessionEnv holds the loom attribution variables (agent name/version,
	// session ID) injected into every spawned command. The controller
	// rewrites it on session create/resume; the runner reads it per
	// execution.
	SessionEnv *process.AtomicSessionEnv
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
	// Questioner is the ask_user tool's answer source (a ChannelQuestioner
	// bridged to the TUI, or the autonomous questioner when headless).
	Questioner domain.Questioner
	// Recorder is the Langfuse observability sink (no-op when unconfigured).
	Recorder trace.Recorder
	// SessionRules holds categorical run_cmd prefixes remembered from
	// interactive "allow always" decisions; shared with Policy.Session.
	SessionRules *permission.SessionRules
	// RememberedStore persists "allow always" memories to SQLite; nil when
	// rules.persist_remembered=false or open failed (session memory still works).
	RememberedStore *permission.RememberedStore
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
	// MCPManager owns every running MCP server subprocess; Close
	// terminates them. Nil when no MCP servers are configured. When every
	// server failed to start, the manager is still kept (with zero
	// clients) so frontends can report the per-server failure reasons.
	MCPManager *mcp.Manager
	// Skills exposes the skills loader/catalog shared by the prompt
	// provider and the read_skill tool; nil when skills are disabled
	// (skills.enabled=false or the built-in system prompt is off).
	Skills *SkillsHandle
	// MemoryStore is the persistent memory store (<base_dir>/memories/);
	// nil when memory is disabled (memory.enabled=false).
	MemoryStore *memory.Store
	// MemoryExtractor runs Phase 1 extraction at session end;
	// nil when memory is disabled.
	MemoryExtractor *memory.Extractor
	// MemoryConsolidator runs Phase 2 consolidation;
	// nil when memory is disabled.
	MemoryConsolidator *memory.Consolidator

	traceProvider *trace.Provider
}

// NewBootstrap creates a new Bootstrap and assembles all runtime components.
// The caller is responsible for calling Close when done.
func NewBootstrap(ctx context.Context, resolved *config.ResolvedConfig, cfg BootstrapConfig) (*Bootstrap, error) {
	if resolved == nil {
		return nil, fmt.Errorf("resolved config is required")
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	// Open session store
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return nil, fmt.Errorf("open session store: %w", err)
	}

	// Open artifact store
	artStore, err := artifact.Open(cfg.ArtifactDir, resolved.Limits.MaxArtifactBytes)
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("open artifact store: %w", err)
	}

	// Create workspace validator
	validator, err := workspace.NewPathValidator(cfg.WorkspaceRoot)
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("create path validator: %w", err)
	}

	// Create process runner. The env allowlist additionally permits the Go
	// toolchain variables the lint tool overrides (the sandbox only allows
	// writes inside the workspace and the temp dir, so GOCACHE must be
	// redirectable for go vet to work at all). SessionEnv injects the loom
	// attribution variables (see process.LoomSessionEnv) into every spawned
	// command so downstream CLIs can attribute traffic to this session.
	sessionEnv := &process.AtomicSessionEnv{}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
		EnvAllowlist: []string{
			"PATH", "LANG", "LC_ALL", "TMPDIR", "HOME",
			"GOCACHE", "GOPATH", "GOMODCACHE", "GOPROXY", "GOSUMDB", "GOFLAGS",
		},
		// Per-session attribution rides the turn context (Controller
		// path, docs/SERVE_DESIGN.md §4.3); the process-level atomic
		// value remains the fallback for context-less paths (headless
		// runAgent, runner tests).
		SessionEnv: func(ctx context.Context) map[string]string {
			if env := process.SessionEnvFromContext(ctx); len(env) > 0 {
				return env
			}
			return sessionEnv.Get()
		},
	})
	if err != nil {
		_ = store.Close()
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
	questioner := cfg.Questioner
	if questioner == nil {
		questioner = domain.AutonomousQuestioner{}
	}
	sessionManager, err := exsession.NewManager(runner, artStore, exsession.DefaultIdleTTL)
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("create session manager: %w", err)
	}
	if err := registerBuiltinTools(registry, validator, runner, artStore, resolved.Limits.MaxToolOutputBytes, goalCell, planCell, questioner, book, sessionManager); err != nil {
		sessionManager.Close()
		_ = store.Close()
		return nil, fmt.Errorf("register tools: %w", err)
	}

	// Session-remembered approvals ("allow always") share one store with the
	// policy layer; declarative user/project rules load on top of the
	// baseline per the config file's rules.* section. The assembled decider
	// chain applies the approval.* baseline mode (on-request by default:
	// sandboxed non-dangerous commands run without prompting).
	sessionRules := permission.NewSessionRules()
	var rememberedStore *permission.RememberedStore
	if resolved.Rules.PersistRemembered {
		rulesDir := resolved.Storage.RulesDir()
		if store, err := permission.OpenRememberedStore(ctx, permission.RememberedDBPath(rulesDir)); err != nil {
			logger.Warn("remembered rules disabled: open store failed", "error", err)
		} else {
			if err := store.MigrateLegacyJSON(ctx, rulesDir); err != nil {
				logger.Warn("remembered rules: legacy migration incomplete", "error", err)
			}
			rememberedStore = store
		}
	}
	policy := permission.DefaultPolicy()
	policy.Session = sessionRules
	policy = permission.AttachRules(ctx, policy, cfg.WorkspaceRoot, resolved.Storage.RulesDir(), permission.RuleLoadOptions{
		Enabled:      resolved.Rules.Enabled,
		Builtin:      resolved.Rules.Builtin,
		Project:      resolved.Rules.Project,
		ProjectAllow: resolved.Rules.ProjectAllow,
	}, logger)
	decider := policy.Decider(resolved.Approval.Mode)
	logger.Info("approval mode", "mode", resolved.Approval.Mode)

	// Langfuse tracing comes from the config file's tracing.* section.
	// Setup failure degrades to a no-op recorder — observability must never
	// break the agent.
	traceCfg := resolved.Tracing
	// Route exporter/client failures into the (discardable) TUI logger:
	// anything written to stderr tears the TUI rendering.
	traceCfg.Logger = logger
	traceCfg.Release = cfg.Version
	var (
		traceRecorder trace.Recorder = trace.Noop()
		traceProvider *trace.Provider
	)
	if traceCfg.Enabled {
		provider, err := trace.Setup(ctx, traceCfg)
		if err != nil {
			logger.Warn("langfuse tracing disabled: setup failed", "error", err)
		} else {
			traceProvider = provider
			traceRecorder = provider.Recorder()
			logger.Info("langfuse tracing enabled",
				"host", traceCfg.Host,
				"environment", traceCfg.Environment,
				"include_content", traceCfg.IncludeContent)
		}
	}

	var (
		promptBuilder agent.PromptBuilder
		skills        *SkillsHandle
	)
	if !resolved.Prompt.DisableBuiltin {
		promptOpts := []prompt.Option{
			prompt.WithExtraInstructions(resolved.Prompt.Extra),
			prompt.WithRulesProvider(prompt.NewFileRulesProvider(cfg.WorkspaceRoot, resolved.Storage.LoomMDPath())),
		}
		if opt := ResolveManagedPrompt(ctx, resolved.Prompt.Managed, traceCfg, resolved.Storage.SessionsDir(), logger); opt != nil {
			promptOpts = append(promptOpts, opt)
		}
		// Skills: registers read_skill and appends the catalog provider.
		// Inside the system-prompt guard so a catalog-less read_skill is
		// never registered. The catalog budget tracks the startup model's
		// window; switching models does not rebuild it (catalogs are far
		// smaller than any window).
		skillsOpt, skillsHandle, err := WireSkills(registry, cfg.WorkspaceRoot, defaultContextWindow(resolved), resolved.Skills, resolved.Storage.SkillsDir(), resolved.Prompt.DisableBuiltin, logger)
		if err != nil {
			_ = store.Close()
			return nil, fmt.Errorf("wire skills: %w", err)
		}
		if skillsOpt != nil {
			promptOpts = append(promptOpts, skillsOpt)
		}
		skills = skillsHandle
		promptBuilder = prompt.NewBuilder(cfg.WorkspaceRoot, promptOpts...)
	}

	// MCP servers: start all configured server subprocesses and
	// register their adapted tools into the registry. Startup is
	// best-effort: a server that fails to start is logged and its
	// tools are absent; the agent continues with built-in tools.
	var mcpManager *mcp.Manager
	if len(resolved.MCP.Servers) > 0 {
		// Convert config.MCPServer → mcp.ServerConfig for the manager.
		mcpCfgs := make(map[string]mcp.ServerConfig, len(resolved.MCP.Servers))
		for name, srv := range resolved.MCP.Servers {
			mcpCfgs[name] = mcp.ServerConfig{
				Command:           srv.Command,
				Args:              srv.Args,
				Env:               srv.Env,
				Cwd:               srv.Cwd,
				URL:               srv.URL,
				Headers:           srv.Headers,
				StartupTimeoutSec: srv.StartupTimeoutSec,
				ToolTimeoutSec:    srv.ToolTimeoutSec,
				EnabledTools:      srv.EnabledTools,
				DisabledTools:     srv.DisabledTools,
			}
		}
		mgr, err := mcp.StartServers(ctx, mcpCfgs, logger)
		if err != nil {
			logger.Warn("mcp: no server could be started; running with built-in tools only", "error", err)
		}
		// Keep the manager even on total failure: it carries the per-server
		// startup errors the /mcp listing reports.
		mcpManager = mgr
		if err == nil {
			for _, tool := range mgr.Tools() {
				if err := registry.Register(tool); err != nil {
					logger.Warn("mcp: failed to register tool", "tool", tool.Definition().Name, "error", err)
				}
			}
			logger.Info("mcp servers started", "servers", len(mgr.Servers()), "tools", len(mgr.Tools()))
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
		researcherRegistry, err := buildSubagentRegistry(validator, runner, artStore, book)
		if err != nil {
			_ = store.Close()
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

		coderRegistry, err := buildCoderRegistry(validator, runner, artStore, book, int(resolved.Limits.MaxToolOutputBytes))
		if err != nil {
			_ = store.Close()
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
			Store:                store,
			Artifacts:            artStore,
			Recorder:             traceRecorder,
			Logger:               logger,
			Registry:             researcherRegistry,
			Prompt:               researcherPrompt,
			Workspace:            cfg.WorkspaceRoot,
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
			_ = store.Close()
			return nil, fmt.Errorf("create sub-agent manager: %w", err)
		}
		factory.Manager = manager
		subagentManager = manager

		delegateTool, err := subagent.NewDelegateTaskTool(factory)
		if err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("delegate_task: %w", err)
		}
		if err := registry.Register(delegateTool); err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("register delegate_task: %w", err)
		}

		// V2 companion tools: wait_subagent and resume_subagent.
		waitTool, err := subagent.NewWaitSubagentTool(manager)
		if err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("wait_subagent: %w", err)
		}
		if err := registry.Register(waitTool); err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("register wait_subagent: %w", err)
		}
		resumeTool, err := subagent.NewResumeSubagentTool(manager)
		if err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("resume_subagent: %w", err)
		}
		if err := registry.Register(resumeTool); err != nil {
			sessionManager.Close()
			_ = store.Close()
			return nil, fmt.Errorf("register resume_subagent: %w", err)
		}

		subagentFactory = factory
		logger.Info("sub-agent delegation enabled", "roles", "researcher+coder", "async", true)
	}

	// Memory system: open the persistent memory store, register the
	// memory tools, and wire the extraction/consolidation pipeline.
	// All of this is gated by the memory.enabled config (default: true).
	var (
		memoryStore        *memory.Store
		memoryExtractor    *memory.Extractor
		memoryConsolidator *memory.Consolidator
	)
	if resolved.Memory.Enabled {
		if memStore, err := memory.OpenStore(resolved.Storage.MemoriesDir()); err != nil {
			logger.Warn("memory system disabled: open store failed", "error", err)
		} else {
			memoryStore = memStore
			// Initialize git for incremental diff detection.
			if err := memStore.InitGit(ctx); err != nil {
				logger.Warn("memory git init failed; consolidation will be disabled until git is available", "error", err)
			}
			// Register memory tools.
			if err := registerMemoryTools(registry, memStore); err != nil {
				logger.Warn("memory tools registration failed", "error", err)
			}
			// Wire extractor and consolidator with the current model.
			provider := resolved.ProviderByName(resolved.Default.Provider)
			if provider != nil {
				model := provider.ModelFor(resolved.Default.Model)
				modelName := resolved.Default.Model
				memoryExtractor = memory.NewExtractor(memStore, model, modelName)
				memoryConsolidator = memory.NewConsolidator(memStore, model, modelName)
			}
		}
	}

	// If memory is enabled, inject the memory prompt into the system
	// prompt builder. This wraps the existing prompt builder with a
	// memory-aware one.
	if memoryStore != nil && promptBuilder != nil {
		promptBuilder = &memoryPromptWrapper{
			inner:  promptBuilder,
			store:  memoryStore,
			logger: logger,
		}
	}

	return &Bootstrap{
		Resolved:           resolved,
		Current:            resolved.Default,
		WorkspaceRoot:      cfg.WorkspaceRoot,
		Store:              store,
		Artifact:           artStore,
		Registry:           registry,
		Policy:             decider,
		permissionPolicy:   &policy,
		approvalMode:       resolved.Approval.Mode,
		PromptBuilder:      promptBuilder,
		Logger:             logger,
		Validator:          validator,
		Runner:             runner,
		Version:            cfg.Version,
		FileStateBook:      book,
		SessionEnv:         sessionEnv,
		GoalCell:           goalCell,
		PlanCell:           planCell,
		SteerCell:          steerCell,
		Questioner:         questioner,
		Recorder:           traceRecorder,
		SessionRules:       sessionRules,
		SubagentModels:     subagentModels,
		RememberedStore:    rememberedStore,
		SubagentFactory:    subagentFactory,
		SubagentManager:    subagentManager,
		SessionManager:     sessionManager,
		MCPManager:         mcpManager,
		Skills:             skills,
		MemoryStore:        memoryStore,
		MemoryExtractor:    memoryExtractor,
		MemoryConsolidator: memoryConsolidator,
		traceProvider:      traceProvider,
	}, nil
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
// the decider they captured at construction.
func (b *Bootstrap) ReloadPolicy(ctx context.Context) error {
	if b.permissionPolicy == nil || b.Resolved == nil {
		return nil
	}
	policy := permission.DefaultPolicy()
	policy.Session = b.permissionPolicy.Session
	// File and store I/O happens outside the lock; only the swap is
	// serialized against readers.
	policy = permission.AttachRules(ctx, policy, b.WorkspaceRoot, b.Resolved.Storage.RulesDir(), permission.RuleLoadOptions{
		Enabled:      b.Resolved.Rules.Enabled,
		Builtin:      b.Resolved.Rules.Builtin,
		Project:      b.Resolved.Rules.Project,
		ProjectAllow: b.Resolved.Rules.ProjectAllow,
	}, b.Logger)
	b.policyMu.Lock()
	defer b.policyMu.Unlock()
	*b.permissionPolicy = policy
	b.Policy = policy.Decider(b.approvalMode)
	return nil
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

// registerMemoryTools registers the memory tools (list, read, search,
// add_note) with the tool registry.
func registerMemoryTools(registry *agent.ToolRegistry, store *memory.Store) error {
	tools := []struct {
		name string
		mk   func() (domain.Tool, error)
	}{
		{"memory_list", func() (domain.Tool, error) { return memory.NewListTool(store) }},
		{"memory_read", func() (domain.Tool, error) { return memory.NewReadTool(store) }},
		{"memory_search", func() (domain.Tool, error) { return memory.NewSearchTool(store) }},
		{"memory_add_note", func() (domain.Tool, error) { return memory.NewAddNoteTool(store) }},
	}
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

func registerBuiltinTools(registry *agent.ToolRegistry, validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, maxOutputBytes int64, goalCell *agent.GoalCell, planCell *agent.PlanCell, questioner domain.Questioner, book *workspace.FileStateBook, sessionManager *exsession.Manager) error {
	tools := []struct {
		name string
		mk   func() (domain.Tool, error)
	}{
		{"read_file", func() (domain.Tool, error) { return builtin.NewReadFileTool(validator, book) }},
		{"list_dir", func() (domain.Tool, error) { return builtin.NewListDirTool(validator) }},
		{"search", func() (domain.Tool, error) { return builtin.NewSearchTool(validator, runner) }},
		{"glob", func() (domain.Tool, error) { return builtin.NewGlobTool(validator, runner) }},
		{"view_image", func() (domain.Tool, error) { return builtin.NewViewImageTool(validator) }},
		{"edit", func() (domain.Tool, error) { return edit.NewEditTool(validator, book) }},
		{"write", func() (domain.Tool, error) { return edit.NewWriteTool(validator) }},
		{"git_status", func() (domain.Tool, error) { return gittools.NewGitStatusTool(validator) }},
		{"git_diff", func() (domain.Tool, error) { return gittools.NewGitDiffTool(validator) }},
		{"git_log", func() (domain.Tool, error) { return gittools.NewGitLogTool(validator) }},
		{"git_merge_base", func() (domain.Tool, error) { return gittools.NewGitMergeBaseTool(validator) }},
		{"git_blame", func() (domain.Tool, error) { return gittools.NewGitBlameTool(validator) }},
		{"lint", func() (domain.Tool, error) { return lint.NewLintTool(validator, runner) }},
		{"web_fetch", func() (domain.Tool, error) { return webfetch.NewWebFetchTool(artStore) }},
		{"web_search", func() (domain.Tool, error) { return websearch.NewWebSearchTool() }},
	}
	for _, tt := range tools {
		t, err := tt.mk()
		if err != nil {
			return fmt.Errorf("%s: %w", tt.name, err)
		}
		if err := registry.Register(t); err != nil {
			return fmt.Errorf("register %s: %w", tt.name, err)
		}
	}
	// run_cmd needs artifact store
	runCmd, err := command.NewRunCmdToolWithArtifacts(validator, runner, artStore, int(maxOutputBytes))
	if err != nil {
		return fmt.Errorf("run_cmd: %w", err)
	}
	if err := registry.Register(runCmd); err != nil {
		return fmt.Errorf("register run_cmd: %w", err)
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
	tools := []struct {
		name string
		mk   func() (domain.Tool, error)
	}{
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
	for _, tt := range tools {
		t, err := tt.mk()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", tt.name, err)
		}
		if err := registry.Register(t); err != nil {
			return nil, fmt.Errorf("register %s: %w", tt.name, err)
		}
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
	// Start from the researcher registry and add the writable tools.
	registry := agent.NewToolRegistry()
	tools := []struct {
		name string
		mk   func() (domain.Tool, error)
	}{
		// Read-only tools (same as researcher).
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
		// Writable tools (coder-specific).
		{"edit", func() (domain.Tool, error) { return edit.NewEditTool(validator, book) }},
		{"write", func() (domain.Tool, error) { return edit.NewWriteTool(validator) }},
		{"lint", func() (domain.Tool, error) { return lint.NewLintTool(validator, runner) }},
	}
	for _, tt := range tools {
		t, err := tt.mk()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", tt.name, err)
		}
		if err := registry.Register(t); err != nil {
			return nil, fmt.Errorf("register %s: %w", tt.name, err)
		}
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

// Close releases all resources held by the Bootstrap.
func (b *Bootstrap) Close() {
	// Drain in-flight sub-agents before closing the session store;
	// child goroutines hold store references until they persist their
	// final checkpoint.
	if b.SubagentManager != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		b.SubagentManager.Shutdown(ctx)
		cancel()
	}
	// Run Phase 2 consolidation before closing: merges raw memories
	// into MEMORY.md and regenerates the summary. Best-effort — a
	// consolidation failure must never hang shutdown.
	if b.MemoryConsolidator != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
		if changed, err := b.MemoryConsolidator.Consolidate(ctx); err != nil && b.Logger != nil {
			b.Logger.Warn("memory consolidation failed", "error", err)
		} else if changed && b.Logger != nil {
			b.Logger.Info("memory consolidation completed")
		}
		cancel()
	}
	if b.RememberedStore != nil {
		if err := b.RememberedStore.Close(); err != nil && b.Logger != nil {
			b.Logger.Warn("remembered store shutdown failed", "error", err)
		}
	}
	if b.MCPManager != nil {
		if err := b.MCPManager.Close(); err != nil && b.Logger != nil {
			b.Logger.Warn("mcp manager shutdown failed", "error", err)
		}
	}
	if b.SessionManager != nil {
		b.SessionManager.Close()
	}
	if b.traceProvider != nil {
		// Flush buffered spans with a bounded wait; tracing must never hang
		// shutdown.
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		if err := b.traceProvider.Shutdown(ctx); err != nil && b.Logger != nil {
			b.Logger.Warn("langfuse tracing shutdown failed", "error", err)
		}
	}
	if b.Store != nil {
		if closer, ok := b.Store.(interface{ Close() error }); ok {
			_ = closer.Close()
		}
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
	base, refs, err := w.inner.Build(ctx)
	if err != nil {
		return "", nil, err
	}

	// Inject memory summary (hot tier).
	provider := memory.NewPromptProvider(w.store)
	summary, err := provider.MemoryPrompt(ctx)
	if err != nil && w.logger != nil {
		w.logger.Warn("memory prompt injection failed", "error", err)
	}

	// Assemble the injected memory section in one place so the audit
	// rule ref hashes exactly what was appended to the prompt.
	var section strings.Builder
	if summary != "" {
		section.WriteString("\n\n# Memory\n\n")
		section.WriteString(summary)
	}
	// Inject memory instructions so the model knows how to use memory tools.
	section.WriteString("\n\n")
	section.WriteString(memory.MemoryInstructions)
	injected := section.String()
	base += injected

	// Add the memory rule ref for audit; the context manifest requires a
	// non-empty hash per rule.
	refs = append(refs, provider.RuleRef(injected))

	return base, refs, nil
}
