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

package process

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestRunnerExecutesProgramAndCapturesStreams(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "capture.py", []string{
		"import os, sys",
		"print('cwd=' + os.getcwd())",
		"print('args=' + ','.join(sys.argv[1:]))",
		"sys.stderr.write('stderr-line\\n')",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "capture",
		Args:    []string{"alpha", "beta"},
		Cwd:     root,
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if result.ExitCode != 0 {
		t.Fatalf("ExitCode = %d, want 0", result.ExitCode)
	}
	if result.Isolation != ProcessGroupIsolation.Name() {
		t.Fatalf("Isolation = %q, want %q", result.Isolation, ProcessGroupIsolation.Name())
	}
	if got, want := result.ExecutablePath, realPath(t, executable); got != want {
		t.Fatalf("ExecutablePath = %q, want %q", got, want)
	}
	if got, want := result.ExecutableHash, fileSHA256(t, executable); got != want {
		t.Fatalf("ExecutableHash = %q, want %q", got, want)
	}
	stdout := string(result.Stdout)
	if !strings.Contains(stdout, "cwd="+realPath(t, root)) || !strings.Contains(stdout, "args=alpha,beta") {
		t.Fatalf("stdout = %q, want cwd and args", stdout)
	}
	if got := string(result.Stderr); got != "stderr-line\n" {
		t.Fatalf("stderr = %q, want stderr-line", got)
	}
	if result.Truncated {
		t.Fatal("Truncated = true, want false")
	}
}

func TestRunnerTruncatesCombinedOutput(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "truncate.py", []string{
		"import sys",
		"sys.stdout.write('A' * 80)",
		"sys.stderr.write('B' * 80)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:  ExplicitTestSandbox{},
		LookPath: fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program:     "truncate",
		Cwd:         root,
		OutputLimit: 64,
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if !result.Truncated {
		t.Fatal("Truncated = false, want true")
	}
	if got := len(result.Stdout) + len(result.Stderr); got != 64 {
		t.Fatalf("captured bytes = %d, want 64", got)
	}
}

func TestRunnerStripsSecretEnvironmentVariables(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "env.py", []string{
		"import json, os",
		"keys = ['SAFE_VALUE', 'OPENAI_API_KEY', 'MY_SECRET_TOKEN', 'AWS_REGION', 'PATH']",
		"print(json.dumps({key: os.environ.get(key, '') for key in keys}, sort_keys=True))",
	})
	t.Setenv("SAFE_VALUE", "from-parent")
	t.Setenv("OPENAI_API_KEY", "blocked-openai")
	t.Setenv("MY_SECRET_TOKEN", "blocked-secret")
	t.Setenv("AWS_REGION", "blocked-aws")
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "SAFE_VALUE", "OPENAI_API_KEY", "MY_SECRET_TOKEN", "AWS_REGION"},
		LookPath:     fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "env",
		Cwd:     root,
		Env: map[string]string{
			"SAFE_VALUE":      "override-safe",
			"OPENAI_API_KEY":  "override-openai",
			"MY_SECRET_TOKEN": "override-secret",
			"AWS_REGION":      "override-aws",
		},
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	var values map[string]string
	if err := json.Unmarshal(bytesTrimSpace(result.Stdout), &values); err != nil {
		t.Fatalf("json.Unmarshal(stdout) error = %v, stdout=%q", err, string(result.Stdout))
	}
	if got := values["SAFE_VALUE"]; got != "override-safe" {
		t.Fatalf("SAFE_VALUE = %q, want override-safe", got)
	}
	for _, key := range []string{"OPENAI_API_KEY", "MY_SECRET_TOKEN", "AWS_REGION"} {
		if got := values[key]; got != "" {
			t.Fatalf("%s = %q, want empty", key, got)
		}
	}
	if got := values["PATH"]; strings.TrimSpace(got) == "" {
		t.Fatal("PATH is empty, want minimal allowlisted path")
	}
}

