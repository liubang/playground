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
// Created: 2026/08/06

package app

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/mcp"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
)

// ProcessRuntime holds every process-level resource that is independent of
// any workspace: the session store, artifact store, model configuration,
// tracing, the shared MCP manager, the user-global memory system, and the
// process-wide approval-memory stores. Its lifetime equals the process's
// (docs/WORKSPACE_DESIGN.md §5.1).
//
// Workspace-scoped assembly (Bootstrap) embeds *ProcessRuntime so the
// process-level fields stay reachable through the workspace handle with no
// call-site churn; each workspace assembles only its own validator, runner,
// registry, policy, prompt builder, skills and sub-agent runtime on top.
type ProcessRuntime struct {
	Resolved *config.ResolvedConfig
	Current  config.ProviderModelRef
	Store    domain.SessionStore
	Artifact domain.ArtifactStore
	Recorder trace.Recorder
	Logger   *slog.Logger
	Version  string
	// SessionEnv holds the loom attribution variables injected into every
	// spawned command; per-session values ride the turn context (SERVE_DESIGN
	// G3), this process-level atomic is the context-less fallback.
	SessionEnv      *process.AtomicSessionEnv
	SessionRules    *permission.SessionRules
	RememberedStore *permission.RememberedStore
	// MCPManager owns every running MCP server subprocess (process-level:
	// per-workspace assembly would spawn N×M subprocesses, WORKSPACE_DESIGN
	// D2). Its tools are registered into each workspace's registry.
	MCPManager *mcp.Manager
	// Memory system (user-global, WORKSPACE_DESIGN D5). The tools are
	// registered per workspace; the store/extractor/consolidator are shared.
	MemoryStore        *memory.Store
	MemoryExtractor    *memory.Extractor
	MemoryConsolidator *memory.Consolidator
	// MemoryJobQueue persists Phase 1 extraction jobs. Session shutdown
	// enqueues a job (a cheap upsert); the background pipeline drains it —
	// no model calls ever happen on the exit path (MEMORY_DESIGN P0-A).
	MemoryJobQueue memory.JobQueue
	// Questioner is the headless fallback (AutonomousQuestioner); a
	// session-level ChannelQuestioner takes precedence when present.
	Questioner domain.Questioner

	traceProvider      *trace.Provider
	memoryPipelineStop context.CancelFunc
}

