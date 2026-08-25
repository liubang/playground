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

package e2e

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/tool/exsession"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// TestExecSessionE2E drives a scripted turn through the REAL loop: the
// model starts a short-lived session, the await-yield returns after the
// process exits, and the merged output lands in the transcript.
func TestExecSessionE2E(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	ws := t.TempDir()
	validator, err := workspacepkg.NewPathValidator(ws)
	if err != nil {
		t.Fatal(err)
	}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		t.Fatal(err)
	}
	probe, probeErr := runner.Run(context.Background(), process.CommandSpec{
		Program: "true", Cwd: ws, Timeout: 5 * time.Second,
	})
	switch {
	case errors.Is(probeErr, process.ErrSandboxUnavailable):
		t.Skip("platform sandbox unavailable on this machine")
	case probeErr != nil:
		t.Fatalf("sandbox probe error = %v", probeErr)
	case probe.ExitCode != 0 || strings.Contains(string(probe.Stderr), "sandbox_apply"):
		t.Skipf("nested sandbox-exec not permitted (stderr=%q)", probe.Stderr)
	}

	script := filepath.Join(ws, "tick.py")
	if err := os.WriteFile(script, []byte("print('boot', flush=True)\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	manager, err := exsession.NewManager(runner, nil, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	execTool, err := exsession.NewExecSessionTool(validator, manager)
	if err != nil {
		t.Fatal(err)
	}
	stdinTool, err := exsession.NewWriteStdinTool(manager)
	if err != nil {
		t.Fatal(err)
	}
	registry := agent.NewToolRegistry()
	if err := registry.Register(execTool); err != nil {
		t.Fatal(err)
	}
	if err := registry.Register(stdinTool); err != nil {
		t.Fatal(err)
	}

	callArgs, err := json.Marshal(map[string]any{
		"command":       python + " " + script,
		"working_dir":   ws,
		"yield_time_ms": 10000,
	})
	if err != nil {
		t.Fatal(err)
	}
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{{ID: domain.NewToolCallID(), Name: "exec_session", Arguments: callArgs}},
			StopReason: domain.StopToolUse, UsageIn: 10, UsageOut: 5,
		},
		fakes.ScriptEntry{Text: "session finished", StopReason: domain.StopEndTurn, UsageIn: 20, UsageOut: 8},
	)
	run := newRun(t, "run the session")
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake",
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Policy:   permission.DefaultPolicy(),
		Registry: registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(ws),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	// The exec_session result must be recorded in the transcript with the
	// exited status and the merged output.
	var toolResultText string
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				for _, content := range part.ToolResult.Content {
					if content.Kind == domain.PartText {
						toolResultText = content.Text
					}
				}
			}
		}
	}
	if toolResultText == "" {
		t.Fatal("no tool result recorded in transcript")
	}
	var payload struct {
		SessionID string `json:"session_id"`
		Status    string `json:"status"`
		ExitCode  int    `json:"exit_code"`
		Output    string `json:"output"`
	}
	if err := json.Unmarshal([]byte(toolResultText), &payload); err != nil {
		t.Fatalf("tool result is not the exec_session payload: %v (text=%s)", err, toolResultText)
	}
	if payload.SessionID == "" || payload.Status != "exited" || payload.ExitCode != 0 {
		t.Fatalf("unexpected payload: %+v", payload)
	}
	if !strings.Contains(payload.Output, "boot") {
		t.Fatalf("output = %q, want it to contain 'boot'", payload.Output)
	}
}

