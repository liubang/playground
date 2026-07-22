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
// Created: 2026/07/22 21:10

package command

import (
	"context"
	"encoding/json"
	"errors"
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

func TestRunCommandToolSuccessAndNonZeroExit(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "SAFE_VALUE", "MY_SECRET_TOKEN", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	workingDir := mustMkdirAllPath(t, filepath.Join(root, "subdir"))
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import json, os, sys; print(json.dumps({'argv': sys.argv[1:], 'cwd': os.getcwd(), 'safe': os.environ.get('SAFE_VALUE', ''), 'secret': os.environ.get('MY_SECRET_TOKEN', '')}, sort_keys=True)); sys.stderr.buffer.write(b'bad\\xfferr')", "alpha", "beta"},
		WorkingDir:     stringPtr(workingDir),
		Env:            &map[string]string{"SAFE_VALUE": "kept", "MY_SECRET_TOKEN": "drop-me"},
		TimeoutMs:      int64Ptr(2000),
		MaxOutputBytes: int64Ptr(4096),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if got, want := prepared.Definition.Capabilities, []domain.Capability{domain.CapProcessExec}; len(got) != len(want) || got[0] != want[0] {
		t.Fatalf("capabilities = %v, want %v", got, want)
	}
	if prepared.Risk != domain.R2 {
		t.Fatalf("prepared.Risk = %v, want R2", prepared.Risk)
	}
	if !strings.Contains(prepared.ApprovalDesc, "'python3' '-c'") {
		t.Fatalf("approval desc missing quoted command: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "env[MY_SECRET_TOKEN, SAFE_VALUE]") {
		t.Fatalf("approval desc missing env keys: %q", prepared.ApprovalDesc)
	}
	if strings.Contains(prepared.ApprovalDesc, "kept") || strings.Contains(prepared.ApprovalDesc, "drop-me") {
		t.Fatalf("approval desc leaked env value: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "cwd='subdir'") || !strings.Contains(prepared.ApprovalDesc, "timeout=2000ms") || !strings.Contains(prepared.ApprovalDesc, "network=deny") {
		t.Fatalf("approval desc missing execution context: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "args_hash=") {
		t.Fatalf("approval desc missing args hash: %q", prepared.ApprovalDesc)
	}
	assertWorkspaceRootBindings(t, prepared, validator.Root())

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output runCommandOutput
	decodeToolResult(t, result, &output)
	if output.ExitCode != 0 || output.Signal != "" {
		t.Fatalf("unexpected process exit: %+v", output)
	}
	if output.Isolation != process.ProcessGroupIsolation.Name() {
		t.Fatalf("output.Isolation = %q, want %q", output.Isolation, process.ProcessGroupIsolation.Name())
	}
	if output.ExecutablePath != realPath(t, python) {
		t.Fatalf("ExecutablePath = %q, want %q", output.ExecutablePath, realPath(t, python))
	}
	if output.Hash == "" {
		t.Fatal("expected executable hash")
	}
	if output.Stderr != "bad?err" {
		t.Fatalf("stderr = %q, want bad?err", output.Stderr)
	}
	var stdout map[string]any
	if err := json.Unmarshal([]byte(output.Stdout), &stdout); err != nil {
		t.Fatalf("json.Unmarshal(stdout) error = %v, stdout=%q", err, output.Stdout)
	}
	if got := stdout["cwd"]; got != realPath(t, workingDir) {
		t.Fatalf("stdout cwd = %v, want %q", got, realPath(t, workingDir))
	}
	if got := stdout["safe"]; got != "kept" {
		t.Fatalf("stdout safe = %v, want kept", got)
	}
	if got := stdout["secret"]; got != "" {
		t.Fatalf("stdout secret = %v, want empty", got)
	}
	argv, ok := stdout["argv"].([]any)
	if !ok || len(argv) != 2 || argv[0] != "alpha" || argv[1] != "beta" {
		t.Fatalf("stdout argv = %#v, want [alpha beta]", stdout["argv"])
	}

	nonZeroPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import sys; sys.stderr.write('boom\\n'); sys.exit(7)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(2000),
		MaxOutputBytes: int64Ptr(4096),
	}))
	if err != nil {
		t.Fatalf("Prepare(non-zero) error = %v", err)
	}
	nonZero := tool.Execute(context.Background(), nonZeroPrepared)
	if nonZero.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute(non-zero) status = %s, want success: %+v", nonZero.Status, nonZero.Error)
	}
	decodeToolResult(t, nonZero, &output)
	if output.ExitCode != 7 || output.Stderr != "boom\n" {
		t.Fatalf("unexpected non-zero output: %+v", output)
	}
}

