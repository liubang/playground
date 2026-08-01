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

// Package mcp implements a Model Context Protocol client over the stdio
// transport: it spawns MCP servers as child processes, performs the
// initialize handshake, discovers tools, and proxies tool calls over
// newline-delimited JSON-RPC. Discovered tools are adapted into
// domain.Tool values so MCP servers plug into loom's permission,
// signing, and approval machinery like any built-in tool.
package mcp

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"strings"
	"sync"
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

// ClientConfig describes one MCP server subprocess.
type ClientConfig struct {
	Command string
	Args    []string
	// Env entries are injected on top of the inherited process
	// environment (unlike codex's env_clear, loom keeps PATH/HOME so
	// launcher commands like npx resolve the same way they do in the
	// user's shell).
	Env map[string]string
	Cwd string
	// StartupTimeout bounds spawn+initialize; ToolTimeout bounds one
	// tools/call. Zero selects the defaults (30s / 300s).
	StartupTimeout time.Duration
	ToolTimeout    time.Duration
	// Logger receives server stderr lines and protocol anomalies; nil
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
	cfg    ClientConfig
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	logger *slog.Logger

	nextID  atomic.Int64
	mu      sync.Mutex // guards pending + closed
	pending map[int64]chan rpcMessage
	closed  bool

	serverName    string
	serverVersion string
}

// Start launches the server subprocess and performs the initialize
// handshake within cfg.StartupTimeout.
func Start(ctx context.Context, cfg ClientConfig) (*Client, error) {
	if strings.TrimSpace(cfg.Command) == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "mcp server command is required")
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

	cmd := exec.Command(cfg.Command, cfg.Args...)
	if cfg.Cwd != "" {
		cmd.Dir = cfg.Cwd
	}
	cmd.Env = mergeEnv(os.Environ(), cfg.Env)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: stdin pipe failed", domain.WithCause(err))
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: stdout pipe failed", domain.WithCause(err))
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: stderr pipe failed", domain.WithCause(err))
	}
	if err := cmd.Start(); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: failed to start %q", cfg.Command), domain.WithCause(err))
	}

	client := &Client{
		cfg:     cfg,
		cmd:     cmd,
		stdin:   stdin,
		logger:  logger,
		pending: make(map[int64]chan rpcMessage),
	}
	go client.readLoop(stdout)
	go client.logStderr(stderr)

	handshakeCtx, cancel := context.WithTimeout(ctx, cfg.StartupTimeout)
	defer cancel()
	if err := client.initialize(handshakeCtx); err != nil {
		_ = client.Close()
		return nil, err
	}
	return client, nil
}

func mergeEnv(base []string, extra map[string]string) []string {
	if len(extra) == 0 {
		return base
	}
	out := append([]string(nil), base...)
	for k, v := range extra {
		out = append(out, k+"="+v)
	}
	return out
}

func (c *Client) logStderr(stderr io.Reader) {
	scanner := bufio.NewScanner(stderr)
	scanner.Buffer(make([]byte, 64<<10), 1<<20)
	for scanner.Scan() {
		c.logger.Debug("mcp server stderr", "server", c.cfg.name, "line", scanner.Text())
	}
}

// readLoop dispatches inbound messages: responses resolve their pending
// channel; notifications and server->client requests are logged and
// dropped (loom advertises no client capabilities).
func (c *Client) readLoop(stdout io.Reader) {
	scanner := bufio.NewScanner(stdout)
	scanner.Buffer(make([]byte, 1<<20), maxMessageBytes)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var msg rpcMessage
		if err := json.Unmarshal(line, &msg); err != nil {
			c.logger.Warn("mcp: dropping malformed message", "server", c.cfg.name, "error", err)
			continue
		}
		if msg.ID == nil {
			continue // notification
		}
		if msg.Method != "" {
			c.logger.Debug("mcp: dropping unsupported server request", "server", c.cfg.name, "method", msg.Method)
			continue
		}
		c.mu.Lock()
		ch, ok := c.pending[*msg.ID]
		delete(c.pending, *msg.ID)
		c.mu.Unlock()
		if ok {
			ch <- msg
			close(ch)
		}
	}
	// Transport died: fail every pending call so waiters never hang.
	c.mu.Lock()
	for id, ch := range c.pending {
		ch <- rpcMessage{Error: &rpcError{Code: -32000, Message: "mcp: server closed the connection"}}
		close(ch)
		delete(c.pending, id)
	}
	c.mu.Unlock()
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
	if err := c.call(ctx, "initialize", params, &result); err != nil {
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: initialize handshake with %q failed", c.cfg.Command), domain.WithCause(err))
	}
	c.serverName = result.ServerInfo.Name
	c.serverVersion = result.ServerInfo.Version

	notification, err := marshalNotification("notifications/initialized", nil)
	if err != nil {
		return err
	}
	if _, err := c.stdin.Write(notification); err != nil {
		return domain.NewError(domain.ErrUnavailable, "mcp: failed to send initialized notification", domain.WithCause(err))
	}
	return nil
}

// call sends one request and waits for the matching response, honoring ctx.
func (c *Client) call(ctx context.Context, method string, params any, out any) error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return domain.NewError(domain.ErrUnavailable, "mcp: client is closed")
	}
	id := c.nextID.Add(1)
	ch := make(chan rpcMessage, 1)
	c.pending[id] = ch
	c.mu.Unlock()

	request, err := marshalRequest(id, method, params)
	if err != nil {
		return domain.NewError(domain.ErrInternal, "mcp: failed to encode request", domain.WithCause(err))
	}
	if _, err := c.stdin.Write(request); err != nil {
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		return domain.NewError(domain.ErrUnavailable, "mcp: failed to write request", domain.WithCause(err))
	}

	select {
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		return ctx.Err()
	case msg := <-ch:
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

// Close terminates the connection and the subprocess.
func (c *Client) Close() error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return nil
	}
	c.closed = true
	c.mu.Unlock()

	_ = c.stdin.Close()
	if c.cmd.Process != nil {
		_ = c.cmd.Process.Kill()
	}
	err := c.cmd.Wait()
	if err != nil && !errors.Is(err, os.ErrProcessDone) {
		var exitErr *exec.ExitError
		if !errors.As(err, &exitErr) {
			return err
		}
	}
	return nil
}
