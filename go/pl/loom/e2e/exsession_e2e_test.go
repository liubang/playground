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
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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
		"program":       python,
		"args":          []string{script},
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
		Policy:   permission.DefaultPolicy().Decider(permission.ModeOnRequest),
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