func TestRunCommandToolTimeoutAndCancelled(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	timeoutPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import time; print('start', flush=True); time.sleep(30)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(100),
		MaxOutputBytes: int64Ptr(4096),
	}))
	if err != nil {
		t.Fatalf("Prepare(timeout) error = %v", err)
	}
	timeoutResult := tool.Execute(context.Background(), timeoutPrepared)
	if timeoutResult.Status != domain.ToolStatusTimeout {
		t.Fatalf("timeout status = %s, want timeout: %+v", timeoutResult.Status, timeoutResult.Error)
	}
	var timeoutOutput runCommandOutput
	decodeToolResult(t, timeoutResult, &timeoutOutput)
	if !timeoutOutput.TimedOut || timeoutOutput.Cancelled {
		t.Fatalf("timeout output = %+v, want timed_out only", timeoutOutput)
	}

	cancelPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import time; time.sleep(30)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(2000),
		MaxOutputBytes: int64Ptr(4096),
	}))
	if err != nil {
		t.Fatalf("Prepare(cancel) error = %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(100 * time.Millisecond)
		cancel()
	}()
	cancelResult := tool.Execute(ctx, cancelPrepared)
	if cancelResult.Status != domain.ToolStatusCancelled {
		t.Fatalf("cancel status = %s, want cancelled: %+v", cancelResult.Status, cancelResult.Error)
	}
	var cancelOutput runCommandOutput
	decodeToolResult(t, cancelResult, &cancelOutput)
	if cancelOutput.TimedOut || !cancelOutput.Cancelled {
		t.Fatalf("cancel output = %+v, want cancelled only", cancelOutput)
	}
}

