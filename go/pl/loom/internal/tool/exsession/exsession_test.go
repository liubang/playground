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
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	script := writeScript(t, root, "tick.py", []string{
		"import time",
		"print('boot', flush=True)",
		"time.sleep(2)",
		"print('done', flush=True)",
	})
	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    python + " " + script,
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

	pollPrepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Action:      actionPoll,
		SessionID:   started.SessionID,
		YieldTimeMs: 5000,
	})
	poll := decodeSuccess(t, execTool.Execute(context.Background(), pollPrepared))
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

// TestWriteFeedsInteractiveProcess drives a stdin-reading program
// through the session.
func TestWriteFeedsInteractiveProcess(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	script := writeScript(t, root, "echo.py", []string{
		"import sys",
		"for line in sys.stdin:",
		"    print('got:' + line.strip(), flush=True)",
	})
	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    python + " " + script,
		WorkingDir: root,
	})
	started := decodeSuccess(t, execTool.Execute(context.Background(), prepared))

	writePrepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		SessionID:   started.SessionID,
		Chars:       "ping\n",
		YieldTimeMs: 5000,
	})
	if writePrepared.Risk != domain.R1 {
		t.Fatalf("write Risk = %v, want R1", writePrepared.Risk)
	}
	out := decodeSuccess(t, execTool.Execute(context.Background(), writePrepared))
	if !strings.Contains(out.Output, "got:ping") {
		t.Fatalf("output = %q, want it to contain 'got:ping'", out.Output)
	}
	if out.Status != "running" {
		t.Fatalf("status = %q, want running", out.Status)
	}
}

// TestExecSessionKill stops a long-running session through the kill
// action and reports the final status.
func TestExecSessionKill(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	script := writeScript(t, root, "sleep.py", []string{
		"import time",
		"print('ready', flush=True)",
		"time.sleep(3600)",
	})
	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    python + " " + script,
		WorkingDir: root,
	})
	started := decodeSuccess(t, execTool.Execute(context.Background(), prepared))
	if started.Status != "running" {
		t.Fatalf("status = %q, want running", started.Status)
	}

	killPrepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Action:    actionKill,
		SessionID: started.SessionID,
	})
	if killPrepared.Risk != domain.R1 {
		t.Fatalf("kill Risk = %v, want R1", killPrepared.Risk)
	}
	killed := decodeSuccess(t, execTool.Execute(context.Background(), killPrepared))
	if killed.Status != "killed" && killed.Status != "exited" {
		t.Fatalf("status after kill = %q, want killed or exited", killed.Status)
	}

	entry, ok := manager.Get(started.SessionID)
	if !ok {
		t.Fatal("session missing from manager after kill")
	}
	if entry.session.Running() {
		t.Fatal("session process still running after kill")
	}
}

// TestDriveRiskNeverBelowDefinitionDefault pins the contract the agent
// loop's prepared-call drift check enforces
// (agent.validatePreparedExecution): a per-call tier below the
// definition's static default fails closed with a security error.
// write/poll/kill pin R1 per call (the process was approved at start
// time), so the definition must stay capability-free (static R0) —
// declaring CapProcessExec would grade the definition R2 and reject
// every drive call before execution.
func TestDriveRiskNeverBelowDefinitionDefault(t *testing.T) {
	validator, _ := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	if got := execTool.Definition().Risk(); got != domain.R0 {
		t.Fatalf("definition Risk() = %v, want R0 (capability-free)", got)
	}
	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{SessionID: "sess_probe"})
	if prepared.Risk != domain.R1 {
		t.Fatalf("prepared Risk = %v, want R1", prepared.Risk)
	}
	if prepared.Risk < prepared.Definition.Risk() {
		t.Fatalf("per-call risk %v must not sit below definition default %v", prepared.Risk, prepared.Definition.Risk())
	}
}

func TestExecSessionUnknownSession(t *testing.T) {
	validator, _ := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	for _, action := range []string{actionWrite, actionPoll, actionKill} {
		prepared := prepareCall(t, execTool, "exec_session", sessionArgs{Action: action, SessionID: "sess_missing"})
		result := execTool.Execute(context.Background(), prepared)
		if result.Status != domain.ToolStatusError {
			t.Fatalf("action=%s status = %s, want error", action, result.Status)
		}
		if result.Error == nil || result.Error.Code != string(domain.ErrInvalidInput) {
			t.Fatalf("action=%s error = %+v, want invalid_input", action, result.Error)
		}
	}
}

