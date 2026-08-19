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
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"
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
	// resolved is the live configuration, swapped atomically by
	// SwapResolved when a config save is hot-applied (PUT /v1/config).
	// Readers must go through Resolved() and not retain the pointer
	// beyond a single unit of work (turn construction, policy reload).
	resolved atomic.Pointer[config.ResolvedConfig]
	// Current is the configured default model (config file / launch flag).
	// Reads of the effective selection must go through CurrentModel: a
	// manual switch updates the runtime-current value (and persists it),
	// which may differ from Current afterwards. Writes after construction
	// go through SetConfiguredDefault (config hot-reload); reads through
	// CurrentDefault — both under prefMu.
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

	// prefMu guards Current/currentModel/reasoningPref: written when the
	// user switches model or reasoning (and persisted to the store) or
	// when a hot-applied config changes the configured default, read by
	// every session's controller when resolving the effective selection.
	prefMu             sync.RWMutex
	currentModel       config.ProviderModelRef
	reasoningPref      string
	traceProvider      *trace.Provider
	memoryPipelineStop context.CancelFunc
	// autoArchiveStop cancels the background session archiver (nil when
	// the archiver was never started — e.g. bare test runtimes).
	autoArchiveStop context.CancelFunc
	// mcpMu guards MCPManager: created on demand when a hot-applied
	// config introduces the first MCP server, swapped on shutdown.
	mcpMu sync.RWMutex
}

// Preference keys stored in the session store's app_prefs table.
const (
	prefKeyModel     = "model"
	prefKeyReasoning = "reasoning"
)

// Resolved returns the live resolved configuration; see the resolved
// field comment for the retention contract.
func (p *ProcessRuntime) Resolved() *config.ResolvedConfig {
	return p.resolved.Load()
}

// SwapResolved atomically replaces the resolved configuration. Components
// that read Resolved() per unit of work (turn construction, policy
// reload, prompt builds) pick up the new values on their next cycle;
// components that captured fields at assembly time keep the old ones
// until explicitly rebuilt (see SessionService.ApplyConfig).
func (p *ProcessRuntime) SwapResolved(next *config.ResolvedConfig) {
	p.resolved.Store(next)
}

// CurrentDefault returns the configured default model (hot-reload safe).
func (p *ProcessRuntime) CurrentDefault() config.ProviderModelRef {
	p.prefMu.RLock()
	defer p.prefMu.RUnlock()
	return p.Current
}

// SetConfiguredDefault updates the configured default model after a
// config hot-reload. A persisted manual preference still wins in
// CurrentModel; it is dropped (memory and store) when it no longer
// resolves against the new configuration (e.g. its provider was
// removed).
func (p *ProcessRuntime) SetConfiguredDefault(ctx context.Context, ref config.ProviderModelRef) {
	p.prefMu.Lock()
	p.Current = ref
	dropPref := false
	if p.currentModel != (config.ProviderModelRef{}) {
		if rc := p.resolved.Load(); rc != nil {
			if _, err := rc.ResolveRef(p.currentModel.String()); err != nil {
				p.currentModel = config.ProviderModelRef{}
				dropPref = true
			}
		}
	}
	p.prefMu.Unlock()
	if dropPref {
		// Clear the persisted value too, so a restart does not warn about
		// the same unresolvable preference again.
		p.persistPref(ctx, prefKeyModel, "")
	}
}

// MCP returns the shared MCP manager (nil when no server is configured).
func (p *ProcessRuntime) MCP() *mcp.Manager {
	p.mcpMu.RLock()
	defer p.mcpMu.RUnlock()
	return p.MCPManager
}

// SetMCPManager installs (or clears) the shared MCP manager.
func (p *ProcessRuntime) SetMCPManager(m *mcp.Manager) {
	p.mcpMu.Lock()
	defer p.mcpMu.Unlock()
	p.MCPManager = m
}

// CurrentModel returns the runtime-effective model selection: the user's
// latest manual switch when one happened, otherwise the configured default.
func (p *ProcessRuntime) CurrentModel() config.ProviderModelRef {
	p.prefMu.RLock()
	defer p.prefMu.RUnlock()
	if p.currentModel != (config.ProviderModelRef{}) {
		return p.currentModel
	}
	// Hand-assembled runtimes (tests) set only Current: fall back to it.
	return p.Current
}

// ReasoningPreference returns the persisted reasoning dial ("off"/"low"/
// "medium"/"high"/"default"); "" or "default" means no override — the
// selected model's configured reasoning applies.
func (p *ProcessRuntime) ReasoningPreference() string {
	p.prefMu.RLock()
	defer p.prefMu.RUnlock()
	return p.reasoningPref
}

// SetModelPreference records a manual model switch as the runtime current
// selection and persists it so future sessions (and restarts) inherit it.
// Persistence failure is logged, not fatal: the in-memory choice stands.
func (p *ProcessRuntime) SetModelPreference(ctx context.Context, ref config.ProviderModelRef) {
	p.prefMu.Lock()
	p.currentModel = ref
	p.prefMu.Unlock()
	p.persistPref(ctx, prefKeyModel, ref.String())
}

// SetReasoningPreference records a manual reasoning switch ("default"
// clears the override) and persists it.
func (p *ProcessRuntime) SetReasoningPreference(ctx context.Context, effort string) {
	p.prefMu.Lock()
	p.reasoningPref = effort
	p.prefMu.Unlock()
	p.persistPref(ctx, prefKeyReasoning, effort)
}

