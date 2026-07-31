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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/tool/lint"
	"github.com/liubang/playground/go/pl/loom/internal/tool/webfetch"
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
	// §4.4): rules → danger → session → mode-aware baseline.
	Policy        agent.Policy
	PromptBuilder agent.PromptBuilder
	Logger        *slog.Logger
	Validator     *workspace.PathValidator
	Runner        *process.Runner
	Version       string
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
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDB)
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
		SessionEnv: sessionEnv.Get,
	})
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("create process runner: %w", err)
	}

	// Create tool registry and register built-in tools
	registry := agent.NewToolRegistry()
	goalCell := agent.NewGoalCell()
	planCell := agent.NewPlanCell()
	steerCell := agent.NewSteerCell()
	questioner := cfg.Questioner
	if questioner == nil {
		questioner = domain.AutonomousQuestioner{}
	}
	if err := registerBuiltinTools(registry, validator, runner, artStore, resolved.Limits.MaxToolOutputBytes, goalCell, planCell, questioner); err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("register tools: %w", err)
	}

	// Session-remembered approvals ("allow always") share one store with the
	// policy layer; declarative user/project rules load on top of the
	// baseline per the config file's rules.* section. The assembled decider
	// chain applies the approval.* baseline mode (on-request by default:
	// sandboxed non-dangerous commands run without prompting).
	sessionRules := permission.NewSessionRules()
	policy := permission.DefaultPolicy()
	policy.Session = sessionRules
	policy = permission.AttachRules(policy, cfg.WorkspaceRoot, permission.RuleLoadOptions{
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

	var promptBuilder agent.PromptBuilder
	if !resolved.Prompt.DisableBuiltin {
		promptOpts := []prompt.Option{prompt.WithExtraInstructions(resolved.Prompt.Extra)}
		if opt := ResolveManagedPrompt(ctx, resolved.Prompt.Managed, traceCfg, resolved.Storage.SessionDB, logger); opt != nil {
			promptOpts = append(promptOpts, opt)
		}
		// Skills: registers read_skill and appends the catalog provider.
		// Inside the system-prompt guard so a catalog-less read_skill is
		// never registered. The catalog budget tracks the startup model's
		// window; switching models does not rebuild it (catalogs are far
		// smaller than any window).
		skillsOpt, err := WireSkills(registry, cfg.WorkspaceRoot, defaultContextWindow(resolved), resolved.Skills, resolved.Prompt.DisableBuiltin, logger)
		if err != nil {
			_ = store.Close()
			return nil, fmt.Errorf("wire skills: %w", err)
		}
		if skillsOpt != nil {
			promptOpts = append(promptOpts, skillsOpt)
		}
		promptBuilder = prompt.NewBuilder(cfg.WorkspaceRoot, promptOpts...)
	}

	return &Bootstrap{
		Resolved:      resolved,
		Current:       resolved.Default,
		WorkspaceRoot: cfg.WorkspaceRoot,
		Store:         store,
		Artifact:      artStore,
		Registry:      registry,
		Policy:        decider,
		PromptBuilder: promptBuilder,
		Logger:        logger,
		Validator:     validator,
		Runner:        runner,
		Version:       cfg.Version,
		SessionEnv:    sessionEnv,
		GoalCell:      goalCell,
		PlanCell:      planCell,
		SteerCell:     steerCell,
		Questioner:    questioner,
		Recorder:      traceRecorder,
		SessionRules:  sessionRules,
		traceProvider: traceProvider,
	}, nil
}

// defaultContextWindow returns the startup model's context window (0 = the
// loop falls back to Limits.MaxInputTokens as the occupancy proxy).
func defaultContextWindow(resolved *config.ResolvedConfig) int64 {
	if meta, ok := resolved.ModelMeta(resolved.Default); ok {
		return meta.ContextWindow
	}
	return 0
}

// registerBuiltinTools registers all built-in tools with the registry.
func registerBuiltinTools(registry *agent.ToolRegistry, validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, maxOutputBytes int64, goalCell *agent.GoalCell, planCell *agent.PlanCell, questioner domain.Questioner) error {
	// The file-state book is shared by read_file (records hashes) and edit
	// (checks drift) to detect external modification without model-carried
	// hashes.
	book := workspace.NewFileStateBook()
	tools := []struct {
		name string
		mk   func() (domain.Tool, error)
	}{
		{"read_file", func() (domain.Tool, error) { return builtin.NewReadFileTool(validator, book) }},
		{"list_dir", func() (domain.Tool, error) { return builtin.NewListDirTool(validator) }},
		{"search", func() (domain.Tool, error) { return builtin.NewSearchTool(validator, runner) }},
		{"glob", func() (domain.Tool, error) { return builtin.NewGlobTool(validator, runner) }},
		{"edit", func() (domain.Tool, error) { return edit.NewEditTool(validator, book) }},
		{"write", func() (domain.Tool, error) { return edit.NewWriteTool(validator) }},
		{"git_status", func() (domain.Tool, error) { return gittools.NewGitStatusTool(validator) }},
		{"git_diff", func() (domain.Tool, error) { return gittools.NewGitDiffTool(validator) }},
		{"git_log", func() (domain.Tool, error) { return gittools.NewGitLogTool(validator) }},
		{"lint", func() (domain.Tool, error) { return lint.NewLintTool(validator, runner) }},
		{"web_fetch", func() (domain.Tool, error) { return webfetch.NewWebFetchTool(artStore) }},
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

// Close releases all resources held by the Bootstrap.
func (b *Bootstrap) Close() {
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
func ResolveManagedPrompt(ctx context.Context, managed config.ManagedPrompt, traceCfg trace.Config, sessionDBPath string, logger *slog.Logger) prompt.Option {
	if managed.Name == "" || !traceCfg.Enabled {
		return nil
	}
	label := managed.Label
	if label == "" {
		label = "production"
	}
	cacheDir := filepath.Join(filepath.Dir(sessionDBPath), "prompt_cache")
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
