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

// Package mcp implements a Model Context Protocol client: it connects to
// MCP servers over the stdio transport (spawning them as child
// processes) or the streamable HTTP transport (POSTing JSON-RPC to a
// remote endpoint), performs the initialize handshake, discovers tools,
// and proxies tool calls. Discovered tools are adapted into domain.Tool
// values so MCP servers plug into loom's permission, signing, and
// approval machinery like any built-in tool.
package mcp

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"sync/atomic"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	// protocolVersion is the MCP revision loom speaks. Servers negotiating
	// an older revision answer with their own version, which we adopt (the
	// methods loom uses — initialize, tools/list, tools/call — are stable
	// across revisions).
	protocolVersion = "2025-06-18"

	defaultStartupTimeout = 30 * time.Second
	defaultToolTimeout    = 300 * time.Second

	maxMessageBytes = 16 << 20
)

// ClientConfig describes one MCP server connection. Exactly one of
// Command (stdio transport: spawn a child process) or URL (streamable
// HTTP transport: POST to a remote endpoint) must be set.
type ClientConfig struct {
	Command string
	Args    []string
	// Env entries are injected on top of the inherited process
	// environment (unlike codex's env_clear, loom keeps PATH/HOME so
	// launcher commands like npx resolve the same way they do in the
	// user's shell).
	Env map[string]string
	Cwd string
	// URL selects the streamable HTTP transport; Headers carries static
	// per-request headers such as Authorization.
	URL     string
	Headers map[string]string
	// StartupTimeout bounds connect+initialize; ToolTimeout bounds one
	// tools/call. Zero selects the defaults (30s / 300s).
	StartupTimeout time.Duration
	ToolTimeout    time.Duration
	// Logger receives server diagnostics and protocol anomalies; nil
	// selects slog.Default().
	Logger *slog.Logger
	// name is set by the manager for log attribution.
	name string
}

// ToolSpec is one tool discovered from a server.
type ToolSpec struct {
	Name        string          `json:"name"`
	Description string          `json:"description,omitempty"`
	InputSchema json.RawMessage `json:"inputSchema"`
	Annotations *struct {
		ReadOnlyHint    bool `json:"readOnlyHint,omitempty"`
		DestructiveHint bool `json:"destructiveHint,omitempty"`
		OpenWorldHint   bool `json:"openWorldHint,omitempty"`
	} `json:"annotations,omitempty"`
}

// CallToolResult is the tools/call response payload.
type CallToolResult struct {
	Content []struct {
		Type     string `json:"type"`
		Text     string `json:"text,omitempty"`
		Data     string `json:"data,omitempty"`
		MimeType string `json:"mimeType,omitempty"`
	} `json:"content"`
	IsError bool `json:"isError,omitempty"`
}

// Client is one running MCP server connection.
type Client struct {
	cfg       ClientConfig
	transport transport
	logger    *slog.Logger

	nextID atomic.Int64
	closed atomic.Bool

	serverName    string
	serverVersion string
}

// Start connects to the server and performs the initialize handshake
// within cfg.StartupTimeout.
func Start(ctx context.Context, cfg ClientConfig) (*Client, error) {
	hasCommand := strings.TrimSpace(cfg.Command) != ""
	hasURL := strings.TrimSpace(cfg.URL) != ""
	if hasCommand == hasURL {
		return nil, domain.NewError(domain.ErrInvalidInput, "mcp: exactly one of command or url is required")
	}
	if cfg.StartupTimeout <= 0 {
		cfg.StartupTimeout = defaultStartupTimeout
	}
	if cfg.ToolTimeout <= 0 {
		cfg.ToolTimeout = defaultToolTimeout
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	var tr transport
	var err error
	if hasURL {
		tr, err = newHTTPTransport(cfg, logger)
	} else {
		tr, err = startStdioTransport(cfg, logger)
	}
	if err != nil {
		return nil, err
	}

	client := &Client{cfg: cfg, transport: tr, logger: logger}
	handshakeCtx, cancel := context.WithTimeout(ctx, cfg.StartupTimeout)
	defer cancel()
	if err := client.initialize(handshakeCtx); err != nil {
		_ = client.Close()
		return nil, err
	}
	return client, nil
}

func (c *Client) initialize(ctx context.Context) error {
	params := map[string]any{
		"protocolVersion": protocolVersion,
		"capabilities":    map[string]any{},
		"clientInfo":      map[string]any{"name": "loom", "version": "0.1"},
	}
	var result struct {
		ProtocolVersion string `json:"protocolVersion"`
		ServerInfo      struct {
			Name    string `json:"name"`
			Version string `json:"version"`
		} `json:"serverInfo"`
	}
	target := c.cfg.Command
	if target == "" {
		target = c.cfg.URL
	}
	if err := c.call(ctx, "initialize", params, &result); err != nil {
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: initialize handshake with %q failed", target), domain.WithCause(err))
	}
	c.serverName = result.ServerInfo.Name
	c.serverVersion = result.ServerInfo.Version

	notification, err := marshalNotification("notifications/initialized", nil)
	if err != nil {
		return err
	}
	return c.transport.notify(ctx, notification)
}

// call sends one request and waits for the matching response, honoring ctx.
func (c *Client) call(ctx context.Context, method string, params any, out any) error {
	if c.closed.Load() {
		return domain.NewError(domain.ErrUnavailable, "mcp: client is closed")
	}
	id := c.nextID.Add(1)
	request, err := marshalRequest(id, method, params)
	if err != nil {
		return domain.NewError(domain.ErrInternal, "mcp: failed to encode request", domain.WithCause(err))
	}
	msg, err := c.transport.roundTrip(ctx, id, request)
	if err != nil {
		return err
	}
	if msg.Error != nil {
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: %s failed: %s", method, msg.Error.Message))
	}
	if out != nil && len(msg.Result) > 0 {
		if err := json.Unmarshal(msg.Result, out); err != nil {
			return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: malformed %s result", method), domain.WithCause(err))
		}
	}
	return nil
}

// ListTools discovers every tool the server offers.
func (c *Client) ListTools(ctx context.Context) ([]ToolSpec, error) {
	var result struct {
		Tools []ToolSpec `json:"tools"`
	}
	listCtx, cancel := context.WithTimeout(ctx, c.cfg.StartupTimeout)
	defer cancel()
	if err := c.call(listCtx, "tools/list", map[string]any{}, &result); err != nil {
		return nil, err
	}
	return result.Tools, nil
}

// CallTool invokes one tool with cfg.ToolTimeout.
func (c *Client) CallTool(ctx context.Context, name string, arguments json.RawMessage) (CallToolResult, error) {
	var args any
	if len(arguments) > 0 {
		if err := json.Unmarshal(arguments, &args); err != nil {
			return CallToolResult{}, domain.NewError(domain.ErrInvalidInput, "mcp: tool arguments must be valid JSON", domain.WithCause(err))
		}
	} else {
		args = map[string]any{}
	}
	callCtx, cancel := context.WithTimeout(ctx, c.cfg.ToolTimeout)
	defer cancel()
	var result CallToolResult
	if err := c.call(callCtx, "tools/call", map[string]any{"name": name, "arguments": args}, &result); err != nil {
		return CallToolResult{}, err
	}
	return result, nil
}

// ServerInfo describes the connected server (for diagnostics).
func (c *Client) ServerInfo() (string, string) {
	return c.serverName, c.serverVersion
}

// Close terminates the connection (and the subprocess, for stdio).
func (c *Client) Close() error {
	if c.closed.CompareAndSwap(false, true) {
		return c.transport.close()
	}
	return nil
}
