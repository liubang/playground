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
// Created: 2026/07/24

package lint

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

// --- helpers ---

type fakeRunner struct {
	result process.Result
	err    error
	calls  []process.CommandSpec
}

func (f *fakeRunner) Run(ctx context.Context, spec process.CommandSpec) (process.Result, error) {
	f.calls = append(f.calls, spec)
	return f.result, f.err
}

func newValidator(t *testing.T) (*workspacepkg.PathValidator, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "workspace")
	mustMkdirAll(t, root)
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	// The validator canonicalizes symlinks (e.g. /var → /private/var on
	// macOS); always compare against its root, not the original input.
	return validator, validator.Root()
}

func newToolCall[T any](t *testing.T, name string, args T) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw}
}

func decodeToolResult(t *testing.T, result domain.ToolResult, out any) {
	t.Helper()
	if len(result.Content) != 1 {
		t.Fatalf("len(result.Content) = %d, want 1", len(result.Content))
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), out); err != nil {
		t.Fatalf("json.Unmarshal(tool result) error = %v, payload=%s", err, result.Content[0].Text)
	}
}

func mustWriteFile(t *testing.T, path string, data []byte) {
	t.Helper()
	mustMkdirAll(t, filepath.Dir(path))
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatalf("os.WriteFile(%q) error = %v", path, err)
	}
}

func mustMkdirAll(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(path, 0755); err != nil {
		t.Fatalf("os.MkdirAll(%q) error = %v", path, err)
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

// stubLookPath makes the named binaries visible (at /fake/<name>) and all
// others missing.
func stubLookPath(t *testing.T, available ...string) {
	t.Helper()
	original := lookPath
	set := map[string]struct{}{}
	for _, name := range available {
		set[name] = struct{}{}
	}
	lookPath = func(name string) (string, error) {
		if _, ok := set[name]; ok {
			return filepath.Join("/fake", name), nil
		}
		return "", exec.ErrNotFound
	}
	t.Cleanup(func() { lookPath = original })
}

func newLintTool(t *testing.T, validator *workspacepkg.PathValidator, runner cmdRunner) *LintTool {
	t.Helper()
	tool, err := NewLintTool(validator, runner)
	if err != nil {
		t.Fatalf("NewLintTool() error = %v", err)
	}
	return tool
}

// --- arg validation ---

func TestLintValidateArgs(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "main.go"), []byte("package main\n"))
	tool := newLintTool(t, validator, &fakeRunner{})

	t.Run("defaults applied", func(t *testing.T) {
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		var args lintArgs
		if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
			t.Fatalf("unmarshal canonical args: %v", err)
		}
		if args.Path != "." || args.MaxDiagnostics != defaultMaxDiagnostics {
			t.Fatalf("defaults not applied: %+v", args)
		}
		if prepared.Risk != domain.R2 {
			t.Fatalf("risk = %v, want R2", prepared.Risk)
		}
	})

	t.Run("invalid linter", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Linter: "pylint"}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("invalid severity", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Severity: "info"}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("max_diagnostics out of range", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{MaxDiagnostics: 9999}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("missing path", func(t *testing.T) {
		_, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "nope/never.go"}))
		assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	})

	t.Run("sensitive component rejected", func(t *testing.T) {
		mustWriteFile(t, filepath.Join(root, ".git", "config"), []byte("x"))
		_, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: ".git"}))
		assertAgentErrorCode(t, err, domain.ErrSecurity)
	})
}

// --- detection ---

