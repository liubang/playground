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
	"os"
	"path/filepath"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
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
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// sessionStoreCloser is the interface the Bootstrap needs from its Store.
// The domain.SessionStore doesn't include Close, but concrete implementations
// (e.g. SQLiteStore) provide it.
type sessionStoreCloser interface {
	domain.SessionStore
	Close() error
}

// BootstrapConfig holds all configuration needed to bootstrap a Loom runtime.
type BootstrapConfig struct {
	// WorkspaceRoot is the absolute path to the workspace directory.
	WorkspaceRoot string
	// SessionDBPath is the path to the SQLite session database.
	SessionDBPath string
	// ArtifactDir is the path to the artifact directory.
	ArtifactDir string
	// ArtifactMaxBytes is the maximum size for a single artifact.
	ArtifactMaxBytes int64
	// ModelName is the model identifier (e.g. "gpt-4o").
	ModelName string
	// BaseURL is the OpenAI-compatible API base URL.
	BaseURL string
	// APIKey is the API key for the model provider.
	APIKey string
	// WireAPI selects the wire protocol (chat or responses).
	WireAPI openai.WireAPI
	// Limits defines runtime limits for the agent.
	Limits domain.Limits
	// Policy defines the security policy.
	Policy permission.Policy
	// SystemPromptExtra holds extra instructions appended to the built-in
	// system prompt as a dedicated section.
	SystemPromptExtra string
	// DisableSystemPrompt disables injection of the built-in system prompt.
	DisableSystemPrompt bool
	// Logger is the slog.Logger to use; if nil, a default is created.
	Logger *slog.Logger
}

// Bootstrap assembles the runtime components for a Loom session.
// It owns the lifecycle of the session store, artifact store, tool
// registry, and model provider.
type Bootstrap struct {
	Config        BootstrapConfig
	Store         domain.SessionStore
	Artifact      domain.ArtifactStore
	Registry      *agent.ToolRegistry
	Model         domain.Model
	ModelName     string
	Policy        permission.Policy
	PromptBuilder agent.PromptBuilder
	Logger        *slog.Logger
	Validator     *workspace.PathValidator
	Runner        *process.Runner
}

// NewBootstrap creates a new Bootstrap and assembles all runtime components.
// The caller is responsible for calling Close when done.
func NewBootstrap(ctx context.Context, cfg BootstrapConfig) (*Bootstrap, error) {
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	// Open session store
	store, err := session.OpenSQLiteStore(ctx, cfg.SessionDBPath)
	if err != nil {
		return nil, fmt.Errorf("open session store: %w", err)
	}

	// Open artifact store
	artifactMaxBytes := cfg.ArtifactMaxBytes
	if artifactMaxBytes <= 0 {
		artifactMaxBytes = cfg.Limits.MaxArtifactBytes
	}
	artStore, err := artifact.Open(cfg.ArtifactDir, artifactMaxBytes)
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
	// redirectable for go vet to work at all).
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
		EnvAllowlist: []string{
			"PATH", "LANG", "LC_ALL", "TMPDIR", "HOME",
			"GOCACHE", "GOPATH", "GOMODCACHE", "GOPROXY", "GOSUMDB", "GOFLAGS",
		},
	})
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("create process runner: %w", err)
	}

	// Create tool registry and register built-in tools
	registry := agent.NewToolRegistry()
	if err := registerBuiltinTools(registry, validator, runner, artStore, cfg.Limits.MaxToolOutputBytes); err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("register tools: %w", err)
	}

	// Create model provider
	wireAPI := cfg.WireAPI
	if wireAPI == "" {
		wireAPI = openai.WireAPIChatCompletions
	}
	provider, err := openai.New(openai.Config{
		BaseURL:    cfg.BaseURL,
		APIKey:     cfg.APIKey,
		WireAPI:    wireAPI,
		MaxRetries: 2,
	})
	if err != nil {
		_ = store.Close()
		return nil, fmt.Errorf("create model provider: %w", err)
	}

	policy := cfg.Policy
	if !policy.AutoApproveR1 && !policy.AskR2 {
		p := permission.DefaultPolicy()
		policy = p
	}

	var promptBuilder agent.PromptBuilder
	if !cfg.DisableSystemPrompt {
		promptBuilder = prompt.NewBuilder(cfg.WorkspaceRoot,
			prompt.WithExtraInstructions(cfg.SystemPromptExtra))
	}

	return &Bootstrap{
		Config:        cfg,
		Store:         store,
		Artifact:      artStore,
		Registry:      registry,
		Model:         provider,
		ModelName:     cfg.ModelName,
		Policy:        policy,
		PromptBuilder: promptBuilder,
		Logger:        logger,
		Validator:     validator,
		Runner:        runner,
	}, nil
}

// registerBuiltinTools registers all built-in tools with the registry.
func registerBuiltinTools(registry *agent.ToolRegistry, validator *workspace.PathValidator, runner *process.Runner, artStore domain.ArtifactStore, maxOutputBytes int64) error {
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
	return nil
}

// Close releases all resources held by the Bootstrap.
func (b *Bootstrap) Close() {
	if b.Store != nil {
		if closer, ok := b.Store.(interface{ Close() error }); ok {
			_ = closer.Close()
		}
	}
}

// DefaultBootstrapConfig creates a BootstrapConfig with sensible defaults
// derived from environment variables and the given workspace root.
func DefaultBootstrapConfig(workspaceRoot string) BootstrapConfig {
	return BootstrapConfig{
		WorkspaceRoot:       workspaceRoot,
		ModelName:           getEnvDefault("LOOM_MODEL", "gpt-4o"),
		BaseURL:             os.Getenv("LOOM_BASE_URL"),
		APIKey:              os.Getenv("LOOM_API_KEY"),
		Limits:              domain.DefaultLimits(),
		Policy:              permission.DefaultPolicy(),
		SystemPromptExtra:   os.Getenv("LOOM_SYSTEM_PROMPT_EXTRA"),
		DisableSystemPrompt: os.Getenv("LOOM_DISABLE_SYSTEM_PROMPT") == "1",
	}
}

// DerivePaths computes session DB and artifact directory paths from a base directory.
func DerivePaths(baseDir string) (sessionDBPath, artifactDir string) {
	return filepath.Join(baseDir, "sessions.db"),
		filepath.Join(baseDir, "artifacts")
}

func getEnvDefault(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}