// persistPref writes one preference when the store supports it; failures
// only cost the restart-survival of the choice, never the live selection.
func (p *ProcessRuntime) persistPref(ctx context.Context, key, value string) {
	store, ok := p.Store.(*session.SQLiteStore)
	if !ok {
		return
	}
	if err := store.SetPref(ctx, key, value); err != nil && p.Logger != nil {
		p.Logger.Warn("persist preference failed", "key", key, "error", err)
	}
}

// loadPrefs restores the persisted model/reasoning preferences over the
// configured defaults. Unresolvable or absent values are ignored — config
// changes (renamed providers/models) must never break startup.
func (p *ProcessRuntime) loadPrefs(ctx context.Context) {
	store, ok := p.Store.(*session.SQLiteStore)
	if !ok {
		return
	}
	if raw, err := store.GetPref(ctx, prefKeyModel); err == nil && raw != "" {
		if ref, err := p.Resolved().ResolveRef(raw); err == nil {
			p.currentModel = ref
		} else if p.Logger != nil {
			p.Logger.Warn("ignoring unresolvable persisted model preference", "value", raw, "error", err)
		}
	}
	if raw, err := store.GetPref(ctx, prefKeyReasoning); err == nil {
		p.reasoningPref = raw
	}
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

	// Augment a sparse process PATH (GUI/launchd launches) with the
	// conventional toolchain directories before anything resolves programs:
	// exec.LookPath reads the parent PATH, and both the sandboxed minimal
	// env and the escalated full env derive from it. Without this, a
	// desktop-launched loom reports "go: command not found" inside the
	// sandbox and the model wastes turns on futile escalations. The
	// login-shell probe (configured first) contributes the user's own
	// shell PATH as a higher-priority layer; it probes asynchronously and
	// never blocks startup.
	process.ConfigureShellPathProbe(resolved.Storage.CacheDir())
	if added := process.AugmentProcessPATH(resolved.Tools.PathExtra); len(added) > 0 {
		logger.Info("augmented process PATH with toolchain dirs", "added", added)
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
		// config.MCPServer aliases mcp.ServerConfig (REVIEW R12), so the
		// resolved map flows through without a field-by-field copy.
		mgr, err := mcp.StartServers(ctx, resolved.MCP.Servers, logger)
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

	p := &ProcessRuntime{
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
	}
	// The runtime-current selection starts at the configured default, then
	// the persisted manual choice (if any) overrides it. currentModel is
	// deliberately NOT seeded from the default: it must only ever hold a
	// manual/persisted preference, so a hot-applied new default takes
	// effect for processes that never made a manual choice.
	p.resolved.Store(resolved)
	p.loadPrefs(ctx)
	// Session auto-archiver (sessions.auto_archive_after): one pass at
	// startup, then hourly. Each pass reads Resolved() fresh, so a
	// hot-applied config change takes effect on the next pass without a
	// restart. The sweep is a single idempotent UPDATE — concurrent loom
	// processes sharing the same database may all run it safely. A
	// live-but-idle session CAN be archived by the sweep; the store's
	// read-only enforcement then rejects its next write with
	// ErrSessionArchived until the user unarchives it.
	p.startSessionArchiver(store)
	return p, nil
}

// sessionArchiveSweepInterval is how often the background archiver
// re-scans for stale sessions; the staleness threshold itself comes from
// sessions.auto_archive_after and is read fresh every pass.
const sessionArchiveSweepInterval = time.Hour

// startSessionArchiver launches the background session archiver. The
// goroutine is cheap when the feature is disabled: every pass re-reads
// the resolved config and returns immediately while
// sessions.auto_archive_after is unset.
func (p *ProcessRuntime) startSessionArchiver(store *session.SQLiteStore) {
	ctx, cancel := context.WithCancel(context.Background())
	p.autoArchiveStop = cancel
	go func() {
		ticker := time.NewTicker(sessionArchiveSweepInterval)
		defer ticker.Stop()
		for {
			p.archiveStaleSessionsOnce(ctx, store)
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
}

// archiveStaleSessionsOnce runs one archiver pass: no-op while disabled.
func (p *ProcessRuntime) archiveStaleSessionsOnce(ctx context.Context, store *session.SQLiteStore) {
	resolved := p.Resolved()
	if resolved == nil || resolved.Sessions.AutoArchiveAfter <= 0 {
		return
	}
	cutoff := time.Now().UTC().Add(-resolved.Sessions.AutoArchiveAfter)
	n, err := store.ArchiveStaleSessions(ctx, cutoff)
	if err != nil {
		// A cancelled pass (Close raced the sweep) is a normal shutdown.
		if !errors.Is(err, context.Canceled) {
			p.Logger.Warn("session auto-archive sweep failed", "error", err)
		}
		return
	}
	if n > 0 {
		p.Logger.Info("stale sessions auto-archived",
			"count", n, "idle_for", resolved.Sessions.AutoArchiveAfter.String())
	}
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
	// Stop the session auto-archiver; an in-flight pass finishes or
	// aborts with the cancelled context.
	if p.autoArchiveStop != nil {
		p.autoArchiveStop()
	}
	if p.RememberedStore != nil {
		if err := p.RememberedStore.Close(); err != nil && p.Logger != nil {
			p.Logger.Warn("remembered store shutdown failed", "error", err)
		}
	}
	if mgr := p.MCP(); mgr != nil {
		if err := mgr.Close(); err != nil && p.Logger != nil {
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