// ProcessRuntimeConfig carries the entry-point-specific inputs that do not
// live in the config file: process paths, the build version, and logging.
type ProcessRuntimeConfig struct {
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

// NewProcessRuntime assembles the process-level resources from a resolved
// configuration file. It owns the lifecycle of the session store, artifact
// store, MCP manager, memory system, and tracing. The caller is responsible
// for calling Close when done. Workspace-scoped components are assembled
// separately per workspace via NewWorkspaceBootstrap.
func NewProcessRuntime(ctx context.Context, resolved *config.ResolvedConfig, cfg ProcessRuntimeConfig) (*ProcessRuntime, error) {
	if resolved == nil {
		return nil, fmt.Errorf("resolved config is required")
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	// Open session store.
	store, err := session.OpenSQLiteStore(ctx, resolved.Storage.SessionDBPath())
	if err != nil {
		return nil, fmt.Errorf("open session store: %w", err)
	}

	// Open artifact store.
	artStore, err := artifact.Open(cfg.ArtifactDir, resolved.Limits.MaxArtifactBytes)
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("open artifact store: %w", err)
	}

	// SessionEnv injects the loom attribution variables into every spawned
	// command so downstream CLIs can attribute traffic to this session.
	sessionEnv := &process.AtomicSessionEnv{}

	questioner := cfg.Questioner
	if questioner == nil {
		questioner = domain.AutonomousQuestioner{}
	}

	// Session-remembered approvals ("allow always") share one store with the
	// policy layer; declarative user/project rules load on top per workspace.
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
	logger.Info("approval mode", "mode", resolved.Approval.Mode)

	// Langfuse tracing comes from the config file's tracing.* section.
	// Setup failure degrades to a no-op recorder — observability must never
	// break the agent.
	traceCfg := resolved.Tracing
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

	// MCP servers: start all configured server subprocesses once for the
	// process. Startup is best-effort: a server that fails to start is logged
	// and its tools are absent; the manager is still kept (possibly with zero
	// clients) so frontends can report the per-server failure reasons.
	var mcpManager *mcp.Manager
	if len(resolved.MCP.Servers) > 0 {
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
		mcpManager = mgr
		if err == nil {
			logger.Info("mcp servers started", "servers", len(mgr.Servers()), "tools", len(mgr.Tools()))
		}
	}

	// Memory system (user-global): open the persistent store and wire the
	// extraction/consolidation pipeline. Tool registration and the prompt
	// wrapper happen per workspace in NewWorkspaceBootstrap.
	var (
		memoryStore        *memory.Store
		memoryExtractor    *memory.Extractor
		memoryConsolidator *memory.Consolidator
		memoryJobQueue     memory.JobQueue
		memoryPipelineStop context.CancelFunc
	)
	if resolved.Memory.Enabled {
		if memStore, err := memory.OpenStore(resolved.Storage.MemoriesDir()); err != nil {
			logger.Warn("memory system disabled: open store failed", "error", err)
		} else {
			memoryStore = memStore
			if err := memStore.InitGit(ctx); err != nil {
				logger.Warn("memory git init failed; consolidation will be disabled until git is available", "error", err)
			}
			// Per-role model selection (P2): extraction and consolidation
			// may pin their own models; both fall back to the default.
			extractRef := resolved.Default
			if resolved.Memory.ExtractModel != nil {
				extractRef = *resolved.Memory.ExtractModel
			}
			if provider := resolved.ProviderByName(extractRef.Provider); provider != nil {
				// Structured output (P4) is only requested from wire APIs
				// that support it (OpenAI chat/responses).
				structured := provider.WireAPIFor(extractRef.Model) != "messages"
				memoryExtractor = memory.NewExtractor(memStore, provider.ModelFor(extractRef.Model), extractRef.Model, structured)
			}
			consolidationRef := resolved.Default
			if resolved.Memory.ConsolidationModel != nil {
				consolidationRef = *resolved.Memory.ConsolidationModel
			}
			if provider := resolved.ProviderByName(consolidationRef.Provider); provider != nil {
				memoryConsolidator = memory.NewConsolidator(memStore, provider.ModelFor(consolidationRef.Model), consolidationRef.Model)
			}
			// Background pipeline (P0-A): drains the persistent extraction
			// job queue at startup and every RunInterval afterwards. The
			// shutdown path only enqueues jobs; model work never blocks
			// exit. A cancelled pass leaves leased jobs to be reclaimed
			// after lease expiry, so Close does not wait for it.
			if memoryExtractor != nil {
				memoryJobQueue = store
				pipelineCfg := memory.DefaultPipelineConfig()
				pipelineCfg.MaxJobsPerRun = resolved.Memory.MaxJobsPerRun
				pipelineCfg.MinIdle = resolved.Memory.MinSessionIdle
				pipelineCfg.MaxAge = resolved.Memory.MaxSessionAge
				pipeline := memory.NewPipeline(store, store, memoryExtractor, memoryConsolidator, pipelineCfg, logger)
				pipelineCtx, cancel := context.WithCancel(context.Background())
				memoryPipelineStop = cancel
				pipeline.Start(pipelineCtx, resolved.Memory.RunInterval)
				logger.Info("memory pipeline started",
					"run_interval", resolved.Memory.RunInterval,
					"max_jobs_per_run", resolved.Memory.MaxJobsPerRun)
			}
		}
	}

	return &ProcessRuntime{
		Resolved:           resolved,
		Current:            resolved.Default,
		Store:              store,
		Artifact:           artStore,
		Recorder:           traceRecorder,
		Logger:             logger,
		Version:            cfg.Version,
		SessionEnv:         sessionEnv,
		SessionRules:       sessionRules,
		RememberedStore:    rememberedStore,
		MCPManager:         mcpManager,
		MemoryStore:        memoryStore,
		MemoryExtractor:    memoryExtractor,
		MemoryConsolidator: memoryConsolidator,
		MemoryJobQueue:     memoryJobQueue,
		Questioner:         questioner,
		traceProvider:      traceProvider,
		memoryPipelineStop: memoryPipelineStop,
	}, nil
}

// Close releases the process-level resources. It must run after every
// workspace Bootstrap has been closed (workspace Close drains sub-agents and
// background processes that may still reference the shared store).
func (p *ProcessRuntime) Close() {
	if p == nil {
		return
	}
	// Stop the background memory pipeline. In-flight claims stay leased
	// and are reclaimed after lease expiry on the next startup — no
	// consolidation runs on the exit path (P0-A).
	if p.memoryPipelineStop != nil {
		p.memoryPipelineStop()
	}
	if p.RememberedStore != nil {
		if err := p.RememberedStore.Close(); err != nil && p.Logger != nil {
			p.Logger.Warn("remembered store shutdown failed", "error", err)
		}
	}
	if p.MCPManager != nil {
		if err := p.MCPManager.Close(); err != nil && p.Logger != nil {
			p.Logger.Warn("mcp manager shutdown failed", "error", err)
		}
	}
	if p.traceProvider != nil {
		// Flush buffered spans with a bounded wait; tracing must never hang
		// shutdown.
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		if err := p.traceProvider.Shutdown(ctx); err != nil && p.Logger != nil {
			p.Logger.Warn("langfuse tracing shutdown failed", "error", err)
		}
	}
	if p.Store != nil {
		if closer, ok := p.Store.(interface{ Close() error }); ok {
			_ = closer.Close()
		}
	}
}
