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

// Package lint implements the lint tool: deterministic project detection
// plus sandboxed linter execution, normalized into structured diagnostics.
// Supported engines: golangci-lint / go vet (go.mod), eslint (package.json
// with eslint config), ruff (pyproject.toml/ruff.toml) and clang-tidy
// (compile_commands.json, C/C++ file targets).
package lint

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultMaxDiagnostics = 100
	maxMaxDiagnostics     = 500
	lintTimeout           = 120 * time.Second
	lintOutputLimit       = 4 << 20
	stderrTailBytes       = 2048
)

type lintArgs struct {
	Path           string `json:"path,omitempty"`
	Linter         string `json:"linter,omitempty"`
	Severity       string `json:"severity,omitempty"`
	MaxDiagnostics int    `json:"max_diagnostics,omitempty"`
}

type lintOutput struct {
	Path            string       `json:"path"`
	ProjectRoot     string       `json:"project_root"`
	Linter          string       `json:"linter"`
	Command         string       `json:"command"`
	DiagnosticCount int          `json:"diagnostic_count"`
	Truncated       bool         `json:"truncated"`
	DurationMs      int64        `json:"duration_ms"`
	Diagnostics     []diagnostic `json:"diagnostics"`
	Note            string       `json:"note,omitempty"`
}

// cmdRunner abstracts process execution so tests can substitute fakes.
type cmdRunner interface {
	Run(ctx context.Context, spec process.CommandSpec) (process.Result, error)
}

// LintTool runs the project linter and reports structured diagnostics.
type LintTool struct {
	base   baseTool
	runner cmdRunner
}

// NewLintTool creates the lint tool. The runner must be the sandboxed
// process runner; a nil runner makes Execute fail with an internal error.
func NewLintTool(validator *workspacepkg.PathValidator, runner cmdRunner) (*LintTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "lint",
		Description: "Lint the project containing path and return structured diagnostics (errors/warnings with file, line, column and message). " +
			"The engine is detected from project markers: golangci-lint or go vet for go.mod, eslint for package.json, ruff for pyproject.toml, " +
			"clang-tidy for C/C++ files with a compile_commands.json. Pass an explicit linter to override detection. " +
			"Run this after editing code to catch problems early.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"linter":{"type":"string","enum":["auto","golangci-lint","go-vet","eslint","ruff","clang-tidy"]},"severity":{"type":"string","enum":["all","error","warning"]},"max_diagnostics":{"type":"integer","minimum":1,"maximum":500}}}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"project_root":{"type":"string"},"linter":{"type":"string"},"command":{"type":"string"},"diagnostic_count":{"type":"integer"},"truncated":{"type":"boolean"},"duration_ms":{"type":"integer"},"diagnostics":{"type":"array"},"note":{"type":"string"}},"required":["path","project_root","linter","command","diagnostic_count","truncated","duration_ms","diagnostics"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec, domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &LintTool{base: base, runner: runner}, nil
}

func (t *LintTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *LintTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[lintArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := t.validateArgs(args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	linterLabel := args.Linter
	if linterLabel == "" || linterLabel == linterAuto {
		linterLabel = "auto-detect"
	}
	approvalDesc := fmt.Sprintf("Lint %s (%s)", args.Path, linterLabel)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *LintTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}
	if t.runner == nil {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInternal, "lint tool requires a process runner"))
	}

	args, err := decodeStrict[lintArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, err := resolveExistingPath(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	plan, err := t.detect(pathInfo, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	result, err := t.runner.Run(ctx, process.CommandSpec{
		Program:     plan.Argv[0],
		Args:        plan.Argv[1:],
		Cwd:         plan.ProjectRoot,
		Env:         plan.Env,
		Timeout:     lintTimeout,
		OutputLimit: lintOutputLimit,
	})
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("failed to run %s", plan.Linter), domain.WithCause(err), domain.WithRetryable(true)))
	}
	if result.TimedOut {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrTimeout,
			fmt.Sprintf("%s timed out after %s", plan.Linter, lintTimeout), domain.WithRetryable(true)))
	}
	if !exitCodeExpected(plan.Linter, result.ExitCode) {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("%s failed with exit code %d: %s", plan.Linter, result.ExitCode, stderrTail(result.Stderr))))
	}

	diags, note, err := decodeDiagnostics(plan, result)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	normalized, truncated := normalizeDiagnostics(diags, plan, t.base.validator.Root(), args.Severity, args.MaxDiagnostics)

	return successResult(prepared.Call.ID, startedAt, lintOutput{
		Path:            args.Path,
		ProjectRoot:     plan.DisplayRoot,
		Linter:          plan.Linter,
		Command:         strings.Join(plan.Argv, " "),
		DiagnosticCount: len(normalized),
		Truncated:       truncated,
		DurationMs:      result.Duration.Milliseconds(),
		Diagnostics:     normalized,
		Note:            note,
	})
}