func TestRunCommandToolRejectsTamperingAndWorkspaceEscape(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	_, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "run_command",
		Arguments: json.RawMessage(`{"program":"python3","args":[],"working_dir":".","env":{},"timeout_ms":1,"max_output_bytes":1,"extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, err = tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(filepath.Join(root, "..")),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(10),
		MaxOutputBytes: int64Ptr(1024),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "print('ok')"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare(valid) error = %v", err)
	}
	prepared.Call.Arguments = mustMarshalRaw(t, runCommandArgs{
		Program:        "python3",
		Args:           []string{"-c", "print('tampered')"},
		WorkingDir:     ".",
		Env:            map[string]string{},
		TimeoutMs:      1000,
		MaxOutputBytes: 1024,
	})
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)

	prepared, err = tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "print('ok')"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare(valid) error = %v", err)
	}
	prepared.WritePaths = []string{filepath.Join(root, "other")}
	prepared.ArgsHash = tool.signPrepared(prepared)
	invalidBindings := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, invalidBindings, domain.ToolStatusError, domain.ErrSecurity)
}

func TestRunCommandToolFailsClosedWithoutSandbox(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.UnsupportedSandbox{Reason: "no sandbox"},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "print('ok')"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrUnavailable)
	if result.Error == nil || !strings.Contains(result.Error.Message, "sandbox") {
		t.Fatalf("expected sandbox error message, got %+v", result.Error)
	}
}

func TestRunCommandToolValidateArguments(t *testing.T) {
	validator, root := newValidator(t)
	_, _, err := validateArgs(validator, rawRunCommandArgs{
		Program:        nil,
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{"": "bad"},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(0),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(maxOutputBytes + 1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
}

func TestClassifyRunError(t *testing.T) {
	cases := []struct {
		name string
		err  error
		code domain.ErrorCode
	}{
		{name: "sandbox required", err: process.ErrSandboxRequired, code: domain.ErrUnavailable},
		{name: "sandbox unavailable", err: process.ErrSandboxUnavailable, code: domain.ErrUnavailable},
		{name: "hash changed", err: process.ErrExecutableHashChanged, code: domain.ErrSecurity},
		{name: "shell disallowed", err: process.ErrShellNotAllowed, code: domain.ErrInvalidInput},
		{name: "cancelled", err: context.Canceled, code: domain.ErrCancelled},
		{name: "timeout", err: context.DeadlineExceeded, code: domain.ErrTimeout},
		{name: "not found", err: exec.ErrNotFound, code: domain.ErrInvalidInput},
		{name: "generic", err: errors.New("boom"), code: domain.ErrUnavailable},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := classifyRunError(tc.err)
			assertAgentErrorCode(t, err, tc.code)
		})
	}
}

func newTool(t *testing.T, validator *workspacepkg.PathValidator, runner *process.Runner) *RunCommandTool {
	t.Helper()
	tool, err := NewRunCommandTool(validator, runner)
	if err != nil {
		t.Fatalf("NewRunCommandTool() error = %v", err)
	}
	return tool
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

func newRunner(t *testing.T, validator *workspacepkg.PathValidator, opts process.RunnerOptions) *process.Runner {
	t.Helper()
	runner, err := process.NewRunner(validator, opts)
	if err != nil {
		t.Fatalf("NewRunner() error = %v", err)
	}
	return runner
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

func realPath(t *testing.T, path string) string {
	t.Helper()
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		t.Fatalf("filepath.EvalSymlinks(%q) error = %v", path, err)
	}
	return resolved
}

func newToolCall[T any](t *testing.T, args T) domain.ToolCall {
	t.Helper()
	return domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "run_command",
		Arguments: mustMarshalRaw(t, args),
	}
}

func mustMarshalRaw[T any](t *testing.T, value T) json.RawMessage {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return data
}

func decodeToolResult(t *testing.T, result domain.ToolResult, out any) {
	t.Helper()
	if len(result.Content) != 1 {
		t.Fatalf("len(result.Content) = %d, want 1", len(result.Content))
	}
	if result.Content[0].Kind != domain.PartText {
		t.Fatalf("result.Content[0].Kind = %s, want text", result.Content[0].Kind)
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), out); err != nil {
		t.Fatalf("json.Unmarshal(tool result) error = %v, payload=%s", err, result.Content[0].Text)
	}
}

func assertToolResultError(t *testing.T, result domain.ToolResult, wantStatus domain.ToolStatus, wantCode domain.ErrorCode) {
	t.Helper()
	if result.Status != wantStatus {
		t.Fatalf("result.Status = %s, want %s", result.Status, wantStatus)
	}
	if result.Error == nil {
		t.Fatal("expected structured tool error")
	}
	if result.Error.Code != string(wantCode) {
		t.Fatalf("result.Error.Code = %q, want %q", result.Error.Code, wantCode)
	}
}

func assertAgentErrorCode(t *testing.T, err error, want domain.ErrorCode) {
	t.Helper()
	if err == nil {
		t.Fatal("expected error")
	}
	var agentErr *domain.AgentError
	if !domain.As(err, &agentErr) {
		t.Fatalf("expected AgentError, got %T: %v", err, err)
	}
	if agentErr.Code != want {
		t.Fatalf("agentErr.Code = %s, want %s", agentErr.Code, want)
	}
}

func assertWorkspaceRootBindings(t *testing.T, prepared domain.PreparedCall, root string) {
	t.Helper()
	if len(prepared.ReadPaths) != 1 || prepared.ReadPaths[0] != root {
		t.Fatalf("prepared.ReadPaths = %v, want [%q]", prepared.ReadPaths, root)
	}
	if len(prepared.WritePaths) != 1 || prepared.WritePaths[0] != root {
		t.Fatalf("prepared.WritePaths = %v, want [%q]", prepared.WritePaths, root)
	}
}

func mustMkdirAllPath(t *testing.T, path string) string {
	t.Helper()
	if err := os.MkdirAll(path, 0o755); err != nil {
		t.Fatalf("os.MkdirAll(%q) error = %v", path, err)
	}
	return path
}

func stringPtr(value string) *string { return &value }
func int64Ptr(value int64) *int64    { return &value }

func TestRunCommandToolApprovalDescShowsDangerousPayloadAndTruncation(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	payload := strings.Repeat("print('boom');", 80)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", payload},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{"OPENAI_API_KEY": "super-secret", "VISIBLE_KEY": "visible-secret"},
		TimeoutMs:      int64Ptr(1234),
		MaxOutputBytes: int64Ptr(4096),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if !strings.Contains(prepared.ApprovalDesc, "'python3' '-c' 'print('") {
		t.Fatalf("approval desc missing dangerous payload prefix: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "boom") {
		t.Fatalf("approval desc missing dangerous payload body: %q", prepared.ApprovalDesc)
	}
	if strings.Contains(prepared.ApprovalDesc, "super-secret") || strings.Contains(prepared.ApprovalDesc, "visible-secret") {
		t.Fatalf("approval desc leaked env value: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "...[truncated]") {
		t.Fatalf("approval desc missing truncation marker: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "args_hash=") {
		t.Fatalf("approval desc missing args hash: %q", prepared.ApprovalDesc)
	}
}

func TestRunCommandToolRejectsKilledBindingMismatch(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "print('ok')"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	prepared.Call.Name = "other"
	prepared.ArgsHash = tool.signPrepared(prepared)
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrSecurity)
}

func TestDurationMilliseconds(t *testing.T) {
	if got := durationMilliseconds(-time.Second); got != 0 {
		t.Fatalf("durationMilliseconds(-1s) = %d, want 0", got)
	}
	if got := durationMilliseconds(1500 * time.Millisecond); got != 1500 {
		t.Fatalf("durationMilliseconds(1500ms) = %d, want 1500", got)
	}
}

func TestHasOnlyWorkspaceRoot(t *testing.T) {
	root := t.TempDir()
	if !hasOnlyWorkspaceRoot([]string{root}, root) {
		t.Fatal("expected exact root binding to match")
	}
	if hasOnlyWorkspaceRoot([]string{filepath.Join(root, "sub")}, root) {
		t.Fatal("unexpected match for subpath")
	}
}

func TestSanitizeUTF8(t *testing.T) {
	if got := sanitizeUTF8([]byte{'a', 0xff, 'b'}); got != "a?b" {
		t.Fatalf("sanitizeUTF8() = %q, want a?b", got)
	}
}

func TestRunCommandToolClassifySignalStillSuccess(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox: process.ExplicitTestSandbox{
			PrepareFunc: func(spec process.SandboxSpec) (process.SandboxLaunch, error) {
				return process.SandboxLaunch{Program: spec.ExecutablePath, Args: append([]string(nil), spec.Args...), Env: append([]string(nil), spec.Env...)}, nil
			},
		},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCommandArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import os, signal; os.kill(os.getpid(), signal.SIGTERM)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("result.Status = %s, want success", result.Status)
	}
	var output runCommandOutput
	decodeToolResult(t, result, &output)
	if output.ExitCode == 0 {
		t.Fatalf("expected non-zero exit for signalled process: %+v", output)
	}
	if output.Signal == "" {
		t.Fatal("expected signal information")
	}
}