func TestDetectGoModule(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "go.mod"), []byte("module example.com/x\n\ngo 1.25\n"))
	mustWriteFile(t, filepath.Join(root, "pkg", "sub", "x.go"), []byte("package sub\n"))
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, "pkg/sub/x.go")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}

	t.Run("golangci-lint preferred with package scope", func(t *testing.T) {
		stubLookPath(t, "golangci-lint", "go")
		plan, err := tool.detect(target, lintArgs{Path: "pkg/sub/x.go"})
		if err != nil {
			t.Fatalf("detect() error = %v", err)
		}
		if plan.Linter != linterGolangCI {
			t.Fatalf("linter = %s, want golangci-lint", plan.Linter)
		}
		if plan.ProjectRoot != root {
			t.Fatalf("project root = %q, want %q", plan.ProjectRoot, root)
		}
		last := plan.Argv[len(plan.Argv)-1]
		if last != "./pkg/sub/..." {
			t.Fatalf("scope = %q, want ./pkg/sub/...", last)
		}
		if plan.Env["GOPROXY"] != "off" || plan.Env["GOCACHE"] == "" {
			t.Fatalf("go env overrides missing: %+v", plan.Env)
		}
	})

	t.Run("go vet fallback", func(t *testing.T) {
		stubLookPath(t, "go")
		plan, err := tool.detect(target, lintArgs{Path: "pkg/sub/x.go"})
		if err != nil {
			t.Fatalf("detect() error = %v", err)
		}
		if plan.Linter != linterGoVet || plan.Parse != parseGoVet {
			t.Fatalf("unexpected plan: %+v", plan)
		}
	})

	t.Run("no go toolchain", func(t *testing.T) {
		stubLookPath(t)
		_, err := tool.detect(target, lintArgs{Path: "pkg/sub/x.go"})
		assertAgentErrorCode(t, err, domain.ErrUnavailable)
	})

	t.Run("forced go-vet beats golangci-lint", func(t *testing.T) {
		stubLookPath(t, "golangci-lint", "go")
		plan, err := tool.detect(target, lintArgs{Path: "pkg/sub/x.go", Linter: linterGoVet})
		if err != nil {
			t.Fatalf("detect() error = %v", err)
		}
		if plan.Linter != linterGoVet {
			t.Fatalf("linter = %s, want go-vet", plan.Linter)
		}
	})

	t.Run("forced golangci-lint unavailable", func(t *testing.T) {
		stubLookPath(t, "go")
		_, err := tool.detect(target, lintArgs{Path: "pkg/sub/x.go", Linter: linterGolangCI})
		assertAgentErrorCode(t, err, domain.ErrUnavailable)
	})
}

func TestDetectESLint(t *testing.T) {
	validator, root := newValidator(t)
	web := filepath.Join(root, "web")
	mustWriteFile(t, filepath.Join(web, "package.json"), []byte("{}\n"))
	mustWriteFile(t, filepath.Join(web, "eslint.config.js"), []byte("export default [];\n"))
	mustWriteFile(t, filepath.Join(web, "node_modules", ".bin", "eslint"), []byte("#!/bin/sh\n"))
	mustWriteFile(t, filepath.Join(web, "src", "a.ts"), []byte("export const a = 1;\n"))
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, "web/src")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}
	plan, err := tool.detect(target, lintArgs{Path: "web/src"})
	if err != nil {
		t.Fatalf("detect() error = %v", err)
	}
	if plan.Linter != linterESLint || plan.ProjectRoot != web {
		t.Fatalf("unexpected plan: %+v", plan)
	}
	if plan.Argv[len(plan.Argv)-1] != filepath.Join(web, "src") {
		t.Fatalf("eslint target arg = %q", plan.Argv[len(plan.Argv)-1])
	}
}

func TestDetectESLintMissingBinary(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "package.json"), []byte("{}\n"))
	mustWriteFile(t, filepath.Join(root, ".eslintrc.json"), []byte("{}\n"))
	mustWriteFile(t, filepath.Join(root, "a.ts"), []byte("export const a = 1;\n"))
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, ".")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}
	_, err = tool.detect(target, lintArgs{Path: "."})
	assertAgentErrorCode(t, err, domain.ErrUnavailable)
}

func TestDetectRuff(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "pyproject.toml"), []byte("[project]\nname = \"x\"\n"))
	mustWriteFile(t, filepath.Join(root, "x.py"), []byte("import os\n"))
	stubLookPath(t, "ruff")
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, ".")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}
	plan, err := tool.detect(target, lintArgs{Path: "."})
	if err != nil {
		t.Fatalf("detect() error = %v", err)
	}
	if plan.Linter != linterRuff || plan.ProjectRoot != root {
		t.Fatalf("unexpected plan: %+v", plan)
	}
}

