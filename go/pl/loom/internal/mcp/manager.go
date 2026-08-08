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
	"reflect"
	"sort"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ServerConfig is one configured MCP server (from mcp_servers in the
// config file). Command selects the stdio transport, URL the streamable
// HTTP transport; exactly one is set (the config loader enforces this).
type ServerConfig struct {
	Command string            `yaml:"command,omitempty"`
	Args    []string          `yaml:"args,omitempty"`
	Env     map[string]string `yaml:"env,omitempty"`
	Cwd     string            `yaml:"cwd,omitempty"`
	// URL/Headers configure the streamable HTTP transport; Headers
	// carries static per-request headers such as Authorization.
	URL     string            `yaml:"url,omitempty"`
	Headers map[string]string `yaml:"headers,omitempty"`
	// StartupTimeoutSec bounds spawn+initialize (default 30s);
	// ToolTimeoutSec bounds one tools/call (default 300s).
	StartupTimeoutSec float64 `yaml:"startup_timeout_sec,omitempty"`
	ToolTimeoutSec    float64 `yaml:"tool_timeout_sec,omitempty"`
	// EnabledTools/DisabledTools filter the discovered catalog by the
	// server-local tool names. EnabledTools nil means "all".
	EnabledTools  []string `yaml:"enabled_tools,omitempty"`
	DisabledTools []string `yaml:"disabled_tools,omitempty"`
}

func (c ServerConfig) clientConfig(name string, logger *slog.Logger) ClientConfig {
	return ClientConfig{
		Command: c.Command,
		Args:    append([]string(nil), c.Args...),
		// Env/Headers are shared with the config map by design: nothing
		// mutates them after load (reconnects read, never write).
		Env:            c.Env,
		Cwd:            c.Cwd,
		URL:            c.URL,
		Headers:        c.Headers,
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

// ServerStatus is the read-only projection of one configured MCP server,
// backing frontend listings (/mcp). A server that failed to start or to
// answer tools/list is reported with Connected=false and the failure
// reason, so the listing can explain why its tools are absent.
type ServerStatus struct {
	Name      string `json:"name"`
	Connected bool   `json:"connected"`
	// Error carries the startup/tools-list failure when Connected is false.
	Error string `json:"error,omitempty"`
	// Tools lists the exposed (filter-passing, collision-surviving)
	// qualified tool names when Connected is true.
	Tools []string `json:"tools,omitempty"`
}

// serverEntry is one configured server's live state.
type serverEntry struct {
	cfg    ServerConfig
	client *Client       // nil when the last connect attempt failed
	tools  []domain.Tool // adapted, filter-passing tools
	err    string        // last startup/discovery failure ("" when connected)
}

func (e *serverEntry) status(name string) ServerStatus {
	s := ServerStatus{Name: name, Connected: e.client != nil, Error: e.err}
	for _, t := range e.tools {
		s.Tools = append(s.Tools, t.Definition().Name)
	}
	sort.Strings(s.Tools)
	return s
}

// Manager owns every running MCP server and the tools adapted from them.
// It is safe for concurrent use: lifecycle operations (Add/Remove/
// Reconcile/Close) are serialized (they are rare and may block for the
// startup timeout), while readers take consistent snapshots.
type Manager struct {
	logger *slog.Logger

	mgmtMu  sync.Mutex
	mu      sync.RWMutex
	entries map[string]*serverEntry
	closed  bool
}

// NewManager returns an empty manager; servers join via Add/Reconcile.
func NewManager(logger *slog.Logger) *Manager {
	if logger == nil {
		logger = slog.Default()
	}
	return &Manager{logger: logger, entries: make(map[string]*serverEntry)}
}

// StartServers launches every configured server concurrently, mirroring
// codex's McpConnectionSet: one server's failure never blocks the others —
// it is logged and its tools are simply absent. Returns (nil, nil) when no
// servers are configured. When EVERY server fails it returns a non-nil
// Manager alongside the error so callers can still inspect Servers() for
// the per-server failure reasons.
func StartServers(ctx context.Context, cfgs map[string]ServerConfig, logger *slog.Logger) (*Manager, error) {
	if len(cfgs) == 0 {
		return nil, nil
	}
	manager := NewManager(logger)

	names := make([]string, 0, len(cfgs))
	for name := range cfgs {
		names = append(names, name)
	}
	sort.Strings(names)

	type outcome struct {
		name  string
		entry *serverEntry
	}
	results := make(chan outcome)
	var wg sync.WaitGroup
	for _, name := range names {
		name, cfg := name, cfgs[name]
		wg.Add(1)
		go func() {
			defer wg.Done()
			results <- outcome{name: name, entry: manager.connect(ctx, name, cfg)}
		}()
	}
	go func() {
		wg.Wait()
		close(results)
	}()

	connected := 0
	for out := range results {
		if out.entry.client != nil {
			connected++
		}
		manager.entries[out.name] = out.entry
	}
	if connected == 0 {
		return manager, fmt.Errorf("no mcp server could be started")
	}
	return manager, nil
}

// connect starts one server and adapts its tools; the failure modes are
// recorded on the returned entry, never thrown away silently.
func (m *Manager) connect(ctx context.Context, name string, cfg ServerConfig) *serverEntry {
	entry := &serverEntry{cfg: cfg}
	client, err := Start(ctx, cfg.clientConfig(name, m.logger))
	if err != nil {
		m.logger.Warn("mcp server failed to start; its tools are unavailable", "server", name, "error", err)
		entry.err = fmt.Errorf("start: %w", err).Error()
		return entry
	}
	specs, err := client.ListTools(ctx)
	if err != nil {
		m.logger.Warn("mcp server tools/list failed; its tools are unavailable", "server", name, "error", err)
		_ = client.Close()
		entry.err = fmt.Errorf("tools/list: %w", err).Error()
		return entry
	}
	entry.client = client
	for _, spec := range specs {
		if !cfg.allows(spec.Name) {
			continue
		}
		tool, err := NewToolAdapter(client, name, spec)
		if err != nil {
			m.logger.Warn("mcp tool rejected", "server", name, "tool", spec.Name, "error", err)
			continue
		}
		entry.tools = append(entry.tools, tool)
	}
	sort.Slice(entry.tools, func(i, j int) bool {
		return entry.tools[i].Definition().Name < entry.tools[j].Definition().Name
	})
	return entry
}

// Add (re)connects one server: an existing entry with the same name is
// shut down first. The outcome is reported through the returned status —
// a failed connect leaves an entry recording the error, so frontends can
// explain why its tools are absent. Add on a closed manager is an error
// status, never a respawn.
func (m *Manager) Add(ctx context.Context, name string, cfg ServerConfig) ServerStatus {
	m.mgmtMu.Lock()
	defer m.mgmtMu.Unlock()
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return ServerStatus{Name: name, Error: "mcp manager is closed"}
	}
	if old, ok := m.entries[name]; ok && old.client != nil {
		_ = old.client.Close()
	}
	m.mu.Unlock()
	entry := m.connect(ctx, name, cfg)
	m.mu.Lock()
	m.entries[name] = entry
	m.mu.Unlock()
	return entry.status(name)
}

// Remove shuts down and forgets one server; unknown names are no-ops,
// and so is a closed manager.
func (m *Manager) Remove(name string) {
	m.mgmtMu.Lock()
	defer m.mgmtMu.Unlock()
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.closed {
		return
	}
	if e, ok := m.entries[name]; ok {
		if e.client != nil {
			_ = e.client.Close()
		}
		delete(m.entries, name)
	}
}

// Reconcile diffs the manager against the desired configuration: removed
// servers shut down, added servers connect, servers whose configuration
// changed reconnect. Unchanged servers keep their live connection. New
// and changed servers connect concurrently (a slow server never stalls
// the others). Reconcile on a closed manager is a no-op.
func (m *Manager) Reconcile(ctx context.Context, cfgs map[string]ServerConfig) {
	m.mgmtMu.Lock()
	defer m.mgmtMu.Unlock()
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	var removed []string
	toConnect := make(map[string]ServerConfig)
	for name, e := range m.entries {
		if _, ok := cfgs[name]; !ok {
			removed = append(removed, name)
			if e.client != nil {
				_ = e.client.Close()
			}
			delete(m.entries, name)
		}
	}
	for name, cfg := range cfgs {
		e, ok := m.entries[name]
		switch {
		case !ok:
			toConnect[name] = cfg
		case !serverConfigEqual(e.cfg, cfg):
			if e.client != nil {
				_ = e.client.Close()
			}
			toConnect[name] = cfg
		}
	}
	m.mu.Unlock()

	type outcome struct {
		name  string
		entry *serverEntry
	}
	results := make(chan outcome, len(toConnect))
	var wg sync.WaitGroup
	for name, cfg := range toConnect {
		name, cfg := name, cfg
		wg.Add(1)
		go func() {
			defer wg.Done()
			results <- outcome{name: name, entry: m.connect(ctx, name, cfg)}
		}()
	}
	wg.Wait()
	close(results)
	m.mu.Lock()
	for out := range results {
		m.entries[out.name] = out.entry
	}
	m.mu.Unlock()
	if len(toConnect)+len(removed) > 0 {
		names := make([]string, 0, len(toConnect))
		for name := range toConnect {
			names = append(names, name)
		}
		sort.Strings(names)
		m.logger.Info("mcp servers reconciled", "connected", names, "removed", removed)
	}
}

// serverConfigEqual compares two server configs semantically: nil and
// empty slices/maps are equivalent (a hand-written `args: []` must not
// trigger a needless reconnect against a UI-saved omission).
func serverConfigEqual(a, b ServerConfig) bool {
	norm := func(c ServerConfig) ServerConfig {
		if len(c.Args) == 0 {
			c.Args = nil
		}
		if len(c.Env) == 0 {
			c.Env = nil
		}
		if len(c.Headers) == 0 {
			c.Headers = nil
		}
		if len(c.EnabledTools) == 0 {
			c.EnabledTools = nil
		}
		if len(c.DisabledTools) == 0 {
			c.DisabledTools = nil
		}
		return c
	}
	return reflect.DeepEqual(norm(a), norm(b))
}

// Servers returns every configured server's status, ordered by server name.
func (m *Manager) Servers() []ServerStatus {
	m.mu.RLock()
	defer m.mu.RUnlock()
	names := make([]string, 0, len(m.entries))
	for name := range m.entries {
		names = append(names, name)
	}
	sort.Strings(names)
	out := make([]ServerStatus, 0, len(names))
	for _, name := range names {
		out = append(out, m.entries[name].status(name))
	}
	return out
}

// Status returns one server's status; ok=false when the name is unknown.
func (m *Manager) Status(name string) (ServerStatus, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	e, ok := m.entries[name]
	if !ok {
		return ServerStatus{}, false
	}
	return e.status(name), true
}

// Tools returns the adapted tools from every connected server, ordered by
// qualified name.
func (m *Manager) Tools() []domain.Tool {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []domain.Tool
	for _, e := range m.entries {
		out = append(out, e.tools...)
	}
	sort.Slice(out, func(i, j int) bool {
		return out[i].Definition().Name < out[j].Definition().Name
	})
	return out
}

// Close shuts down every server, last-writer-wins on errors.
func (m *Manager) Close() error {
	m.mgmtMu.Lock()
	defer m.mgmtMu.Unlock()
	m.mu.Lock()
	defer m.mu.Unlock()
	var firstErr error
	for _, e := range m.entries {
		if e.client != nil {
			if err := e.client.Close(); err != nil && firstErr == nil {
				firstErr = err
			}
		}
	}
	m.entries = make(map[string]*serverEntry)
	m.closed = true
	return firstErr
}