// TestResolveAction pins the inference and per-action validation rules.
func TestResolveAction(t *testing.T) {
	infer := func(args sessionArgs) string {
		t.Helper()
		if err := resolveAction(&args); err != nil {
			t.Fatalf("resolveAction(%+v) error = %v", args, err)
		}
		return args.Action
	}
	if got := infer(sessionArgs{Command: "ls"}); got != actionStart {
		t.Fatalf("command-only inferred %q, want start", got)
	}
	if got := infer(sessionArgs{SessionID: "s", Chars: "x"}); got != actionWrite {
		t.Fatalf("session+chars inferred %q, want write", got)
	}
	if got := infer(sessionArgs{SessionID: "s"}); got != actionPoll {
		t.Fatalf("session-only inferred %q, want poll", got)
	}

	for name, args := range map[string]sessionArgs{
		"empty":              {},
		"unknown action":     {Action: "restart", SessionID: "s"},
		"start no command":   {Action: actionStart},
		"write no session":   {Action: actionWrite, Chars: "x"},
		"kill no session":    {Action: actionKill},
		"poll with chars":    {Action: actionPoll, SessionID: "s", Chars: "x"},
		"chars too long":     {Action: actionWrite, SessionID: "s", Chars: strings.Repeat("x", maxCharsBytes+1)},
		"yield out of range": {Action: actionPoll, SessionID: "s", YieldTimeMs: maxYieldMs + 1},
	} {
		if err := resolveAction(&args); err == nil {
			t.Fatalf("%s: expected validation error", name)
		}
	}
}

func TestExecSessionRiskTiers(t *testing.T) {
	validator, root := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	// Shell commands keep the base risk: the sandbox confines them and
	// the permission layer's AST danger screen handles composition.
	shellPrepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    "echo hi | cat",
		WorkingDir: root,
	})
	if shellPrepared.Risk != domain.R2 {
		t.Fatalf("shell Risk = %v, want R2", shellPrepared.Risk)
	}

	// require_escalated without justification is rejected at prepare time.
	_, err := execTool.Prepare(context.Background(), newCall(t, "exec_session", sessionArgs{
		Command:            "python3 -V",
		WorkingDir:         root,
		SandboxPermissions: "require_escalated",
	}))
	if err == nil {
		t.Fatal("Prepare() without justification succeeded, want error")
	}

	// require_escalated with justification is R3.
	escalated := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:            "python3 -V",
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
	validator, root := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    "python3 -V",
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
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	script := writeScript(t, root, "sleep.py", []string{
		"import time",
		"print('ready', flush=True)",
		"time.sleep(3600)",
	})
	prepared := prepareCall(t, execTool, "exec_session", sessionArgs{
		Command:    python + " " + script,
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

// A missing working_dir must be named in the error so the model can
// correct course without guessing.
func TestValidateCommandArgsErrorNamesWorkingDir(t *testing.T) {
	validator, _ := newValidator(t)
	_, err := validateCommandArgs(validator, &commandArgs{Command: "echo", WorkingDir: "no/such/dir"})
	if err == nil || !strings.Contains(err.Error(), `working_dir does not exist: "no/such/dir"`) {
		t.Fatalf("error = %v, want the offending path named", err)
	}
}

// A missing command is rejected with the contract stated explicitly
// (single command string plus an example), not a bare missing-field error.
func TestValidateCommandArgsMissingCommandError(t *testing.T) {
	validator, root := newValidator(t)
	_, err := validateCommandArgs(validator, &commandArgs{WorkingDir: root})
	if err == nil || !strings.Contains(err.Error(), "single 'command' string") ||
		!strings.Contains(err.Error(), `{"command":`) {
		t.Fatalf("error = %v, want the contract stated with an example", err)
	}
}

// The Codex-style max_output_tokens alias (the field models keep emitting
// from OpenAI training priors) folds into the byte budget at Prepare time;
// the signed canonical arguments carry max_output_bytes only, and the
// canonical field wins when both are present.
func TestExecSessionMaxOutputTokensAlias(t *testing.T) {
	validator, root := newValidator(t)
	manager := newManager(t, validator)
	execTool := newExecSessionTool(t, validator, manager)

	prepared := prepareCall(t, execTool, "exec_session", map[string]any{
		"command":           "echo hi",
		"working_dir":       root,
		"max_output_tokens": 1000,
	})
	var canonical sessionArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &canonical); err != nil {
		t.Fatalf("decode canonical arguments: %v", err)
	}
	if canonical.MaxOutputBytes != 4000 {
		t.Fatalf("MaxOutputBytes = %d, want 4000 (1000 tokens x 4)", canonical.MaxOutputBytes)
	}
	if strings.Contains(string(prepared.Call.Arguments), "max_output_tokens") {
		t.Fatalf("canonical arguments still carry the alias: %s", prepared.Call.Arguments)
	}

	prepared = prepareCall(t, execTool, "exec_session", map[string]any{
		"command":           "echo hi",
		"working_dir":       root,
		"max_output_bytes":  2048,
		"max_output_tokens": 1000,
	})
	if err := json.Unmarshal(prepared.Call.Arguments, &canonical); err != nil {
		t.Fatalf("decode canonical arguments: %v", err)
	}
	if canonical.MaxOutputBytes != 2048 {
		t.Fatalf("MaxOutputBytes = %d, want the canonical 2048 to win over the alias", canonical.MaxOutputBytes)
	}
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

func newManager(t *testing.T, validator *workspacepkg.PathValidator) *Manager {
	t.Helper()
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR", "HOME"},
		LookPath:     exec.LookPath,
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

func ensurePython3(t *testing.T) string {
	t.Helper()
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	return python
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