func TestDetectClangTidy(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "compile_commands.json"), []byte("[]\n"))
	mustWriteFile(t, filepath.Join(root, "main.cpp"), []byte("int main() { return 0; }\n"))
	stubLookPath(t, "clang-tidy")
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, "main.cpp")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}
	plan, err := tool.detect(target, lintArgs{Path: "main.cpp"})
	if err != nil {
		t.Fatalf("detect() error = %v", err)
	}
	if plan.Linter != linterClangTidy {
		t.Fatalf("linter = %s, want clang-tidy", plan.Linter)
	}
}

func TestDetectNothingFound(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "README.txt"), []byte("hi\n"))
	tool := newLintTool(t, validator, &fakeRunner{})

	target, err := resolveExistingPath(validator, ".")
	if err != nil {
		t.Fatalf("resolveExistingPath() error = %v", err)
	}
	_, err = tool.detect(target, lintArgs{Path: "."})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	if !strings.Contains(err.Error(), "go.mod") {
		t.Fatalf("error should list probed markers: %v", err)
	}
}

// --- parsers ---

func TestParseGolangCIOutput(t *testing.T) {
	raw := `{"Issues":[
		{"FromLinter":"govet","Text":"printf: %d expects int","Pos":{"Filename":"main.go","Offset":10,"Line":3,"Column":2}},
		{"FromLinter":"staticcheck","Text":"should omit","Severity":"error","Pos":{"Filename":"x/y.go","Offset":1,"Line":10,"Column":5}}
	]}`
	diags, err := parseGolangCIOutput([]byte(raw))
	if err != nil {
		t.Fatalf("parseGolangCIOutput() error = %v", err)
	}
	if len(diags) != 2 {
		t.Fatalf("len = %d, want 2", len(diags))
	}
	if diags[0].Path != "main.go" || diags[0].Line != 3 || diags[0].Severity != "warning" || diags[0].Source != "govet" {
		t.Fatalf("unexpected diagnostic: %+v", diags[0])
	}
	if diags[1].Severity != "error" {
		t.Fatalf("severity = %s, want error", diags[1].Severity)
	}
}

func TestParseGoVetOutput(t *testing.T) {
	raw := "# example.com/x\npkg/a.go:12:5: printf: Sprintf format %d reads arg 1, but call has 0 args\npkg/b.go:3:1: unreachable code\n"
	diags := parseGoVetOutput([]byte(raw))
	if len(diags) != 2 {
		t.Fatalf("len = %d, want 2: %+v", len(diags), diags)
	}
	if diags[0].Path != "pkg/a.go" || diags[0].Line != 12 || diags[0].Column != 5 {
		t.Fatalf("unexpected diagnostic: %+v", diags[0])
	}
	if diags[1].Message != "unreachable code" || diags[1].Source != "go vet" {
		t.Fatalf("unexpected diagnostic: %+v", diags[1])
	}
}

func TestParseESLintOutput(t *testing.T) {
	raw := `[{"filePath":"/ws/src/a.ts","messages":[
		{"ruleId":"no-unused-vars","severity":2,"message":"'x' is unused.","line":1,"column":7},
		{"ruleId":"semi","severity":1,"message":"Missing semicolon.","line":2,"column":10}
	]}]`
	diags, err := parseESLintOutput([]byte(raw))
	if err != nil {
		t.Fatalf("parseESLintOutput() error = %v", err)
	}
	if len(diags) != 2 || diags[0].Severity != "error" || diags[1].Severity != "warning" {
		t.Fatalf("unexpected diagnostics: %+v", diags)
	}
	if diags[0].Code != "no-unused-vars" {
		t.Fatalf("code = %q", diags[0].Code)
	}
}

func TestParseRuffOutput(t *testing.T) {
	raw := `[{"code":"F401","message":"os imported but unused","filename":"/ws/x.py","location":{"row":1,"column":8}}]`
	diags, err := parseRuffOutput([]byte(raw))
	if err != nil {
		t.Fatalf("parseRuffOutput() error = %v", err)
	}
	if len(diags) != 1 || diags[0].Code != "F401" || diags[0].Line != 1 {
		t.Fatalf("unexpected diagnostics: %+v", diags)
	}
}