func TestRunnerTimeoutKillsProcessGroup(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	pidFile := filepath.Join(root, "grandchild.pid")
	executable := writePythonScript(t, python, root, "timeout.py", []string{
		"import pathlib, subprocess, sys, time",
		"pidfile = pathlib.Path(sys.argv[1])",
		"child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])",
		"pidfile.write_text(str(child.pid), encoding='utf-8')",
		"print('spawned', flush=True)",
		"time.sleep(30)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:  ExplicitTestSandbox{},
		LookPath: fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "timeout",
		Args:    []string{pidFile},
		Cwd:     root,
		Timeout: 300 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if !result.TimedOut || result.Cancelled {
		t.Fatalf("TimedOut/Cancelled = %v/%v, want true/false", result.TimedOut, result.Cancelled)
	}
	pid := readPID(t, pidFile)
	waitForProcessExit(t, pid, 3*time.Second)
}

func TestRunnerCancelKillsProcessGroup(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "cancel.py", []string{
		"import time",
		"time.sleep(30)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:  ExplicitTestSandbox{},
		LookPath: fixedLookPath(executable),
	})

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(150 * time.Millisecond)
		cancel()
	}()
	result, err := runner.Run(ctx, CommandSpec{
		Program: "cancel",
		Cwd:     root,
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if result.TimedOut || !result.Cancelled {
		t.Fatalf("TimedOut/Cancelled = %v/%v, want false/true", result.TimedOut, result.Cancelled)
	}
}

func TestRunnerReturnsAfterLeaderExitWithDetachedStdIOChild(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable() error = %v", err)
	}
	validator, root := newValidator(t)
	pidFile := filepath.Join(root, "detached.pid")
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		LookPath:     fixedLookPath(executable),
		EnvAllowlist: []string{"PATH", "PIPE_LEAK_HELPER", "PIPE_LEAK_GRANDCHILD"},
	})

	startedAt := time.Now()
	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "runner-helper",
		Args: []string{
			"-test.run=^TestRunnerDetachedStdIOHelper$",
			"--",
			pidFile,
		},
		Cwd:         root,
		Timeout:     5 * time.Second,
		OutputLimit: 16 * 1024,
		Env: map[string]string{
			"PIPE_LEAK_HELPER": "1",
		},
	})
	elapsed := time.Since(startedAt)
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if elapsed >= 1500*time.Millisecond {
		t.Fatalf("Run() took %s, want detached stdio child not to block", elapsed)
	}
	if result.TimedOut || result.Cancelled {
		t.Fatalf("unexpected timeout/cancel result: %+v", result)
	}
	if result.Truncated {
		t.Fatalf("Truncated = true, want false: stdout=%q stderr=%q", string(result.Stdout), string(result.Stderr))
	}

	pid := readPID(t, pidFile)
	if err := syscall.Kill(pid, 0); err != nil {
		t.Fatalf("detached child pid %d not running: %v", pid, err)
	}
	_ = syscall.Kill(pid, syscall.SIGKILL)
	waitForProcessExit(t, pid, 3*time.Second)
}

func TestRunnerRejectsMissingSandbox(t *testing.T) {
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "ignored",
		Cwd:     root,
	})
	if !errors.Is(err, ErrSandboxRequired) {
		t.Fatalf("Run() error = %v, want ErrSandboxRequired", err)
	}
	if result.Isolation != UnavailableIsolation.Name() {
		t.Fatalf("Isolation = %q, want %q", result.Isolation, UnavailableIsolation.Name())
	}
}

func TestRunnerRejectsCwdEscape(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "noop.py", []string{"print('ok')"})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:  ExplicitTestSandbox{},
		LookPath: fixedLookPath(executable),
	})

	_, err := runner.Run(context.Background(), CommandSpec{
		Program: "noop",
		Cwd:     filepath.Join(root, ".."),
	})
	if err == nil || !strings.Contains(err.Error(), "validate cwd") {
		t.Fatalf("Run() error = %v, want cwd validation failure", err)
	}
}

func TestRunnerRejectsShellProgram(t *testing.T) {
	validator, root := newValidator(t)
	runner := newRunner(t, validator, RunnerOptions{Sandbox: ExplicitTestSandbox{}})

	_, err := runner.Run(context.Background(), CommandSpec{
		Program: "sh",
		Cwd:     root,
	})
	if !errors.Is(err, ErrShellNotAllowed) {
		t.Fatalf("Run() error = %v, want ErrShellNotAllowed", err)
	}
}

