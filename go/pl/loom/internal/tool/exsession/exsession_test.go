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

package exsession

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// TestExecSessionLifecycle covers the full start → poll → exit arc: the
// session reports running with early output, and a later poll observes the
// exit with the remaining output.
func TestExecSessionLifecycle(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator, python)
	execTool := newExecSessionTool(t, validator, manager)
	stdinTool := newWriteStdinTool(t, manager)

	script := writeScript(t, root, "tick.py", []string{
		"import time",
		"print('boot', flush=True)",
		"time.sleep(2)",
		"print('done', flush=True)",
	})
	prepared := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:    "python3",
		Args:       []string{script},
		WorkingDir: root,
	})
	if prepared.Risk != domain.R2 {
		t.Fatalf("Risk = %v, want R2", prepared.Risk)
	}
	result := execTool.Execute(context.Background(), prepared)
	started := decodeSuccess(t, result)
	if started.SessionID == "" {
		t.Fatal("session_id is empty")
	}
	if started.Status != "running" {
		t.Fatalf("status = %q, want running (output=%q)", started.Status, started.Output)
	}
	if !strings.Contains(started.Output, "boot") {
		t.Fatalf("initial output = %q, want it to contain 'boot'", started.Output)
	}

	pollPrepared := prepareCall(t, stdinTool, "write_stdin", writeStdinArgs{
		SessionID:   started.SessionID,
		YieldTimeMs: 5000,
	})
	poll := decodeSuccess(t, stdinTool.Execute(context.Background(), pollPrepared))
	if poll.Status != "exited" {
		t.Fatalf("poll status = %q, want exited", poll.Status)
	}
	if poll.ExitCode != 0 {
		t.Fatalf("exit_code = %d, want 0", poll.ExitCode)
	}
	if !strings.Contains(poll.Output, "done") {
		t.Fatalf("poll output = %q, want it to contain 'done'", poll.Output)
	}
}

// TestWriteStdinFeedsInteractiveProcess drives a stdin-reading program
// through the session.
func TestWriteStdinFeedsInteractiveProcess(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator, python)
	execTool := newExecSessionTool(t, validator, manager)
	stdinTool := newWriteStdinTool(t, manager)

	script := writeScript(t, root, "echo.py", []string{
		"import sys",
		"for line in sys.stdin:",
		"    print('got:' + line.strip(), flush=True)",
	})
	prepared := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:    "python3",
		Args:       []string{script},
		WorkingDir: root,
	})
	started := decodeSuccess(t, execTool.Execute(context.Background(), prepared))

	writePrepared := prepareCall(t, stdinTool, "write_stdin", writeStdinArgs{
		SessionID:   started.SessionID,
		Chars:       "ping\n",
		YieldTimeMs: 5000,
	})
	if writePrepared.Risk != domain.R1 {
		t.Fatalf("write_stdin Risk = %v, want R1", writePrepared.Risk)
	}
	out := decodeSuccess(t, stdinTool.Execute(context.Background(), writePrepared))
	if !strings.Contains(out.Output, "got:ping") {
		t.Fatalf("output = %q, want it to contain 'got:ping'", out.Output)
	}
	if out.Status != "running" {
		t.Fatalf("status = %q, want running", out.Status)
	}
}

func TestWriteStdinUnknownSession(t *testing.T) {
	python := ensurePython3(t)
	validator, _ := newValidator(t)
	manager := newManager(t, validator, python)
	stdinTool := newWriteStdinTool(t, manager)

	prepared := prepareCall(t, stdinTool, "write_stdin", writeStdinArgs{SessionID: "sess_missing"})
	result := stdinTool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrInvalidInput) {
		t.Fatalf("error = %+v, want invalid_input", result.Error)
	}
}

func TestExecSessionRiskTiers(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator, python)
	execTool := newExecSessionTool(t, validator, manager)

	// Shell interpreters keep the base risk: the sandbox confines them
	// and the permission layer's AST danger screen handles composition.
	shellPrepared := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:    "sh",
		Args:       []string{"-c", "echo hi | cat"},
		WorkingDir: root,
	})
	if shellPrepared.Risk != domain.R2 {
		t.Fatalf("shell Risk = %v, want R2", shellPrepared.Risk)
	}

	// require_escalated without justification is rejected at prepare time.
	_, err := execTool.Prepare(context.Background(), newCall(t, "exec_session", commandArgs{
		Program:            "python3",
		Args:               []string{"-V"},
		WorkingDir:         root,
		SandboxPermissions: "require_escalated",
	}))
	if err == nil {
		t.Fatal("Prepare() without justification succeeded, want error")
	}

	// require_escalated with justification is R3.
	escalated := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:            "python3",
		Args:               []string{"-V"},
		WorkingDir:         root,
		SandboxPermissions: "require_escalated",
		Justification:      "need host network",
	})
	if escalated.Risk != domain.R3 {
		t.Fatalf("escalated Risk = %v, want R3", escalated.Risk)
	}
	if !strings.Contains(escalated.ApprovalDesc, "ESCALATED") {
		t.Fatalf("ApprovalDesc = %q, want ESCALATED marker", escalated.ApprovalDesc)
	}
}