func TestParseClangTidyOutput(t *testing.T) {
	raw := `/ws/main.cpp:3:10: warning: statement should have braces [readability-braces-around-statements]
/ws/main.cpp:3:10: note: expanded from macro 'X'
/ws/main.cpp:5:1: error: expected ';' after expression [clang-diagnostic-error]
`
	diags := parseClangTidyOutput([]byte(raw))
	if len(diags) != 2 {
		t.Fatalf("len = %d, want 2 (notes skipped): %+v", len(diags), diags)
	}
	if diags[0].Code != "readability-braces-around-statements" || diags[0].Severity != "warning" {
		t.Fatalf("unexpected diagnostic: %+v", diags[0])
	}
	if diags[1].Severity != "error" {
		t.Fatalf("unexpected diagnostic: %+v", diags[1])
	}
}

func TestNormalizeDiagnostics(t *testing.T) {
	wsRoot := t.TempDir()
	plan := enginePlan{ProjectRoot: wsRoot}
	diags := []diagnostic{
		{Path: "b.go", Line: 9, Severity: "warning", Message: "w"},
		{Path: "a.go", Line: 3, Severity: "error", Message: "e"},
		{Path: filepath.Join(wsRoot, "sub", "c.go"), Line: 1, Severity: "info", Message: "i"},
	}
	norm, truncated := normalizeDiagnostics(diags, plan, wsRoot, "warning", 100)
	if truncated {
		t.Fatal("unexpected truncation")
	}
	if len(norm) != 2 {
		t.Fatalf("len = %d, want 2 after severity filter: %+v", len(norm), norm)
	}
	if norm[0].Path != "a.go" || norm[1].Path != "b.go" {
		t.Fatalf("not sorted by path: %+v", norm)
	}

	_, truncated = normalizeDiagnostics(diags, plan, wsRoot, "all", 2)
	if !truncated {
		t.Fatal("expected truncation at max=2")
	}

	// Absolute paths under the workspace become relative; outside stays.
	diags = []diagnostic{{Path: filepath.Join(wsRoot, "sub", "c.go"), Line: 1, Severity: "error", Message: "x"}}
	norm, _ = normalizeDiagnostics(diags, plan, wsRoot, "all", 10)
	if norm[0].Path != "sub/c.go" {
		t.Fatalf("path = %q, want sub/c.go", norm[0].Path)
	}
}

// --- Execute end-to-end with fake runner ---

func TestExecuteSuccessWithDiagnostics(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "go.mod"), []byte("module example.com/x\n\ngo 1.25\n"))
	mustWriteFile(t, filepath.Join(root, "main.go"), []byte("package main\n"))
	stubLookPath(t, "golangci-lint")

	runner := &fakeRunner{result: process.Result{
		ExitCode: 1,
		Duration: 120 * time.Millisecond,
		Stdout:   []byte(`{"Issues":[{"FromLinter":"govet","Text":"printf problem","Pos":{"Filename":"main.go","Line":3,"Column":2}}]}`),
	}}
	tool := newLintTool(t, validator, runner)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "."}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}

	var out lintOutput
	decodeToolResult(t, result, &out)
	if out.Linter != linterGolangCI || out.DiagnosticCount != 1 || out.Diagnostics[0].Message != "printf problem" {
		t.Fatalf("unexpected output: %+v", out)
	}
	if out.Diagnostics[0].Path != "main.go" {
		t.Fatalf("diagnostic path = %q, want main.go", out.Diagnostics[0].Path)
	}
	if len(runner.calls) != 1 {
		t.Fatalf("runner calls = %d, want 1", len(runner.calls))
	}
	call := runner.calls[0]
	if call.Cwd != root || call.Timeout != lintTimeout || call.OutputLimit != lintOutputLimit {
		t.Fatalf("unexpected command spec: %+v", call)
	}
	if call.Env["GOCACHE"] == "" || call.Env["GOPROXY"] != "off" {
		t.Fatalf("expected go env overrides: %+v", call.Env)
	}
}

