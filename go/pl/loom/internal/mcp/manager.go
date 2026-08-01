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
// Created: 2026/08/01

package mcp

import (
	"context"
	"fmt"
	"log/slog"
	"sort"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ServerConfig is one configured MCP server (from mcp_servers in the config
// file). Only the stdio transport is supported.
type ServerConfig struct {
	Command string            `yaml:"command"`
	Args    []string          `yaml:"args"`
	Env     map[string]string `yaml:"env"`
	Cwd     string            `yaml:"cwd"`
	// StartupTimeoutSec bounds spawn+initialize (default 30s);
	// ToolTimeoutSec bounds one tools/call (default 300s).
	StartupTimeoutSec float64 `yaml:"startup_timeout_sec"`
	ToolTimeoutSec    float64 `yaml:"tool_timeout_sec"`
	// EnabledTools/DisabledTools filter the discovered catalog by the
	// server-local tool names. EnabledTools nil means "all".
	EnabledTools  []string `yaml:"enabled_tools"`
	DisabledTools []string `yaml:"disabled_tools"`
}

func (c ServerConfig) clientConfig(name string, logger *slog.Logger) ClientConfig {
	return ClientConfig{
		Command:        c.Command,
		Args:           append([]string(nil), c.Args...),
		Env:            c.Env,
		Cwd:            c.Cwd,
		StartupTimeout: secondsOr(c.StartupTimeoutSec, defaultStartupTimeout),
		ToolTimeout:    secondsOr(c.ToolTimeoutSec, defaultToolTimeout),
		Logger:         logger,
		name:           name,
	}
}

func secondsOr(sec float64, fallback time.Duration) time.Duration {
	if sec <= 0 {
		return fallback
	}
	return time.Duration(sec * float64(time.Second))
}

func (c ServerConfig) allows(tool string) bool {
	if len(c.EnabledTools) > 0 {
		found := false
		for _, name := range c.EnabledTools {
			if name == tool {
				found = true
				break
			}
		}
		if !found {
			return false
		}
	}
	for _, name := range c.DisabledTools {
		if name == tool {
			return false
		}
	}
	return true
}

// Manager owns every running MCP server and the tools adapted from them.
type Manager struct {
	clients []*Client
	tools   []domain.Tool
	logger  *slog.Logger
}

// StartServers launches every configured server concurrently, mirroring
// codex's McpConnectionSet: one server's failure never blocks the others —
// it is logged and its tools are simply absent. Returns (nil, nil) when no
// servers are configured.
func StartServers(ctx context.Context, cfgs map[string]ServerConfig, logger *slog.Logger) (*Manager, error) {
	if len(cfgs) == 0 {
		return nil, nil
	}
	if logger == nil {
		logger = slog.Default()
	}

	names := make([]string, 0, len(cfgs))
	for name := range cfgs {
		names = append(names, name)
	}
	sort.Strings(names)

	type outcome struct {
		name   string
		tool   domain.Tool
		client *Client
	}
	results := make(chan outcome)
	var wg sync.WaitGroup
	for _, name := range names {
		name, cfg := name, cfgs[name]
		wg.Add(1)
		go func() {
			defer wg.Done()
			client, err := Start(ctx, cfg.clientConfig(name, logger))
			if err != nil {
				logger.Warn("mcp server failed to start; its tools are unavailable", "server", name, "error", err)
				return
			}
			specs, err := client.ListTools(ctx)
			if err != nil {
				logger.Warn("mcp server tools/list failed; its tools are unavailable", "server", name, "error", err)
				_ = client.Close()
				return
			}
			results <- outcome{name: name, client: client}
			for _, spec := range specs {
				if !cfg.allows(spec.Name) {
					continue
				}
				tool, err := NewToolAdapter(client, name, spec)
				if err != nil {
					logger.Warn("mcp tool rejected", "server", name, "tool", spec.Name, "error", err)
					continue
				}
				results <- outcome{name: name, tool: tool}
			}
		}()
	}
	go func() {
		wg.Wait()
		close(results)
	}()

	manager := &Manager{logger: logger}
	toolNames := make(map[string]string) // qualified name -> "server/tool" for collision logging
	for out := range results {
		if out.client != nil {
			manager.clients = append(manager.clients, out.client)
			continue
		}
		if out.tool != nil {
			if prev, dup := toolNames[out.tool.Definition().Name]; dup {
				manager.logger.Warn("mcp tool name collision; keeping the first", "tool", out.tool.Definition().Name, "kept", prev, "dropped", out.name)
				continue
			}
			toolNames[out.tool.Definition().Name] = out.name
			manager.tools = append(manager.tools, out.tool)
		}
	}
	sort.Slice(manager.tools, func(i, j int) bool {
		return manager.tools[i].Definition().Name < manager.tools[j].Definition().Name
	})
	if len(manager.clients) == 0 {
		return nil, fmt.Errorf("no mcp server could be started")
	}
	return manager, nil
}

// Tools returns the adapted tools from every connected server.
func (m *Manager) Tools() []domain.Tool {
	return append([]domain.Tool(nil), m.tools...)
}

// Close shuts down every server, last-writer-wins on errors.
func (m *Manager) Close() error {
	var firstErr error
	for _, client := range m.clients {
		if err := client.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}