// TestWriteStdinE2E drives the full start → feed → observe arc through the
// REAL loop: exec_session starts an interactive process (R2, approved via
// the approver), then write_stdin feeds it input (R1, auto-approved by the
// baseline). Every write_stdin call used to die before execution with
// "prepared call risk drift detected": its per-call R1 sat below the
// definition's static R2 (CapProcessExec), which validatePreparedExecution
// rejects. This test routes the call through the loop — the only level
// where the drift check runs.
func TestWriteStdinE2E(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	ws := t.TempDir()
	validator, err := workspacepkg.NewPathValidator(ws)
	if err != nil {
		t.Fatal(err)
	}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		t.Fatal(err)
	}
	probe, probeErr := runner.Run(context.Background(), process.CommandSpec{
		Program: "true", Cwd: ws, Timeout: 5 * time.Second,
	})
	switch {
	case errors.Is(probeErr, process.ErrSandboxUnavailable):
		t.Skip("platform sandbox unavailable on this machine")
	case probeErr != nil:
		t.Fatalf("sandbox probe error = %v", probeErr)
	case probe.ExitCode != 0 || strings.Contains(string(probe.Stderr), "sandbox_apply"):
		t.Skipf("nested sandbox-exec not permitted (stderr=%q)", probe.Stderr)
	}

	script := filepath.Join(ws, "echo.py")
	if err := os.WriteFile(script, []byte("import sys\nfor line in sys.stdin:\n    print('got:' + line.strip(), flush=True)\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	manager, err := exsession.NewManager(runner, nil, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	execTool, err := exsession.NewExecSessionTool(validator, manager)
	if err != nil {
		t.Fatal(err)
	}
	stdinTool, err := exsession.NewWriteStdinTool(manager)
	if err != nil {
		t.Fatal(err)
	}
	registry := agent.NewToolRegistry()
	if err := registry.Register(execTool); err != nil {
		t.Fatal(err)
	}
	if err := registry.Register(stdinTool); err != nil {
		t.Fatal(err)
	}

	model := &sessionDrivenModel{command: python + " " + script, workingDir: ws}
	run := newRun(t, "feed the session")
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake",
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Policy:   permission.DefaultPolicy(),
		Registry: registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(ws),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	// The write_stdin result must be a success carrying the process echo;
	// a risk-drift rejection would surface as a tool error instead.
	var echoResult *domain.ToolResult
	for _, msg := range run.Messages {
		for _, part := range msg.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			if part.ToolResult.Error != nil && strings.Contains(part.ToolResult.Error.Message, "risk drift") {
				t.Fatalf("write_stdin rejected by risk drift: %+v", part.ToolResult.Error)
			}
			for _, content := range part.ToolResult.Content {
				if content.Kind == domain.PartText && strings.Contains(content.Text, "got:ping") {
					echoResult = part.ToolResult
				}
			}
		}
	}
	if echoResult == nil {
		t.Fatal("no write_stdin result containing 'got:ping' recorded in transcript")
	}
	if echoResult.Status != domain.ToolStatusSuccess {
		t.Fatalf("write_stdin result status = %s, want success (error=%+v)", echoResult.Status, echoResult.Error)
	}
}

// sessionDrivenModel scripts the flow a real model follows: start the
// session, then derive the write_stdin call from the session_id recorded
// in the transcript, then wrap up.
type sessionDrivenModel struct {
	mu         sync.Mutex
	turn       int
	command    string
	workingDir string
}

func (m *sessionDrivenModel) Stream(_ context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.turn++
	switch m.turn {
	case 1:
		args, err := json.Marshal(map[string]any{
			"command": m.command, "working_dir": m.workingDir, "yield_time_ms": 1000,
		})
		if err != nil {
			return nil, err
		}
		return toolCallStream("exec_session", args), nil
	case 2:
		sessionID := findSessionID(req.Messages)
		if sessionID == "" {
			return nil, fmt.Errorf("sessionDrivenModel: no session_id in transcript")
		}
		args, err := json.Marshal(map[string]any{
			"session_id": sessionID, "chars": "ping\n", "yield_time_ms": 2000,
		})
		if err != nil {
			return nil, err
		}
		return toolCallStream("write_stdin", args), nil
	default:
		return textStream("done"), nil
	}
}

func findSessionID(msgs []domain.Message) string {
	for _, msg := range msgs {
		for _, part := range msg.Parts {
			if part.Kind != domain.PartToolResult || part.ToolResult == nil {
				continue
			}
			for _, content := range part.ToolResult.Content {
				if content.Kind != domain.PartText {
					continue
				}
				var payload struct {
					SessionID string `json:"session_id"`
				}
				if json.Unmarshal([]byte(content.Text), &payload) == nil && payload.SessionID != "" {
					return payload.SessionID
				}
			}
		}
	}
	return ""
}

type sliceStream struct {
	events []domain.ModelEvent
	pos    int
}

func (s *sliceStream) Recv() (domain.ModelEvent, error) {
	if s.pos >= len(s.events) {
		return domain.ModelEvent{}, io.EOF
	}
	evt := s.events[s.pos]
	s.pos++
	return evt, nil
}

func (s *sliceStream) Close() error { return nil }

func toolCallStream(name string, args json.RawMessage) domain.ModelStream {
	return &sliceStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventToolCallStart, ToolIndex: 0, ToolID: domain.NewToolCallID().String(), ToolName: name},
		{Kind: domain.ModelEventToolArgsDelta, ToolIndex: 0, ToolArgs: string(args)},
		{Kind: domain.ModelEventToolCallEnd, ToolIndex: 0},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopToolUse},
	}}
}

func textStream(text string) domain.ModelStream {
	return &sliceStream{events: []domain.ModelEvent{
		{Kind: domain.ModelEventTextStart},
		{Kind: domain.ModelEventTextDelta, TextDelta: text},
		{Kind: domain.ModelEventTextEnd},
		{Kind: domain.ModelEventResponseEnd, StopReason: domain.StopEndTurn},
	}}
}
