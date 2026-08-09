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

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestRunCmdToolExternalizesLargeOutput(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	artifactStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), 4096)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	tool, err := NewRunCmdToolWithArtifacts(validator, runner, artifactStore, 2048)
	if err != nil {
		t.Fatalf("NewRunCmdToolWithArtifacts() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "import sys; sys.stdout.write('o' * 600); sys.stderr.write('e' * 400)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(2000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("status = %q, error = %+v", result.Status, result.Error)
	}
	if len(result.Content) != 3 || result.Content[1].Kind != domain.PartArtifact || result.Content[1].Artifact == nil ||
		result.Content[2].Kind != domain.PartArtifact || result.Content[2].Artifact == nil {
		t.Fatalf("content = %+v, want preview and stdout/stderr artifacts", result.Content)
	}
	// stdout/stderr artifacts are process output (text), not images: the
	// declared media type keeps renderers from treating them as pictures.
	if result.Content[1].Artifact.MediaType != "text/plain" || result.Content[2].Artifact.MediaType != "text/plain" {
		t.Fatalf("artifact media types = %q/%q, want text/plain",
			result.Content[1].Artifact.MediaType, result.Content[2].Artifact.MediaType)
	}
	var preview runCmdOutput
	if err := json.Unmarshal([]byte(result.Content[0].Text), &preview); err != nil {
		t.Fatalf("decode preview: %v", err)
	}
	if !preview.Truncated || !preview.StdoutPreviewTruncated || preview.StderrPreviewTruncated ||
		preview.StdoutBytes != 600 || preview.StderrBytes != 400 || len(result.Content[0].Text) > 2048 {
		t.Fatalf("unexpected preview metadata (encoded=%d): %+v", len(result.Content[0].Text), preview)
	}
	stdout, err := artifactStore.ReadAll(context.Background(), *result.Content[1].Artifact)
	if err != nil {
		t.Fatalf("ReadAll stdout artifact: %v", err)
	}
	stderr, err := artifactStore.ReadAll(context.Background(), *result.Content[2].Artifact)
	if err != nil {
		t.Fatalf("ReadAll stderr artifact: %v", err)
	}
	if len(stdout) != 600 || len(stderr) != 400 || strings.Trim(string(stdout), "o") != "" || strings.Trim(string(stderr), "e") != "" {
		t.Fatalf("unexpected complete artifacts: stdout=%d stderr=%d", len(stdout), len(stderr))
	}
	if result.Metadata["stdout_artifact_id"] != result.Content[1].Artifact.ID.String() ||
		result.Metadata["stderr_artifact_id"] != result.Content[2].Artifact.ID.String() {
		t.Fatalf("artifact metadata = %+v", result.Metadata)
	}
}

func TestBoundCommandOutputPreservesHeadAndTailWithinEncodedLimit(t *testing.T) {
	payload := runCmdOutput{
		Stdout: strings.Repeat("h", 200) + "STDOUT_TAIL",
		Stderr: strings.Repeat("e", 200) + "STDERR_TAIL",
	}
	if err := boundCommandOutput(&payload, 1024); err != nil {
		t.Fatalf("boundCommandOutput: %v", err)
	}
	encoded, err := json.Marshal(payload)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if len(encoded) > 1024 || !strings.Contains(payload.Stdout, "STDOUT_TAIL") || !strings.Contains(payload.Stderr, "STDERR_TAIL") {
		t.Fatalf("bounded output lost tail or exceeded limit: encoded=%d payload=%+v", len(encoded), payload)
	}
}

func TestRunCmdToolMarksArtifactTruncationAndDrainsProcessOutput(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	artifactStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), 64)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	tool, err := NewRunCmdToolWithArtifacts(validator, runner, artifactStore, 2048)
	if err != nil {
		t.Fatalf("NewRunCmdToolWithArtifacts() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program: stringPtr("python3"), Args: &[]string{"-c", "import sys; sys.stdout.write('x' * 500); sys.stderr.write('y' * 300)"},
		WorkingDir: stringPtr(root), Env: &map[string]string{}, TimeoutMs: int64Ptr(2000), MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute result = %+v", result)
	}
	var output runCmdOutput
	if err := json.Unmarshal([]byte(result.Content[0].Text), &output); err != nil {
		t.Fatalf("decode output: %v", err)
	}
	if !output.StdoutArtifactTruncated || !output.StderrArtifactTruncated || output.StdoutBytes != 500 || output.StderrBytes != 300 {
		t.Fatalf("unexpected truncation metadata: %+v", output)
	}
	stdout, err := artifactStore.ReadAll(context.Background(), *output.StdoutArtifact)
	if err != nil || len(stdout) != 64 {
		t.Fatalf("stdout artifact = %d bytes, %v", len(stdout), err)
	}
}

