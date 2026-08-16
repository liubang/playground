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
// Created: 2026/08/16

package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/ui"
)

// runChat starts the interactive TUI chat session.
// If resumeSessionID is non-nil, the session is resumed; otherwise a new session is created.
func runChat(ctx context.Context, workspaceRoot string, resumeSessionID *domain.SessionID) error {
	root, err := resolveWorkspace(workspaceRoot)
	if err != nil {
		return err
	}
	// Config warnings go to stderr: the TUI has not taken over the screen
	// yet, so a stale-env or key-permission warning is still visible.
	resolved, err := loadConfig(true, slog.Default())
	if err != nil {
		return err
	}
	if err := prepareStorage(resolved, true); err != nil {
		return err
	}
	// Unified file logging (<loom home>/logs/loom.YYYY-MM-DD.log, glog
	// style); the TUI owns the screen, so a broken log directory degrades
	// to a discard logger rather than interrupting the interaction.
	logger := newFileLogger(resolved, slog.New(slog.NewTextHandler(io.Discard, nil)))
	proc, registry, bootstrap, err := assembleRuntime(ctx, resolved, root, logger)
	if err != nil {
		return err
	}
	defer proc.Close()
	defer registry.Close()

	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	// Bridge delegate_task child-run lifecycle onto the event stream so the
	// TUI can show live sub-agent progress and the read-only drill-in view.
	app.WireSubagentObserver(bootstrap.SubagentFactory, broker, bootstrap.Store, logger)
	// The TUI is a peer client of the runtime (docs/SERVE_DESIGN.md §10):
	// same SessionService + in-proc client assembly that `loom serve` uses,
	// so every frontend shares one behavior.
	service := app.NewSessionService(proc, registry, broker, app.SessionServiceConfig{
		Logger:   logger,
		RulesDir: resolved.Storage.RulesDir(),
	})
	sessionClient := client.NewInProc(service)

	if resumeSessionID != nil {
		if err := sessionClient.ResumeSession(ctx, *resumeSessionID); err != nil {
			return fmt.Errorf("resume session: %w", err)
		}
	} else if err := sessionClient.NewSession(ctx); err != nil {
		return fmt.Errorf("new session: %w", err)
	}

	// Dumb terminals usually lack a Nerd Font patched font, so they fall
	// back to plain text icons unless ui.icons says otherwise.
	icons := resolved.UI.Icons
	if icons == "" && os.Getenv("TERM") == "dumb" {
		icons = "plain"
	}
	meta, _ := resolved.ModelMeta(bootstrap.CurrentModel())
	contextCfg := resolved.Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	// The status bar measures context occupancy against the effective
	// window so its warning aligns with the compaction trigger.
	effectiveWindow := agent.NewWindowModel(meta.ContextWindow, resolved.Limits.MaxInputTokens, contextCfg).Effective
	opts := ui.InitOptions{
		NoColor:       os.Getenv("NO_COLOR") != "" || os.Getenv("TERM") == "dumb",
		AltScreen:     resolved.UI.AltScreen,
		Icons:         icons,
		Limits:        resolved.Limits,
		ContextWindow: int(effectiveWindow),
		Models:        modelCatalog(resolved),
		Keymap:        resolved.UI.Keymap,
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = service.Shutdown(shutdownCtx)
		broker.Close()
	}()
	return ui.StartTUI(sessionClient, bootstrap.CurrentModel().String(), root, opts)
}

// modelCatalog flattens the resolved providers into the picker's static
// option list, preserving the config file's declaration order.
func modelCatalog(resolved *config.ResolvedConfig) []ui.ModelOption {
	var models []ui.ModelOption
	for i := range resolved.Providers {
		p := &resolved.Providers[i]
		for _, mo := range p.Models {
			models = append(models, ui.ModelOption{
				Provider:      p.Name,
				Name:          mo.Name,
				ContextWindow: mo.ContextWindow,
				WireAPI:       mo.WireAPI,
			})
		}
	}
	return models
}