func TestExecSessionRejectsTamperedArgsHash(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator, python)
	execTool := newExecSessionTool(t, validator, manager)

	prepared := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:    "python3",
		Args:       []string{"-V"},
		WorkingDir: root,
	})
	prepared.ArgsHash = strings.Repeat("0", 64)
	result := execTool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("status = %s, want error", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("error = %+v, want security", result.Error)
	}
}

// TestManagerCloseKillsSessions ensures shutdown reclaims live process
// groups instead of orphaning them.
func TestManagerCloseKillsSessions(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator, python)
	execTool := newExecSessionTool(t, validator, manager)

	script := writeScript(t, root, "sleep.py", []string{
		"import time",
		"print('ready', flush=True)",
		"time.sleep(3600)",
	})
	prepared := prepareCall(t, execTool, "exec_session", commandArgs{
		Program:    "python3",
		Args:       []string{script},
		WorkingDir: root,
	})
	started := decodeSuccess(t, execTool.Execute(context.Background(), prepared))

	entry, ok := manager.Get(started.SessionID)
	if !ok {
		t.Fatal("session missing from manager")
	}
	manager.Close()
	if entry.session.Running() {
		t.Fatal("session still running after Manager.Close")
	}
}

func decodeSuccess(t *testing.T, result domain.ToolResult) sessionOutput {
	t.Helper()
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %s, error = %+v", result.Status, result.Error)
	}
	if len(result.Content) != 1 || result.Content[0].Kind != domain.PartText {
		t.Fatalf("content = %+v, want a single text part", result.Content)
	}
	var out sessionOutput
	if err := json.Unmarshal([]byte(result.Content[0].Text), &out); err != nil {
		t.Fatalf("decode output: %v (text=%s)", err, result.Content[0].Text)
	}
	return out
}

func prepareCall[T any](t *testing.T, tool domain.Tool, name string, args T) domain.PreparedCall {
	t.Helper()
	prepared, err := tool.Prepare(context.Background(), newCall(t, name, args))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	return prepared
}

func newCall[T any](t *testing.T, name string, args T) domain.ToolCall {
	t.Helper()
	data, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: data}
}

func newValidator(t *testing.T) (*workspacepkg.PathValidator, string) {
	t.Helper()
	root := t.TempDir()
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	return validator, root
}

func newManager(t *testing.T, validator *workspacepkg.PathValidator, python string) *Manager {
	t.Helper()
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR", "HOME"},
		LookPath:     fixedLookPath(python),
	})
	if err != nil {
		t.Fatalf("NewRunner() error = %v", err)
	}
	manager, err := NewManager(runner, nil, time.Minute)
	if err != nil {
		t.Fatalf("NewManager() error = %v", err)
	}
	t.Cleanup(manager.Close)
	return manager
}

func newExecSessionTool(t *testing.T, validator *workspacepkg.PathValidator, manager *Manager) *ExecSessionTool {
	t.Helper()
	tool, err := NewExecSessionTool(validator, manager)
	if err != nil {
		t.Fatalf("NewExecSessionTool() error = %v", err)
	}
	return tool
}

func newWriteStdinTool(t *testing.T, manager *Manager) *WriteStdinTool {
	t.Helper()
	tool, err := NewWriteStdinTool(manager)
	if err != nil {
		t.Fatalf("NewWriteStdinTool() error = %v", err)
	}
	return tool
}

func ensurePython3(t *testing.T) string {
	t.Helper()
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	return python
}

func fixedLookPath(path string) func(string) (string, error) {
	return func(string) (string, error) { return path, nil }
}

func writeScript(t *testing.T, root, name string, lines []string) string {
	t.Helper()
	path := filepath.Join(root, name)
	content := strings.Join(lines, "\n") + "\n"
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatalf("write script: %v", err)
	}
	return path
}