func TestRunCmdToolArtifactFailureDoesNotEmbedLargeOutput(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool, err := NewRunCmdToolWithArtifacts(validator, runner, failingArtifactWriter{}, 2048)
	if err != nil {
		t.Fatalf("NewRunCmdToolWithArtifacts() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{"-c", "print('x' * 1000)"},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(2000),
		MaxOutputBytes: int64Ptr(1024),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(domain.ErrUnavailable) {
		t.Fatalf("result = %+v, want unavailable error", result)
	}
	if len(result.Content) != 0 {
		t.Fatalf("failure leaked command output: %+v", result.Content)
	}
}

type failingArtifactWriter struct{}

func (failingArtifactWriter) Begin(context.Context) (domain.StagedArtifact, error) {
	return nil, errors.New("injected artifact failure")
}

func (failingArtifactWriter) Read(_ context.Context, _ domain.ArtifactRef) ([]byte, error) {
	return nil, errors.New("injected artifact failure")
}

func TestRunCmdToolSuccessAndNonZeroExit(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "SAFE_VALUE", "MY_SECRET_TOKEN", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	workingDir := mustMkdirAllPath(t, filepath.Join(root, "subdir"))
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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
	if !strings.Contains(prepared.ApprovalDesc, "cwd='subdir'") || !strings.Contains(prepared.ApprovalDesc, "timeout=2000ms") || !strings.Contains(prepared.ApprovalDesc, "network=loopback-only") {
		t.Fatalf("approval desc missing execution context: %q", prepared.ApprovalDesc)
	}
	if !strings.Contains(prepared.ApprovalDesc, "args_hash=") {
		t.Fatalf("approval desc missing args hash: %q", prepared.ApprovalDesc)
	}
	assertWorkspaceRootBindings(t, prepared, validator.Root())

	// Regression (REVIEW M9): the description was previously built before
	// ArgsHash was signed, so the displayed args_hash came from a different
	// (sha256-of-arguments) fallback and could never be correlated with the
	// ArgsHash recorded in permission events.
	if prepared.ArgsHash == "" {
		t.Fatal("prepared.ArgsHash must be signed")
	}
	if want := "args_hash=" + prepared.ArgsHash[:approvalDescHashPrefixBytes]; !strings.Contains(prepared.ApprovalDesc, want) {
		t.Fatalf("approval desc %q must carry the signed args hash prefix %q", prepared.ApprovalDesc, want)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output runCmdOutput
	decodeToolResult(t, result, &output)
	// Regression (REVIEW M10): MY_SECRET_TOKEN does not survive the sandbox
	// env filter; the drop must be surfaced in the output note so the model
	// learns the constraint instead of retrying blindly.
	if !strings.Contains(output.Note, "MY_SECRET_TOKEN") {
		t.Fatalf("output note must report the dropped env key: %q", output.Note)
	}
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

	nonZeroPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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

func TestRunCmdToolTimeoutAndCancelled(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	timeoutPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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
	var timeoutOutput runCmdOutput
	decodeToolResult(t, timeoutResult, &timeoutOutput)
	if !timeoutOutput.TimedOut || timeoutOutput.Cancelled {
		t.Fatalf("timeout output = %+v, want timed_out only", timeoutOutput)
	}

	cancelPrepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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
	var cancelOutput runCmdOutput
	decodeToolResult(t, cancelResult, &cancelOutput)
	if cancelOutput.TimedOut || !cancelOutput.Cancelled {
		t.Fatalf("cancel output = %+v, want cancelled only", cancelOutput)
	}
}

func TestRunCmdToolRejectsTamperingAndWorkspaceEscape(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	_, err := tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "run_cmd",
		Arguments: json.RawMessage(`{"program":"python3","args":[],"working_dir":".","env":{},"timeout_ms":1,"max_output_bytes":1,"extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, err = tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(filepath.Join(root, "..")),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(10),
		MaxOutputBytes: int64Ptr(1024),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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
	prepared.Call.Arguments = mustMarshalRaw(t, runCmdArgs{
		Program:        "python3",
		Args:           []string{"-c", "print('tampered')"},
		WorkingDir:     ".",
		Env:            map[string]string{},
		TimeoutMs:      1000,
		MaxOutputBytes: 1024,
	})
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)

	prepared, err = tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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

func TestRunCmdToolFailsClosedWithoutSandbox(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.UnsupportedSandbox{Reason: "no sandbox"},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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

func TestRunCmdToolValidateArguments(t *testing.T) {
	validator, root := newValidator(t)
	_, _, err := validateArgs(validator, rawRunCmdArgs{
		Program:        nil,
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{"": "bad"},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(0),
		MaxOutputBytes: int64Ptr(1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, _, err = validateArgs(validator, rawRunCmdArgs{
		Program:        stringPtr("python3"),
		Args:           &[]string{},
		WorkingDir:     stringPtr(root),
		Env:            &map[string]string{},
		TimeoutMs:      int64Ptr(1),
		MaxOutputBytes: int64Ptr(maxOutputBytes + 1),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
}

func TestRunCmdShellProgramKeepsBaseRisk(t *testing.T) {
	validator, _ := newValidator(t)
	python := ensurePython3(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "SAFE_VALUE"},
		LookPath:     fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program: stringPtr("sh"),
		Args:    &[]string{"-c", "echo hi | grep h"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	// Composition alone no longer elevates: the sandbox confines the
	// script and the permission layer's AST danger screen handles the
	// dangerous shapes.
	if prepared.Risk != domain.R2 {
		t.Fatalf("shell risk = %v, want R2", prepared.Risk)
	}
	if !strings.Contains(prepared.ApprovalDesc, "shell=parsed") {
		t.Fatalf("approval desc missing shell marker: %q", prepared.ApprovalDesc)
	}

	// The signed call survives Execute-time verification and actually runs.
	shellRunner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: exec.LookPath,
	})
	shellTool := newTool(t, validator, shellRunner)
	prepared2, err := shellTool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program: stringPtr("sh"),
		Args:    &[]string{"-c", "printf 'a\\nb\\n' | grep b"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := shellTool.Execute(context.Background(), prepared2)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output runCmdOutput
	decodeToolResult(t, result, &output)
	if output.Stdout != "b\n" {
		t.Fatalf("shell pipeline stdout = %q, want %q", output.Stdout, "b\n")
	}

	// Plain programs stay at R2.
	plain, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{Program: stringPtr("python3")}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if plain.Risk != domain.R2 {
		t.Fatalf("plain program risk = %v, want R2", plain.Risk)
	}
	if strings.Contains(plain.ApprovalDesc, "shell=parsed") {
		t.Fatalf("plain approval desc must not carry the shell marker: %q", plain.ApprovalDesc)
	}
}

func TestRunCmdEscalationValidation(t *testing.T) {
	validator, _ := newValidator(t)
	python := ensurePython3(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	t.Run("escalated requires justification", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:            stringPtr("go"),
			Args:               &[]string{"mod", "download"},
			SandboxPermissions: stringPtr("require_escalated"),
		}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("justification accepted with use_default as informational note", func(t *testing.T) {
		// A justification carries no privileges, so rejecting it on sandboxed
		// calls only taught models to retry in a loop. It is accepted and
		// surfaced in the approval description as a plain note.
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:       stringPtr("go"),
			Args:          &[]string{"build", "./..."},
			Justification: stringPtr("编译整个工作区以验证改动"),
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		if prepared.Risk != domain.R2 {
			t.Fatalf("sandboxed risk = %v, want R2", prepared.Risk)
		}
		if !strings.Contains(prepared.ApprovalDesc, "note[编译整个工作区以验证改动]") {
			t.Fatalf("approval desc missing informational note: %q", prepared.ApprovalDesc)
		}
		if strings.Contains(prepared.ApprovalDesc, "ESCALATED") {
			t.Fatalf("sandboxed call must not look escalated: %q", prepared.ApprovalDesc)
		}
	})

	t.Run("justification length bound still applies with use_default", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:       stringPtr("go"),
			Justification: stringPtr(strings.Repeat("x", maxJustificationBytes+1)),
		}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("escalated elevates risk and shows justification", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:            stringPtr("go"),
			Args:               &[]string{"mod", "download"},
			SandboxPermissions: stringPtr("require_escalated"),
			Justification:      stringPtr("Allow downloading Go modules outside the sandbox?"),
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		if prepared.Risk != domain.R3 {
			t.Fatalf("escalated risk = %v, want R3", prepared.Risk)
		}
		if !strings.Contains(prepared.ApprovalDesc, "ESCALATED(no-sandbox)") ||
			!strings.Contains(prepared.ApprovalDesc, "Allow downloading Go modules outside the sandbox?") {
			t.Fatalf("approval desc missing escalation + justification: %q", prepared.ApprovalDesc)
		}
	})
}

func TestRunCmdEscalatedBypassesDefaultSandbox(t *testing.T) {
	validator, root := newValidator(t)
	python := ensurePython3(t)
	// The default sandbox always fails; only an escalated call (DirectSandbox)
	// can get through, which proves the sandbox selection works.
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.UnsupportedSandbox{Reason: "no OS sandbox in this test"},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	blocked, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program: stringPtr("python3"),
		Args:    &[]string{"-c", "print('x')"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if result := tool.Execute(context.Background(), blocked); result.Status != domain.ToolStatusError {
		t.Fatalf("use_default status = %s, want error (unsupported sandbox)", result.Status)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
		Program:            stringPtr("python3"),
		Args:               &[]string{"-c", "import os; p=os.path.join(os.getcwd(), 'esc.txt'); open(p, 'w').write('ok'); print(p)"},
		WorkingDir:         stringPtr(root),
		SandboxPermissions: stringPtr("require_escalated"),
		Justification:      stringPtr("Allow writing esc.txt without a sandbox?"),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	// The baseline stamps the unsandboxed grant on the escalated ask it
	// approves; without it (zero grant), even a require_escalated call
	// runs in the DEFAULT sandbox (L0 must never silently become L2).
	if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusError {
		t.Fatalf("zero-grant escalated status = %s, want error (downgraded to the unavailable default sandbox)", result.Status)
	}
	prepared.Grant = domain.ExecGrant{Unsandboxed: true}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("escalated Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output runCmdOutput
	decodeToolResult(t, result, &output)
	if data, err := os.ReadFile(filepath.Join(root, "esc.txt")); err != nil || string(data) != "ok" {
		t.Fatalf("escalated command did not run outside the sandbox: data=%q err=%v", data, err)
	}
}

func TestRunCmdNeedsNetworkValidation(t *testing.T) {
	validator, _ := newValidator(t)
	python := ensurePython3(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	t.Run("needs_network stays R2 and is marked in the approval desc", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:      stringPtr("talos"),
			Args:         &[]string{"query", "submit"},
			NeedsNetwork: boolPtr(true),
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		if prepared.Risk != domain.R2 {
			t.Fatalf("needs_network risk = %v, want R2 (sandboxed path)", prepared.Risk)
		}
		if !strings.Contains(prepared.ApprovalDesc, "network=requested-in-sandbox") {
			t.Fatalf("approval desc missing the network request marker: %q", prepared.ApprovalDesc)
		}
	})

	t.Run("needs_network conflicts with require_escalated", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program:            stringPtr("go"),
			SandboxPermissions: stringPtr("require_escalated"),
			NeedsNetwork:       boolPtr(true),
			Justification:      stringPtr("x"),
		}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})
}

// TestRunCmdGrantExecutionModes proves the verdict grant drives sandbox
// selection (PERMISSION_DESIGN §3.2): an unsandboxed grant executes even
// when the default sandbox is unavailable, while a network grant widening
// an unsupported sandbox stays fail-closed.
func TestRunCmdGrantExecutionModes(t *testing.T) {
	validator, _ := newValidator(t)
	python := ensurePython3(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.UnsupportedSandbox{Reason: "no OS sandbox in this test"},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	prepare := func() domain.PreparedCall {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program: stringPtr("python3"),
			Args:    &[]string{"-c", "print('x')"},
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		return prepared
	}

	t.Run("unsandboxed grant bypasses the unavailable default sandbox", func(t *testing.T) {
		prepared := prepare()
		prepared.Grant = domain.ExecGrant{Unsandboxed: true}
		if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusSuccess {
			t.Fatalf("status = %s, want success: %+v", result.Status, result.Error)
		}
	})

	t.Run("network grant cannot widen an unsupported sandbox", func(t *testing.T) {
		prepared := prepare()
		prepared.Grant = domain.ExecGrant{NetworkFull: true}
		if result := tool.Execute(context.Background(), prepared); result.Status != domain.ToolStatusError {
			t.Fatalf("status = %s, want error (fail-closed preserved)", result.Status)
		}
	})
}

func TestSandboxGuidanceNote(t *testing.T) {
	t.Run("denial fingerprints point at escalation", func(t *testing.T) {
		cases := []struct {
			stderr string
			hit    bool
		}{
			{"dial tcp: lookup supabase.example.com: no such host", true},
			{"listen tcp 127.0.0.1:19528: bind: operation not permitted", true},
			{"curl: (6) Could not resolve host: example.com", true},
			{"cp: /protected/file: Read-only file system", true},
			{"wget: unable to resolve host address 'x'", false},
			{"compiling main.go", false},
			{"", false},
		}
		for _, tc := range cases {
			note := sandboxGuidanceNote(tc.stderr, false, false)
			if tc.hit && note == "" {
				t.Errorf("sandboxGuidanceNote(%q) = %q, want a note", tc.stderr, note)
			}
			if !tc.hit && note != "" {
				t.Errorf("sandboxGuidanceNote(%q) = %q, want empty", tc.stderr, note)
			}
			if tc.hit {
				// Regression guard: the note must route through the
				// require_escalated approval path, and must never delegate
				// the command to the user's local terminal (the old advice
				// taught the model to give up instead of escalating).
				if !strings.Contains(note, "require_escalated") {
					t.Errorf("sandboxGuidanceNote(%q) = %q, want escalation guidance", tc.stderr, note)
				}
				if strings.Contains(note, "local terminal") {
					t.Errorf("sandboxGuidanceNote(%q) = %q, must not delegate to the user's terminal", tc.stderr, note)
				}
			}
		}
	})

	t.Run("timeout suggests a sandbox network hang", func(t *testing.T) {
		note := sandboxGuidanceNote("still working...", true, false)
		if note == "" {
			t.Fatal("timeout note = empty, want guidance")
		}
		if !strings.Contains(note, "require_escalated") || !strings.Contains(note, "timeout") {
			t.Fatalf("timeout note = %q, want timeout + escalation guidance", note)
		}
	})

	t.Run("denial fingerprint wins over timeout", func(t *testing.T) {
		note := sandboxGuidanceNote("dial tcp: no such host", true, false)
		if !strings.HasPrefix(note, "outbound network and DNS are denied") {
			t.Fatalf("note = %q, want the denial variant", note)
		}
	})

	t.Run("escalated runs get no note", func(t *testing.T) {
		if note := sandboxGuidanceNote("dial tcp: no such host", true, true); note != "" {
			t.Fatalf("escalated note = %q, want empty", note)
		}
		if note := sandboxGuidanceNote("", false, true); note != "" {
			t.Fatalf("escalated note = %q, want empty", note)
		}
	})
}

func TestEscalationDowngradeNote(t *testing.T) {
	// A require_escalated call that ran sandboxed must be told so,
	// explicitly steering the model away from sandbox workarounds.
	note := escalationDowngradeNote(true, false)
	if note == "" {
		t.Fatal("downgraded escalation note = empty, want an explicit signal")
	}
	if !strings.Contains(note, "NOT honored") || !strings.Contains(note, "workaround") {
		t.Fatalf("note = %q, want downgrade signal + anti-workaround guidance", note)
	}
	// Honored escalations and non-escalated calls get no note.
	if note := escalationDowngradeNote(true, true); note != "" {
		t.Fatalf("honored escalation note = %q, want empty", note)
	}
	if note := escalationDowngradeNote(false, false); note != "" {
		t.Fatalf("plain call note = %q, want empty", note)
	}
}

func TestRiskForArgsShellForms(t *testing.T) {
	base := domain.R2
	// Shell forms keep the base risk: the sandbox is the boundary and
	// the AST danger screen catches the dangerous shapes.
	simple := runCmdArgs{Program: "sh", Args: []string{"-c", "mkdir -p .dsx_logs"}}
	if got := riskForArgs(simple, base); got != base {
		t.Errorf("riskForArgs(simple sh -c) = %v, want %v", got, base)
	}
	compound := runCmdArgs{Program: "sh", Args: []string{"-c", "mkdir -p x && echo done"}}
	if got := riskForArgs(compound, base); got != base {
		t.Errorf("riskForArgs(compound sh -c) = %v, want %v", got, base)
	}
	// Escalations always rate R3.
	escalated := runCmdArgs{Program: "make", SandboxPermissions: sandboxRequireEscalated}
	if got := riskForArgs(escalated, base); got != domain.R3 {
		t.Errorf("riskForArgs(escalated) = %v, want R3", got)
	}
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

func newTool(t *testing.T, validator *workspacepkg.PathValidator, runner *process.Runner) *RunCmdTool {
	t.Helper()
	tool, err := NewRunCmdTool(validator, runner)
	if err != nil {
		t.Fatalf("NewRunCmdTool() error = %v", err)
	}
	return tool
}

// A missing working_dir must be named in the error so the model can
// correct course without guessing.
func TestResolveWorkingDirErrorNamesPath(t *testing.T) {
	validator, _ := newValidator(t)
	_, err := resolveWorkingDir(validator, "no/such/dir")
	if err == nil || !strings.Contains(err.Error(), `working_dir does not exist: "no/such/dir"`) {
		t.Fatalf("error = %v, want the offending path named", err)
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
		Name:      "run_cmd",
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
	if !errors.As(err, &agentErr) {
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

func boolPtr(value bool) *bool    { return &value }
func int64Ptr(value int64) *int64 { return &value }

func TestRunCmdApprovalDescSurfacesExternalPathRefs(t *testing.T) {
	python := ensurePython3(t)
	validator, _ := newValidator(t)
	// macOS temp dirs are symlinked (/var → /private/var); the validator
	// stores the resolved root, so workspace-internal paths in this test
	// must be built from validator.Root() to match.
	root := validator.Root()
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)

	refsSegment := func(t *testing.T, desc string) string {
		t.Helper()
		idx := strings.Index(desc, "refs[")
		if idx < 0 {
			t.Fatalf("approval desc missing refs segment: %q", desc)
		}
		end := strings.Index(desc[idx:], "]")
		if end < 0 {
			t.Fatalf("approval desc refs segment unterminated: %q", desc)
		}
		return desc[idx : idx+end+1]
	}

	t.Run("plain argv", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program: stringPtr("ls"),
			Args:    &[]string{filepath.Join(root, "internal"), "/etc/hosts"},
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		seg := refsSegment(t, prepared.ApprovalDesc)
		if !strings.Contains(seg, "/etc/hosts") {
			t.Fatalf("refs segment missing external path: %q", seg)
		}
		if strings.Contains(seg, root) {
			t.Fatalf("refs segment leaked workspace path: %q", seg)
		}
	})

	t.Run("shell payload", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program: stringPtr("sh"),
			Args:    &[]string{"-c", "cat /etc/hosts | head -5"},
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		seg := refsSegment(t, prepared.ApprovalDesc)
		if !strings.Contains(seg, "/etc/hosts") {
			t.Fatalf("refs segment missing shell-payload path: %q", seg)
		}
	})

	t.Run("workspace only", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
			Program: stringPtr("ls"),
			Args:    &[]string{root},
		}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		if strings.Contains(prepared.ApprovalDesc, "refs[") {
			t.Fatalf("approval desc must omit refs for workspace-only paths: %q", prepared.ApprovalDesc)
		}
	})
}

func TestRunCmdToolApprovalDescShowsDangerousPayloadAndTruncation(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	payload := strings.Repeat("print('boom');", 80)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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

func TestRunCmdToolRejectsKilledBindingMismatch(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	runner := newRunner(t, validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: fixedLookPath(python),
	})
	tool := newTool(t, validator, runner)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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

func TestRunCmdToolClassifySignalStillSuccess(t *testing.T) {
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
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, rawRunCmdArgs{
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
	var output runCmdOutput
	decodeToolResult(t, result, &output)
	if output.ExitCode == 0 {
		t.Fatalf("expected non-zero exit for signalled process: %+v", output)
	}
	if output.Signal == "" {
		t.Fatal("expected signal information")
	}
}