func TestRunnerRevalidatesExecutableHashBeforeStart(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "mutate.py", []string{"print('before')"})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox: ExplicitTestSandbox{
			PrepareFunc: func(spec SandboxSpec) (SandboxLaunch, error) {
				mutated := scriptContent(python, []string{"print('after')"})
				if err := os.WriteFile(executable, []byte(mutated), 0o755); err != nil {
					return SandboxLaunch{}, err
				}
				return SandboxLaunch{
					Program: spec.ExecutablePath,
					Args:    append([]string(nil), spec.Args...),
					Env:     append([]string(nil), spec.Env...),
				}, nil
			},
		},
		LookPath: fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "mutate",
		Cwd:     root,
	})
	if !errors.Is(err, ErrExecutableHashChanged) {
		t.Fatalf("Run() error = %v, want ErrExecutableHashChanged", err)
	}
	if result.ExecutablePath != realPath(t, executable) {
		t.Fatalf("ExecutablePath = %q, want %q", result.ExecutablePath, realPath(t, executable))
	}
}

func TestRunnerDetachedStdIOHelper(t *testing.T) {
	if os.Getenv("PIPE_LEAK_HELPER") != "1" && os.Getenv("PIPE_LEAK_GRANDCHILD") != "1" {
		return
	}
	if os.Getenv("PIPE_LEAK_GRANDCHILD") == "1" {
		if len(os.Args) == 0 {
			os.Exit(2)
		}
		pidFile := os.Args[len(os.Args)-1]
		if err := os.WriteFile(pidFile, []byte(strconv.Itoa(os.Getpid())), 0o644); err != nil {
			os.Exit(3)
		}
		time.Sleep(5 * time.Second)
		os.Exit(0)
	}

	if len(os.Args) == 0 {
		os.Exit(2)
	}
	pidFile := os.Args[len(os.Args)-1]
	cmd := exec.Command(os.Args[0], "-test.run=^TestRunnerDetachedStdIOHelper$", "--", pidFile)
	cmd.Env = append(os.Environ(), "PIPE_LEAK_GRANDCHILD=1")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		os.Exit(4)
	}
	deadline := time.Now().Add(2 * time.Second)
	for {
		if _, err := os.Stat(pidFile); err == nil {
			os.Exit(0)
		}
		if time.Now().After(deadline) {
			os.Exit(5)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestUnsupportedSandboxFailsClosed(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "unsupported.py", []string{"print('ok')"})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:  UnsupportedSandbox{Reason: "no sandbox"},
		LookPath: fixedLookPath(executable),
	})

	result, err := runner.Run(context.Background(), CommandSpec{
		Program: "unsupported",
		Cwd:     root,
	})
	if !errors.Is(err, ErrSandboxUnavailable) {
		t.Fatalf("Run() error = %v, want ErrSandboxUnavailable", err)
	}
	if result.Isolation != UnsupportedIsolation.Name() {
		t.Fatalf("Isolation = %q, want %q", result.Isolation, UnsupportedIsolation.Name())
	}
}

func newValidator(t *testing.T) (*workspace.PathValidator, string) {
	t.Helper()
	root := t.TempDir()
	validator, err := workspace.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	return validator, root
}

func newRunner(t *testing.T, validator *workspace.PathValidator, opts RunnerOptions) *Runner {
	t.Helper()
	runner, err := NewRunner(validator, opts)
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

func writePythonScript(t *testing.T, python, dir, name string, body []string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(scriptContent(python, body)), 0o755); err != nil {
		t.Fatalf("os.WriteFile(%q) error = %v", path, err)
	}
	return path
}

func scriptContent(python string, body []string) string {
	return fmt.Sprintf("#!%s\n%s\n", python, strings.Join(body, "\n"))
}

func realPath(t *testing.T, path string) string {
	t.Helper()
	resolved, err := filepath.EvalSymlinks(path)
	if err != nil {
		t.Fatalf("filepath.EvalSymlinks(%q) error = %v", path, err)
	}
	return resolved
}

func fileSHA256(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile(%q) error = %v", path, err)
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func bytesTrimSpace(data []byte) []byte {
	return []byte(strings.TrimSpace(string(data)))
}

func readPID(t *testing.T, path string) int {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		data, err := os.ReadFile(path)
		if err == nil {
			pid, convErr := strconv.Atoi(strings.TrimSpace(string(data)))
			if convErr != nil {
				t.Fatalf("Atoi(%q) error = %v", string(data), convErr)
			}
			return pid
		}
		if time.Now().After(deadline) {
			t.Fatalf("os.ReadFile(%q) error = %v", path, err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func waitForProcessExit(t *testing.T, pid int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		err := syscall.Kill(pid, 0)
		if errors.Is(err, syscall.ESRCH) {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("process %d still exists after %s", pid, timeout)
		}
		time.Sleep(20 * time.Millisecond)
	}
}
