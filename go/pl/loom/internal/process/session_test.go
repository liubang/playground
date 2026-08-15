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

package process

import (
	"strings"
	"testing"
	"time"
)

func TestSessionIncrementalReadAndExit(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "session_basic.py", []string{
		"import sys, time",
		"print('first', flush=True)",
		"time.sleep(0.3)",
		"print('second', flush=True)",
		"sys.stderr.write('err-line\\n')",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	session, err := runner.StartSession(t.Context(), CommandSpec{Program: "session_basic", Cwd: root}, nil)
	if err != nil {
		t.Fatalf("StartSession() error = %v", err)
	}
	if !session.Running() {
		t.Fatal("session not running right after start")
	}

	// First drain picks up only the first line (second one is not out
	// yet). Poll instead of sleeping a fixed 150ms: python interpreter
	// startup latency varies with sandbox load, and the fixed window was
	// an intermittent failure under bazel (REVIEW: known flaky). The
	// script prints "second" only 300ms after start, so a poll that
	// returns as soon as "first" arrives still observes the incremental
	// semantics — the first logical read never contains "second".
	var firstOut string
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		firstOut += session.Read(0).Data
		if strings.Contains(firstOut, "first") {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !strings.Contains(firstOut, "first") {
		t.Fatalf("first Read() = %q, want it to contain 'first'", firstOut)
	}
	if strings.Contains(firstOut, "second") {
		t.Fatalf("first Read() = %q, must not contain 'second' yet", firstOut)
	}

	<-session.Done()
	out := session.Read(0)
	if out.Running {
		t.Fatal("Read() reports running after Done")
	}
	if !strings.Contains(out.Data, "second") || !strings.Contains(out.Data, "err-line") {
		t.Fatalf("final Read() = %q, want remaining stdout and stderr", out.Data)
	}
	if out.ExitCode != 0 {
		t.Fatalf("ExitCode = %d, want 0", out.ExitCode)
	}
	if out.DroppedBytes != 0 {
		t.Fatalf("DroppedBytes = %d, want 0", out.DroppedBytes)
	}
	if out.StdoutBytes == 0 || out.StderrBytes == 0 {
		t.Fatalf("stream byte counts = %d/%d, want both > 0", out.StdoutBytes, out.StderrBytes)
	}
}

func TestSessionWriteStdin(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "session_echo.py", []string{
		"import sys",
		"for line in sys.stdin:",
		"    print('echo:' + line.strip(), flush=True)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	session, err := runner.StartSession(t.Context(), CommandSpec{Program: "session_echo", Cwd: root}, nil)
	if err != nil {
		t.Fatalf("StartSession() error = %v", err)
	}
	defer session.Kill()

	if err := session.Write("hello\n"); err != nil {
		t.Fatalf("Write() error = %v", err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if out := session.Read(0); strings.Contains(out.Data, "echo:hello") {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("did not receive echoed stdin within 5s")
}

func TestSessionKillReclaimsProcessGroup(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "session_sleep.py", []string{
		"import time",
		"print('ready', flush=True)",
		"time.sleep(3600)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	session, err := runner.StartSession(t.Context(), CommandSpec{Program: "session_sleep", Cwd: root}, nil)
	if err != nil {
		t.Fatalf("StartSession() error = %v", err)
	}
	session.Kill()
	if session.Running() {
		t.Fatal("session still running after Kill")
	}
	out := session.Read(0)
	if !out.Killed {
		t.Fatal("Killed = false, want true")
	}
	if err := session.Write("x\n"); err == nil {
		t.Fatal("Write() after Kill succeeded, want error")
	}
}

func TestSessionReadMaxBytesKeepsTail(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "session_volume.py", []string{
		"import sys",
		"sys.stdout.write('A' * 100 + 'B' * 100)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	session, err := runner.StartSession(t.Context(), CommandSpec{Program: "session_volume", Cwd: root}, nil)
	if err != nil {
		t.Fatalf("StartSession() error = %v", err)
	}
	<-session.Done()
	out := session.Read(50)
	if len(out.Data) != 50 {
		t.Fatalf("len(Data) = %d, want 50", len(out.Data))
	}
	if out.Data != strings.Repeat("B", 50) {
		t.Fatalf("Data = %q, want the B tail", out.Data)
	}
	if out.DroppedBytes != 150 {
		t.Fatalf("DroppedBytes = %d, want 150", out.DroppedBytes)
	}
}

func TestSessionBufferCapDropsOldest(t *testing.T) {
	python := ensurePython3(t)
	validator, root := newValidator(t)
	executable := writePythonScript(t, python, root, "session_flood.py", []string{
		"import sys",
		"sys.stdout.write('X' * 300)",
		"sys.stdout.flush()",
		"import time",
		"time.sleep(0.2)",
	})
	runner := newRunner(t, validator, RunnerOptions{
		Sandbox:      ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "LANG", "TMPDIR"},
		LookPath:     fixedLookPath(executable),
	})

	session, err := runner.StartSession(t.Context(), CommandSpec{Program: "session_flood", Cwd: root}, nil)
	if err != nil {
		t.Fatalf("StartSession() error = %v", err)
	}
	session.mu.Lock()
	session.bufCap = 64
	session.mu.Unlock()
	<-session.Done()
	out := session.Read(0)
	if len(out.Data) != 64 {
		t.Fatalf("len(Data) = %d, want 64 (buffer cap)", len(out.Data))
	}
	if out.DroppedBytes != 300-64 {
		t.Fatalf("DroppedBytes = %d, want %d", out.DroppedBytes, 300-64)
	}
}