func TestExecuteFailures(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "go.mod"), []byte("module example.com/x\n\ngo 1.25\n"))
	mustWriteFile(t, filepath.Join(root, "main.go"), []byte("package main\n"))
	stubLookPath(t, "golangci-lint")

	t.Run("runner error is retryable", func(t *testing.T) {
		tool := newLintTool(t, validator, &fakeRunner{err: errors.New("spawn failed")})
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "."}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		result := tool.Execute(context.Background(), prepared)
		if result.Status != domain.ToolStatusError || result.Error.Code != string(domain.ErrUnavailable) || !result.Error.Retryable {
			t.Fatalf("unexpected result: %+v", result)
		}
	})

	t.Run("timeout", func(t *testing.T) {
		tool := newLintTool(t, validator, &fakeRunner{result: process.Result{TimedOut: true}})
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "."}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		result := tool.Execute(context.Background(), prepared)
		if result.Status != domain.ToolStatusTimeout || result.Error.Code != string(domain.ErrTimeout) {
			t.Fatalf("unexpected result: %+v", result)
		}
	})

	t.Run("linter crash includes stderr tail", func(t *testing.T) {
		tool := newLintTool(t, validator, &fakeRunner{result: process.Result{ExitCode: 3, Stderr: []byte("panic: boom")}})
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "."}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		result := tool.Execute(context.Background(), prepared)
		if result.Status != domain.ToolStatusError || !strings.Contains(result.Error.Message, "panic: boom") {
			t.Fatalf("unexpected result: %+v", result)
		}
	})

	t.Run("tampered prepared call", func(t *testing.T) {
		tool := newLintTool(t, validator, &fakeRunner{})
		prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: "."}))
		if err != nil {
			t.Fatalf("Prepare() error = %v", err)
		}
		prepared.ArgsHash = "deadbeef"
		result := tool.Execute(context.Background(), prepared)
		if result.Status != domain.ToolStatusError || result.Error.Code != string(domain.ErrSecurity) {
			t.Fatalf("unexpected result: %+v", result)
		}
	})
}

func TestExecuteGoVetNoteOnUnparseableOutput(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "go.mod"), []byte("module example.com/x\n\ngo 1.25\n"))
	mustWriteFile(t, filepath.Join(root, "main.go"), []byte("package main\n"))
	stubLookPath(t, "go")

	runner := &fakeRunner{result: process.Result{
		ExitCode: 1,
		Stderr:   []byte("# example.com/x\nmain.go:3:2: undefined: fmt.Printlnfoo"),
	}}
	tool := newLintTool(t, validator, runner)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: ".", Linter: linterGoVet}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}
	var out lintOutput
	decodeToolResult(t, result, &out)
	// The vet line parses as a normal diagnostic here; note stays empty.
	if out.DiagnosticCount != 1 || out.Diagnostics[0].Line != 3 {
		t.Fatalf("unexpected output: %+v", out)
	}
}

// --- real go vet end-to-end (skipped without a go toolchain) ---

func TestExecuteRealGoVet(t *testing.T) {
	if _, err := exec.LookPath("go"); err != nil {
		t.Skip("go toolchain not available")
	}
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "go.mod"), []byte("module example.com/vetcase\n\ngo 1.25\n"))
	mustWriteFile(t, filepath.Join(root, "main.go"), []byte(`package main

import "fmt"

func main() {
	fmt.Printf("%d")
}
`))

	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox:      process.ExplicitTestSandbox{},
		EnvAllowlist: []string{"PATH", "HOME", "TMPDIR", "GOCACHE", "GOPROXY", "GOFLAGS"},
	})
	if err != nil {
		t.Fatalf("NewRunner() error = %v", err)
	}
	tool := newLintTool(t, validator, runner)

	// Force go-vet so the outcome does not depend on whether golangci-lint
	// happens to be on PATH in the test environment.
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "lint", lintArgs{Path: ".", Linter: linterGoVet}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}
	var out lintOutput
	decodeToolResult(t, result, &out)
	if out.Linter != linterGoVet {
		t.Fatalf("linter = %s, want go-vet", out.Linter)
	}
	if out.DiagnosticCount == 0 {
		t.Fatalf("expected vet diagnostics for bad Printf call: %+v", out)
	}
	found := false
	for _, d := range out.Diagnostics {
		if strings.Contains(d.Message, "Printf") {
			found = true
		}
	}
	if !found {
		t.Fatalf("expected a Printf diagnostic: %+v", out.Diagnostics)
	}
}