func (t *LintTool) validateArgs(args lintArgs) (lintArgs, pathResolution, error) {
	if args.Path == "" {
		args.Path = "."
	}
	switch args.Linter {
	case "", linterAuto, linterGolangCI, linterGoVet, linterESLint, linterRuff, linterClangTidy:
	default:
		return lintArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("linter must be one of auto, golangci-lint, go-vet, eslint, ruff, clang-tidy; got %q", args.Linter))
	}
	switch args.Severity {
	case "", "all", "error", "warning":
	default:
		return lintArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("severity must be one of all, error, warning; got %q", args.Severity))
	}
	if args.MaxDiagnostics == 0 {
		args.MaxDiagnostics = defaultMaxDiagnostics
	}
	if args.MaxDiagnostics < 1 || args.MaxDiagnostics > maxMaxDiagnostics {
		return lintArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("max_diagnostics must be between 1 and %d", maxMaxDiagnostics))
	}

	pathInfo, err := resolveExistingPath(t.base.validator, args.Path)
	if err != nil {
		return lintArgs{}, pathResolution{}, err
	}
	args.Path = pathInfo.Display
	return args, pathInfo, nil
}

// exitCodeExpected distinguishes "linter ran and possibly found issues" from
// "linter failed to run". All supported linters use 0 for clean and 1 for
// issues-found; any other code indicates a real failure.
func exitCodeExpected(linter string, code int) bool {
	switch linter {
	case linterGolangCI, linterGoVet, linterESLint, linterRuff:
		return code == 0 || code == 1
	case linterClangTidy:
		return code == 0
	default:
		return code == 0
	}
}

// decodeDiagnostics picks the raw output stream and parser for the engine.
// go vet reports on stderr; the JSON linters report on stdout.
func decodeDiagnostics(plan enginePlan, result process.Result) ([]diagnostic, string, error) {
	switch plan.Parse {
	case parseGolangCI:
		diags, err := parseGolangCIOutput(result.Stdout)
		return diags, "", err
	case parseESLint:
		diags, err := parseESLintOutput(result.Stdout)
		return diags, "", err
	case parseRuff:
		diags, err := parseRuffOutput(result.Stdout)
		return diags, "", err
	case parseGoVet:
		diags := parseGoVetOutput(result.Stderr)
		note := ""
		if len(diags) == 0 && result.ExitCode != 0 && len(result.Stderr) > 0 {
			// vet exited 1 without a single parseable diagnostic: surface the
			// raw output (usually a package load failure) instead of nothing.
			note = "go vet produced no parseable diagnostics; raw output tail: " + stderrTail(result.Stderr)
		}
		return diags, note, nil
	case parseClangTidy:
		return parseClangTidyOutput(result.Stdout), "", nil
	default:
		return nil, "", domain.NewError(domain.ErrInternal, "unknown parser kind")
	}
}

func stderrTail(stderr []byte) string {
	tail := stderr
	if len(tail) > stderrTailBytes {
		tail = tail[len(tail)-stderrTailBytes:]
	}
	return strings.TrimSpace(string(tail))
}
