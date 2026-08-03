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
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// stdioTransport speaks MCP over a server subprocess's pipes: requests
// are newline-delimited JSON on stdin, responses arrive interleaved with
// notifications on stdout, and stderr is diagnostic output worth logging.
type stdioTransport struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	logger *slog.Logger
	name   string

	writeMu sync.Mutex // serializes stdin writes (frames must not interleave)
	mu      sync.Mutex // guards pending + closed
	pending map[int64]chan rpcMessage
	closed  bool
}

// startStdioTransport launches the server subprocess; the initialize
// handshake is the Client's job.
func startStdioTransport(cfg ClientConfig, logger *slog.Logger) (*stdioTransport, error) {
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

	t := &stdioTransport{
		cmd:     cmd,
		stdin:   stdin,
		logger:  logger,
		name:    cfg.name,
		pending: make(map[int64]chan rpcMessage),
	}
	go t.readLoop(stdout)
	go t.logStderr(stderr)
	return t, nil
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

func (t *stdioTransport) logStderr(stderr io.Reader) {
	scanner := bufio.NewScanner(stderr)
	scanner.Buffer(make([]byte, 64<<10), 1<<20)
	for scanner.Scan() {
		t.logger.Debug("mcp server stderr", "server", t.name, "line", scanner.Text())
	}
}

// readLoop dispatches inbound messages: responses resolve their pending
// channel; notifications and server->client requests are logged and
// dropped (loom advertises no client capabilities).
func (t *stdioTransport) readLoop(stdout io.Reader) {
	scanner := bufio.NewScanner(stdout)
	scanner.Buffer(make([]byte, 1<<20), maxMessageBytes)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var msg rpcMessage
		if err := json.Unmarshal(line, &msg); err != nil {
			t.logger.Warn("mcp: dropping malformed message", "server", t.name, "error", err)
			continue
		}
		if msg.ID == nil {
			continue // notification
		}
		if msg.Method != "" {
			t.logger.Debug("mcp: dropping unsupported server request", "server", t.name, "method", msg.Method)
			continue
		}
		t.mu.Lock()
		ch, ok := t.pending[*msg.ID]
		delete(t.pending, *msg.ID)
		t.mu.Unlock()
		if ok {
			ch <- msg
			close(ch)
		}
	}
	// Transport died: fail every pending call so waiters never hang.
	t.mu.Lock()
	for id, ch := range t.pending {
		ch <- rpcMessage{Error: &rpcError{Code: -32000, Message: "mcp: server closed the connection"}}
		close(ch)
		delete(t.pending, id)
	}
	t.mu.Unlock()
}

// writeFrame sends one message under the write lock so concurrent
// frames never interleave on the pipe.
func (t *stdioTransport) writeFrame(msg []byte) error {
	t.writeMu.Lock()
	defer t.writeMu.Unlock()
	_, err := t.stdin.Write(append(msg, '\n'))
	return err
}

func (t *stdioTransport) roundTrip(ctx context.Context, id int64, request []byte) (rpcMessage, error) {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return rpcMessage{}, domain.NewError(domain.ErrUnavailable, "mcp: client is closed")
	}
	ch := make(chan rpcMessage, 1)
	t.pending[id] = ch
	t.mu.Unlock()

	if err := t.writeFrame(request); err != nil {
		t.mu.Lock()
		delete(t.pending, id)
		t.mu.Unlock()
		return rpcMessage{}, domain.NewError(domain.ErrUnavailable, "mcp: failed to write request", domain.WithCause(err))
	}

	select {
	case <-ctx.Done():
		t.mu.Lock()
		delete(t.pending, id)
		t.mu.Unlock()
		return rpcMessage{}, ctx.Err()
	case msg := <-ch:
		return msg, nil
	}
}

func (t *stdioTransport) notify(_ context.Context, notification []byte) error {
	t.mu.Lock()
	closed := t.closed
	t.mu.Unlock()
	if closed {
		return domain.NewError(domain.ErrUnavailable, "mcp: client is closed")
	}
	if err := t.writeFrame(notification); err != nil {
		return domain.NewError(domain.ErrUnavailable, "mcp: failed to send initialized notification", domain.WithCause(err))
	}
	return nil
}

// close terminates the connection and the subprocess.
func (t *stdioTransport) close() error {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return nil
	}
	t.closed = true
	t.mu.Unlock()

	_ = t.stdin.Close()
	if t.cmd.Process != nil {
		_ = t.cmd.Process.Kill()
	}
	err := t.cmd.Wait()
	if err != nil && !errors.Is(err, os.ErrProcessDone) {
		var exitErr *exec.ExitError
		if !errors.As(err, &exitErr) {
			return err
		}
	}
	return nil
}
