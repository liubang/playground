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
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxProgramBytes                   = 4096
	maxArgsCount                      = 256
	maxArgBytes                       = 8192
	maxWorkingDirBytes                = 4096
	maxEnvVars                        = 64
	maxEnvKeyBytes                    = 256
	maxEnvValueBytes                  = 8192
	maxApprovalDescBytes              = 512
	approvalDescHashPrefixBytes       = 12
	minTimeoutMs                int64 = 1
	maxTimeoutMs                int64 = 10 * 60 * 1000
	maxOutputBytes              int64 = 1 << 20
	defaultModelOutputBytes           = 64 * 1024
)

type rawRunCmdArgs struct {
	Program            *string            `json:"program"`
	Args               *[]string          `json:"args"`
	WorkingDir         *string            `json:"working_dir"`
	Env                *map[string]string `json:"env"`
	TimeoutMs          *int64             `json:"timeout_ms"`
	MaxOutputBytes     *int64             `json:"max_output_bytes"`
	SandboxPermissions *string            `json:"sandbox_permissions"`
	NeedsNetwork       *bool              `json:"needs_network"`
	NeedsGUIOpen       *bool              `json:"needs_gui_open"`
	Justification      *string            `json:"justification"`
}

type runCmdArgs struct {
	Program            string            `json:"program"`
	Args               []string          `json:"args"`
	WorkingDir         string            `json:"working_dir"`
	Env                map[string]string `json:"env"`
	TimeoutMs          int64             `json:"timeout_ms"`
	MaxOutputBytes     int64             `json:"max_output_bytes"`
	SandboxPermissions string            `json:"sandbox_permissions"`
	NeedsNetwork       bool              `json:"needs_network,omitempty"`
	NeedsGUIOpen       bool              `json:"needs_gui_open,omitempty"`
	Justification      string            `json:"justification,omitempty"`
}

// sandbox_permissions values: the default sandboxed execution, or an
// escalated run outside the sandbox after explicit user approval.
const (
	sandboxUseDefault       = "use_default"
	sandboxRequireEscalated = "require_escalated"
	maxJustificationBytes   = 240
)

type runCmdOutput struct {
	Stdout                  string              `json:"stdout"`
	Stderr                  string              `json:"stderr"`
	StdoutBytes             int64               `json:"stdout_bytes"`
	StderrBytes             int64               `json:"stderr_bytes"`
	StdoutPreviewTruncated  bool                `json:"stdout_preview_truncated"`
	StderrPreviewTruncated  bool                `json:"stderr_preview_truncated"`
	StdoutArtifactTruncated bool                `json:"stdout_artifact_truncated"`
	StderrArtifactTruncated bool                `json:"stderr_artifact_truncated"`
	StdoutArtifact          *domain.ArtifactRef `json:"stdout_artifact,omitempty"`
	StderrArtifact          *domain.ArtifactRef `json:"stderr_artifact,omitempty"`
	// StdoutArtifactPath/StderrArtifactPath give the model a directly
	// readable location (run_cmd cat/sed/grep) instead of an opaque blob
	// ID; empty when the artifact store cannot resolve paths.
	StdoutArtifactPath string `json:"stdout_artifact_path,omitempty"`
	StderrArtifactPath string `json:"stderr_artifact_path,omitempty"`
	ExitCode           int    `json:"exit_code"`
	Signal             string `json:"signal"`
	DurationMs         int64  `json:"duration_ms"`
	TimedOut           bool   `json:"timed_out"`
	Cancelled          bool   `json:"cancelled"`
	Truncated          bool   `json:"truncated"`
	Isolation          string `json:"isolation"`
	ExecutablePath     string `json:"executable_path"`
	Hash               string `json:"hash"`
	Note               string `json:"note,omitempty"`
}

type preparedFingerprint struct {
	CallID      string                `json:"call_id"`
	Arguments   json.RawMessage       `json:"arguments"`
	ReadPaths   []string              `json:"read_paths"`
	WritePaths  []string              `json:"write_paths"`
	Risk        domain.RiskLevel      `json:"risk"`
	Definition  domain.ToolDefinition `json:"definition"`
	ExecRequest *domain.ExecRequest   `json:"exec_request,omitempty"`
}

type resolvedWorkingDir struct {
	Absolute string
	Display  string
}

// RunCmdTool adapts process.Runner as the builtin run_cmd domain tool.
type RunCmdTool struct {
	def              domain.ToolDefinition
	validator        *workspacepkg.PathValidator
	runner           *process.Runner
	artifacts        domain.ArtifactStore
	modelOutputBytes int
	key              [32]byte
}

// NewRunCmdTool creates a run_cmd tool bound to a workspace validator and process runner.
func NewRunCmdTool(
	validator *workspacepkg.PathValidator,
	runner *process.Runner,
) (*RunCmdTool, error) {
	return NewRunCmdToolWithArtifacts(validator, runner, nil, defaultModelOutputBytes)
}

// NewRunCmdToolWithArtifacts creates a run_cmd tool that externalizes
// captured output exceeding modelOutputBytes into an immutable artifact.
func NewRunCmdToolWithArtifacts(
	validator *workspacepkg.PathValidator,
	runner *process.Runner,
	artifacts domain.ArtifactStore,
	modelOutputBytes int,
) (*RunCmdTool, error) {
	if validator == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if runner == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "process runner is required")
	}
	if modelOutputBytes <= 0 {
		return nil, domain.NewError(domain.ErrInvalidInput, "model output limit must be positive")
	}
	def := domain.ToolDefinition{
		Name: "run_cmd",
		Description: "Execute a program in a sandbox. Always set working_dir explicitly instead of wrapping the command in 'cd ... &&' — it keeps the audit trail accurate. " +
			"Prefer plain argv form (program + args + env) for single commands; use program='sh' with args=['-c','...'] freely when you need pipes, redirection, '&&' chaining, or glob expansion — " +
			"both forms run sandboxed WITHOUT user approval; only danger-listed patterns (destructive commands, pipes into a shell, writes to sensitive paths) or sandbox escapes ever prompt. " +
			"argv is executed directly without a shell: wildcards like '*.go' are passed to the program literally, " +
			"so glob expansion is a legitimate reason to use the 'sh -c' form (or use the glob tool to find files first). " +
			"Only 'program' is required: working_dir defaults to '.', env to empty, timeout_ms to 120000, max_output_bytes to 65536. " +
			"Output beyond the limit is stored as an artifact with a head/tail preview. " +
			"Inside the sandbox, env entries are filtered by a security allowlist; keys that do not survive the filter are reported back in the output's 'note' field (escalated runs inherit the full user environment). " +
			"The sandbox denies outbound network and DNS but allows loopback networking (bind/listen/connect on localhost), " +
			"and denies writes outside the workspace and temp dir. " +
			"When a task-critical command fails (or hangs until the timeout) because the sandbox denied OUTBOUND NETWORK or DNS (SSO/OAuth, HTTP APIs, package downloads), " +
			"PREFER retrying the same command with needs_network=true: after a lightweight approval it runs INSIDE the sandbox with outbound network granted (credentials stay unreadable), and the user can remember it as a scoped rule. " +
			"When a command fails because it tried to OPEN A GUI APPLICATION (macOS 'open' a URL/app, Apple Events — LaunchServices/NSOSStatusErrorDomain errors), retry with needs_gui_open=true: after approval it runs INSIDE the sandbox with GUI-open granted. Use the same flag to proactively show the user a web page (open <url>). " +
			"Reserve sandbox_permissions='require_escalated' (with a short justification question) for failures network/gui cannot explain — writes outside the workspace, TTY needs, credential files — it runs OUTSIDE the sandbox with the full user environment after explicit approval (R3). " +
			"Do not give up or ask the user to run it themselves before offering the matching approval. " +
			"needs_network and needs_gui_open must NOT be combined with require_escalated (escalated runs already have full network and GUI access). " +
			"'justification' is an optional short note shown to the user at approval time; it is REQUIRED with sandbox_permissions='require_escalated' and simply informational otherwise.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"program":{"type":"string","minLength":1,"maxLength":4096},"args":{"type":"array","maxItems":256,"items":{"type":"string","maxLength":8192}},"working_dir":{"type":"string","minLength":1,"maxLength":4096},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192}},"timeout_ms":{"type":"integer","minimum":1,"maximum":600000},"max_output_bytes":{"type":"integer","minimum":1,"maximum":1048576},"sandbox_permissions":{"type":"string","enum":["use_default","require_escalated"]},"needs_network":{"type":"boolean"},"needs_gui_open":{"type":"boolean"},"justification":{"type":"string","minLength":1,"maxLength":240}},"required":["program"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"stdout":{"type":"string"},"stderr":{"type":"string"},"stdout_bytes":{"type":"integer"},"stderr_bytes":{"type":"integer"},"stdout_preview_truncated":{"type":"boolean"},"stderr_preview_truncated":{"type":"boolean"},"stdout_artifact_truncated":{"type":"boolean"},"stderr_artifact_truncated":{"type":"boolean"},"stdout_artifact":{"type":"object"},"stderr_artifact":{"type":"object"},"stdout_artifact_path":{"type":"string"},"stderr_artifact_path":{"type":"string"},"exit_code":{"type":"integer"},"signal":{"type":"string"},"duration_ms":{"type":"integer"},"timed_out":{"type":"boolean"},"cancelled":{"type":"boolean"},"truncated":{"type":"boolean"},"isolation":{"type":"string"},"executable_path":{"type":"string"},"hash":{"type":"string"},"note":{"type":"string"}},"required":["stdout","stderr","stdout_bytes","stderr_bytes","stdout_preview_truncated","stderr_preview_truncated","stdout_artifact_truncated","stderr_artifact_truncated","exit_code","signal","duration_ms","timed_out","cancelled","truncated","isolation","executable_path","hash"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}

	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return &RunCmdTool{
		def:              def,
		validator:        validator,
		runner:           runner,
		artifacts:        artifacts,
		modelOutputBytes: modelOutputBytes,
		key:              key,
	}, nil
}

func (t *RunCmdTool) Definition() domain.ToolDefinition {
	return t.def
}

func (t *RunCmdTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}

	rawArgs, err := decodeStrict[rawRunCmdArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, _, err := validateArgs(t.validator, rawArgs)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	root := t.validator.Root()
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: cloneRawMessage(canonical),
		},
		Definition: t.def,
		Risk:       riskForArgs(args, t.def.Risk()),
		ReadPaths:  []string{root},
		WritePaths: []string{root},
		// The typed execution contract the policy layer classifies on
		// (REVIEW M17/A2); covered by the signature below.
		ExecRequest: &domain.ExecRequest{
			Argv:         append([]string{args.Program}, args.Args...),
			Escalated:    args.SandboxPermissions == sandboxRequireEscalated,
			NeedsNetwork: args.NeedsNetwork,
			NeedsGUIOpen: args.NeedsGUIOpen,
		},
	}
	// Sign before rendering the description so the displayed args_hash
	// correlates with the ArgsHash recorded in permission events.
	prepared.ArgsHash = t.signPrepared(prepared)
	prepared.ApprovalDesc = buildApprovalDesc(args, prepared, t.validator.Root())
	return prepared, nil
}

// riskForArgs elevates to R3 only when the call escapes the default
// sandbox (require_escalated runs outside it with full user privileges).
// Shell invocations keep the base risk: the sandbox confines them the
// same as plain argv, and the permission layer's danger screen analyzes
// the script AST (per-subcommand screening, pipe-into-shell and
// sensitive-redirect detection) — composition alone is not a risk.
func riskForArgs(args runCmdArgs, base domain.RiskLevel) domain.RiskLevel {
	if args.SandboxPermissions == sandboxRequireEscalated {
		return domain.R3
	}
	return base
}

func (t *RunCmdTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if !hasOnlyWorkspaceRoot(prepared.ReadPaths, t.validator.Root()) || !hasOnlyWorkspaceRoot(prepared.WritePaths, t.validator.Root()) {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call workspace bindings are invalid"))
	}

	args, err := decodeStrict[runCmdArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	_, resolvedDir, err := validateCanonicalArgs(t.validator, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	stdoutStage, stderrStage, err := t.beginOutputArtifacts(ctx)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if stdoutStage != nil {
		defer stdoutStage.Abort()
	}
	if stderrStage != nil {
		defer stderrStage.Abort()
	}
	previewLimit := args.MaxOutputBytes
	if previewLimit > int64(t.modelOutputBytes) {
		previewLimit = int64(t.modelOutputBytes)
	}
	// The policy verdict's grant decides the execution mode
	// (docs/PERMISSION_DESIGN.md §3.2): a rule/session grant can even
	// DOWNGRADE a require_escalated request back into a widened sandbox —
	// downgrades only ever restrict. A zero grant means the DEFAULT
	// sandbox, even for require_escalated requests: the baseline stamps an
	// explicit Unsandboxed grant on every escalated ask it approves, so a
	// zero grant here can only come from a grant-less allow (L0) — and L0
	// trust must never be silently promoted to L2 unsandboxed execution.
	grant := process.Grant{
		Unsandboxed:   prepared.Grant.Unsandboxed,
		NetworkFull:   prepared.Grant.NetworkFull,
		WritablePaths: prepared.Grant.WritablePaths,
		GUIOpen:       prepared.Grant.GUIOpen,
	}
	runnerResult, err := t.runner.RunWithGrant(ctx, process.CommandSpec{
		Program:      args.Program,
		Args:         append([]string(nil), args.Args...),
		Cwd:          resolvedDir.Absolute,
		Env:          cloneStringMap(args.Env),
		Timeout:      time.Duration(args.TimeoutMs) * time.Millisecond,
		OutputLimit:  max(int64(1), previewLimit/2),
		StdoutWriter: stdoutStage,
		StderrWriter: stderrStage,
	}, grant)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyRunError(err))
	}

	commitCtx := ctx
	cancelCommit := func() {}
	if ctx == nil || ctx.Err() != nil {
		base := context.Background()
		if ctx != nil {
			base = context.WithoutCancel(ctx)
		}
		commitCtx, cancelCommit = context.WithTimeout(base, 5*time.Second)
	}
	defer cancelCommit()
	stdoutRef, stderrRef, err := commitOutputArtifacts(commitCtx, stdoutStage, stderrStage)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(
			domain.ErrUnavailable, "command completed but captured output could not be committed",
			domain.WithCause(err),
		))
	}
	payload := runCmdOutput{
		Stdout:                  sanitizeUTF8(runnerResult.Stdout),
		Stderr:                  sanitizeUTF8(runnerResult.Stderr),
		StdoutBytes:             runnerResult.StdoutBytes,
		StderrBytes:             runnerResult.StderrBytes,
		StdoutPreviewTruncated:  runnerResult.StdoutTruncated,
		StderrPreviewTruncated:  runnerResult.StderrTruncated,
		StdoutArtifactTruncated: stageTruncated(stdoutStage),
		StderrArtifactTruncated: stageTruncated(stderrStage),
		StdoutArtifact:          stdoutRef,
		StderrArtifact:          stderrRef,
		StdoutArtifactPath:      artifactPathFor(t.artifacts, stdoutRef),
		StderrArtifactPath:      artifactPathFor(t.artifacts, stderrRef),
		ExitCode:                runnerResult.ExitCode,
		Signal:                  runnerResult.Signal,
		DurationMs:              durationMilliseconds(runnerResult.Duration),
		TimedOut:                runnerResult.TimedOut,
		Cancelled:               runnerResult.Cancelled,
		Truncated:               runnerResult.Truncated || stageTruncated(stdoutStage) || stageTruncated(stderrStage),
		Isolation:               runnerResult.Isolation,
		ExecutablePath:          runnerResult.ExecutablePath,
		Hash:                    runnerResult.ExecutableHash,
		Note: combineNotes(
			escalationDowngradeNote(args.SandboxPermissions == sandboxRequireEscalated, prepared.Grant.Unsandboxed),
			commandNotFoundNote(
				string(runnerResult.Stderr),
				runnerResult.ExitCode,
				args.SandboxPermissions == sandboxRequireEscalated,
				args.Env,
			),
			sandboxGuidanceNote(
				string(runnerResult.Stderr),
				runnerResult.TimedOut,
				args.SandboxPermissions == sandboxRequireEscalated,
			),
			droppedEnvNote(runnerResult.DroppedEnvKeys),
		),
	}
	if err := boundCommandOutput(&payload, t.modelOutputBytes); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	status := domain.ToolStatusSuccess
	if runnerResult.TimedOut {
		status = domain.ToolStatusTimeout
	} else if runnerResult.Cancelled {
		status = domain.ToolStatusCancelled
	}
	return contentResultWithArtifacts(prepared.Call.ID, status, startedAt, payload, stdoutRef, stderrRef)
}

func (t *RunCmdTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != t.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if !sameDefinition(prepared.Definition, t.def) {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	// The risk tier depends on the program and sandbox mode (shell or
	// escalated ⇒ R3), so recompute it from the signed arguments instead of
	// assuming the definition default.
	args, err := decodeStrict[runCmdArgs](prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments are unreadable")
	}
	if prepared.Risk != riskForArgs(args, t.def.Risk()) {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	if expected := t.signPrepared(prepared); !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func (t *RunCmdTool) signPrepared(prepared domain.PreparedCall) string {
	fingerprint := preparedFingerprint{
		CallID:      prepared.Call.ID.String(),
		Arguments:   cloneRawMessage(prepared.Call.Arguments),
		ReadPaths:   append([]string(nil), prepared.ReadPaths...),
		WritePaths:  append([]string(nil), prepared.WritePaths...),
		Risk:        prepared.Risk,
		Definition:  prepared.Definition,
		ExecRequest: prepared.ExecRequest,
	}
	payload, _ := json.Marshal(fingerprint)
	h := hmac.New(sha256.New, t.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

// Default values applied when the model omits optional parameters, keeping
// run_cmd calls terse (only 'program' is required).
const (
	defaultTimeoutMs      int64 = 120000
	defaultMaxOutputBytes int64 = 64 << 10
)

func validateArgs(
	validator *workspacepkg.PathValidator,
	raw rawRunCmdArgs,
) (runCmdArgs, resolvedWorkingDir, error) {
	if raw.Program == nil {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program is required")
	}

	args := runCmdArgs{
		Program:            strings.TrimSpace(*raw.Program),
		Args:               []string{},
		Env:                map[string]string{},
		TimeoutMs:          defaultTimeoutMs,
		MaxOutputBytes:     defaultMaxOutputBytes,
		SandboxPermissions: sandboxUseDefault,
	}
	if raw.SandboxPermissions != nil {
		args.SandboxPermissions = strings.TrimSpace(*raw.SandboxPermissions)
	}
	if raw.NeedsNetwork != nil {
		args.NeedsNetwork = *raw.NeedsNetwork
	}
	if raw.NeedsGUIOpen != nil {
		args.NeedsGUIOpen = *raw.NeedsGUIOpen
	}
	if raw.Justification != nil {
		args.Justification = strings.TrimSpace(*raw.Justification)
	}
	if raw.Args != nil {
		args.Args = append([]string(nil), (*raw.Args)...)
	}
	if raw.Env != nil {
		args.Env = cloneStringMap(*raw.Env)
	}
	if raw.TimeoutMs != nil {
		args.TimeoutMs = *raw.TimeoutMs
	}
	if raw.MaxOutputBytes != nil {
		args.MaxOutputBytes = *raw.MaxOutputBytes
	}

	workingDir := "."
	if raw.WorkingDir != nil {
		workingDir = *raw.WorkingDir
	}
	resolvedDir, err := resolveWorkingDir(validator, workingDir)
	if err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	args.WorkingDir = resolvedDir.Display
	return validateCanonicalArgs(validator, args)
}

func validateCanonicalArgs(
	validator *workspacepkg.PathValidator,
	args runCmdArgs,
) (runCmdArgs, resolvedWorkingDir, error) {
	if validator == nil {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if args.Program == "" {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program is required")
	}
	if len(args.Program) > maxProgramBytes {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("program exceeds %d bytes", maxProgramBytes))
	}
	if strings.ContainsRune(args.Program, 0) {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program contains null byte")
	}
	if len(args.Args) > maxArgsCount {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args exceeds %d items", maxArgsCount))
	}
	for i, arg := range args.Args {
		if len(arg) > maxArgBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] exceeds %d bytes", i, maxArgBytes))
		}
		if strings.ContainsRune(arg, 0) {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] contains null byte", i))
		}
	}
	resolvedDir, err := resolveWorkingDir(validator, args.WorkingDir)
	if err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	args.WorkingDir = resolvedDir.Display
	if args.TimeoutMs < minTimeoutMs || args.TimeoutMs > maxTimeoutMs {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("timeout_ms must be between %d and %d", minTimeoutMs, maxTimeoutMs))
	}
	if args.MaxOutputBytes < 1 || args.MaxOutputBytes > maxOutputBytes {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 1 and %d", maxOutputBytes))
	}
	if len(args.Env) > maxEnvVars {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env exceeds %d entries", maxEnvVars))
	}
	canonicalEnv := make(map[string]string, len(args.Env))
	for key, value := range args.Env {
		if len(key) == 0 {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "env contains an empty key")
		}
		if len(key) > maxEnvKeyBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q exceeds %d bytes", key, maxEnvKeyBytes))
		}
		if strings.ContainsAny(key, "=\x00") {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q is invalid", key))
		}
		if len(value) > maxEnvValueBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q exceeds %d bytes", key, maxEnvValueBytes))
		}
		if strings.ContainsRune(value, 0) {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q contains null byte", key))
		}
		canonicalEnv[key] = value
	}
	args.Env = canonicalEnv

	switch args.SandboxPermissions {
	case sandboxUseDefault:
		// A justification is accepted on any call: it is only an
		// informational note shown to the approver, with no effect on
		// privileges. Rejecting it for sandboxed calls taught models to
		// retry the same call in a loop (observed 11 consecutive
		// prepare_failed results in one session).
		if len(args.Justification) > maxJustificationBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("justification exceeds %d bytes", maxJustificationBytes))
		}
	case sandboxRequireEscalated:
		if args.NeedsNetwork || args.NeedsGUIOpen {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "needs_network/needs_gui_open cannot be combined with sandbox_permissions=require_escalated (escalated runs already have full network and GUI access; use the needs_* flags for the sandboxed path)")
		}
		if args.Justification == "" {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "justification is required with sandbox_permissions=require_escalated (ask the user a short yes/no question)")
		}
		if len(args.Justification) > maxJustificationBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("justification exceeds %d bytes", maxJustificationBytes))
		}
	default:
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("sandbox_permissions must be %q or %q", sandboxUseDefault, sandboxRequireEscalated))
	}
	return args, resolvedDir, nil
}

func resolveWorkingDir(
	validator *workspacepkg.PathValidator,
	input string,
) (resolvedWorkingDir, error) {
	if validator == nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if strings.TrimSpace(input) == "" {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir is required")
	}
	if len(input) > maxWorkingDirBytes {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir exceeds %d bytes", maxWorkingDirBytes))
	}
	absolute, err := validator.Validate(input)
	if err != nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(absolute)
	if err != nil {
		if os.IsNotExist(err) {
			// Echo the offending path so the model can correct course
			// without guessing which working_dir was rejected.
			return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir does not exist: %q", domain.TruncateForErrorEcho(input)), domain.WithCause(err))
		}
		return resolvedWorkingDir{}, domain.NewError(domain.ErrUnavailable, "failed to stat working_dir", domain.WithCause(err))
	}
	if !info.IsDir() {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir must be a directory")
	}
	rel, err := filepath.Rel(validator.Root(), absolute)
	if err != nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInternal, "failed to normalize working_dir", domain.WithCause(err))
	}
	return resolvedWorkingDir{Absolute: absolute, Display: displayPath(rel)}, nil
}

// sandboxGuidanceNote detects the fingerprints of sandbox denials — network
// and DNS failures, socket and write denials, and timeouts that look like
// network hangs — and returns an actionable note for the model, so the
// constraint is learned from the first failure instead of inferred over
// several attempts. The note points at the require_escalated retry the tool
// description documents: loom can still run the command, just outside the
// sandbox with explicit approval. Escalated runs get no note (nothing
// sandbox-related to learn), and "" is returned when nothing matches.
// droppedEnvNote surfaces env override keys the sandbox allowlist filtered
// out, so the model learns the constraint from the first run instead of
// retrying against an environment it believes exists.
func droppedEnvNote(dropped []string) string {
	if len(dropped) == 0 {
		return ""
	}
	return fmt.Sprintf("env keys dropped by the sandbox allowlist: %s", strings.Join(dropped, ", "))
}

// escalationDowngradeNote tells the model when a require_escalated call
// ran WITHOUT the unsandboxed grant it asked for. Without this signal the
// failure is indistinguishable from an ordinary sandbox denial, and the
// model resorts to inventing sandbox workarounds instead of surfacing
// that an explicit approval is required.
func escalationDowngradeNote(escalated bool, grantUnsandboxed bool) string {
	if !escalated || grantUnsandboxed {
		return ""
	}
	return "require_escalated was NOT honored: policy downgraded this call to sandboxed execution " +
		"(see the isolation field). Do NOT write workaround scripts or patches to bypass the sandbox — " +
		"unsandboxed execution requires an explicit user approval; if this command failed because of " +
		"sandbox restrictions, inform the user that approving the escalation prompt is required."
}

// commandNotFoundNote detects the shell's missing-program fingerprint
// (exit 127 + "command not found" on stderr — the `sh -c` path never
// reaches loom's own LookPath error) and teaches the PATH fix instead of
// an escalation: a GUI-launched loom inherits a sparse system PATH, so a
// missing tool almost never means the sandbox must be escaped — the last
// thing we want is the model learning "command not found ⇒
// require_escalated". Escalated runs get no note (they already see the
// full user environment).
func commandNotFoundNote(stderr string, exitCode int, escalated bool, envOverrides map[string]string) string {
	if escalated || exitCode != 127 {
		return ""
	}
	if !strings.Contains(strings.ToLower(stderr), "command not found") {
		return ""
	}
	effectivePATH := strings.TrimSpace(envOverrides["PATH"])
	if effectivePATH == "" {
		effectivePATH = os.Getenv("PATH")
	}
	return "a program was not found on the sandbox PATH (" + effectivePATH + "). " +
		"loom launched from a GUI inherits a sparse system PATH without user toolchains. " +
		"Do NOT retry with sandbox_permissions='require_escalated' for a missing tool — the sandbox is not the problem, the PATH is. " +
		"Instead: locate the binary and use an absolute program path or pass env={\"PATH\": \"<dir>:...\"} explicitly; " +
		"for a permanent fix, register the directory in tools.path_extra in the loom config (hot-applied on save)."
}

// combineNotes joins non-empty notes with a separator.
func combineNotes(notes ...string) string {
	parts := make([]string, 0, len(notes))
	for _, n := range notes {
		if n != "" {
			parts = append(parts, n)
		}
	}
	return strings.Join(parts, "; ")
}

func sandboxGuidanceNote(stderr string, timedOut, escalated bool) string {
	if escalated {
		return ""
	}
	const networkAdvice = "If this failure is caused by missing OUTBOUND NETWORK or DNS (SSO/OAuth, HTTP APIs, package downloads), " +
		"PREFER retrying the SAME command with needs_network=true — after a lightweight approval it runs INSIDE the sandbox with network granted. " +
		"Only when the failure is NOT network-related (write access outside the workspace/temp dir, TTY, credential files like SSO tokens the sandbox hides), " +
		"retry with sandbox_permissions='require_escalated' and a short justification — after approval it runs OUTSIDE the sandbox with the full user environment. " +
		"Do not give up or ask the user to run it themselves before offering the matching approval."
	lower := strings.ToLower(stderr)
	// GUI-denial signatures must be checked FIRST: Apple Event rejections
	// typically surface as "operation not permitted", which the generic
	// otherPatterns below would misclassify as network/write guidance
	// (docs/BROWSER_DESIGN.md §4.2).
	guiPatterns := []string{
		"_lsline", "nsosstatuserrordomain", // LaunchServices error payloads
		"appleevent", "erraevent", "procnotfound", // Apple Event delivery
		"lsopenurlswithrole", "kls", // LSOpenURLsWithRole / kLS* codes
	}
	for _, p := range guiPatterns {
		if strings.Contains(lower, p) {
			return "the sandbox denies driving GUI applications (macOS 'open', Apple Events). " +
				"If this command tried to open a URL or application, retry the SAME command with needs_gui_open=true — " +
				"after a lightweight approval it runs INSIDE the sandbox with GUI-open granted, and the user can remember it as a scoped rule. " +
				"Only when the failure is NOT GUI-related, fall back to the network/escalated guidance below."
		}
	}
	networkPatterns := []string{
		"no such host", "nodename nor servname", "name or service not known", // DNS resolution
		"could not resolve", "temporary failure in name resolution",
		"network is unreachable", "can't assign requested address",
	}
	for _, p := range networkPatterns {
		if strings.Contains(lower, p) {
			return "outbound network and DNS are denied by the sandbox (loopback networking on localhost is allowed). " + networkAdvice
		}
	}
	otherPatterns := []string{
		"address family not supported", "operation not permitted", // socket/bind/listen denials
		"read-only file system", // write outside workspace/temp dir
	}
	for _, p := range otherPatterns {
		if strings.Contains(lower, p) {
			return "the sandbox restricts networking to loopback and writes to the workspace and temp dir. " + networkAdvice
		}
	}
	if timedOut {
		return "the command was killed by the timeout while running inside the sandbox. Sandboxed commands have no outbound network or DNS (loopback only), " +
			"so network-dependent commands (SSO/OAuth, HTTP APIs, package downloads) usually hang until the timeout instead of failing fast. " + networkAdvice
	}
	return ""
}

func classifyRunError(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, process.ErrSandboxRequired), errors.Is(err, process.ErrSandboxUnavailable):
		return domain.NewError(domain.ErrUnavailable, "process sandbox is unavailable", domain.WithCause(err))
	case errors.Is(err, process.ErrExecutableHashChanged):
		return domain.NewError(domain.ErrSecurity, "resolved executable changed before start", domain.WithCause(err))
	case errors.Is(err, context.Canceled):
		return domain.NewError(domain.ErrCancelled, "operation cancelled", domain.WithCause(err))
	case errors.Is(err, context.DeadlineExceeded):
		return domain.NewError(domain.ErrTimeout, "operation timed out", domain.WithCause(err))
	}

	var agentErr *domain.AgentError
	if errors.As(err, &agentErr) {
		return err
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
	}
	if errors.Is(err, exec.ErrNotFound) || strings.Contains(err.Error(), "executable file not found") {
		return domain.NewError(domain.ErrInvalidInput,
			"program could not be resolved on PATH ("+os.Getenv("PATH")+"); "+
				"use an absolute program path, pass env={\"PATH\": \"<dir>:...\"}, or register the directory in tools.path_extra in the loom config",
			domain.WithCause(err))
	}
	if strings.Contains(err.Error(), "validate cwd") {
		return domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	if strings.Contains(err.Error(), "stdout pipe") || strings.Contains(err.Error(), "stderr pipe") || strings.Contains(err.Error(), "start command") || strings.Contains(err.Error(), "wait command") {
		return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
}

func decodeStrict[T any](raw json.RawMessage) (T, error) {
	var out T
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&out); err != nil {
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must be valid JSON matching the tool schema", domain.WithCause(err))
	}
	var trailing struct{}
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value")
		}
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value", domain.WithCause(err))
	}
	return out, nil
}

func boundCommandOutput(payload *runCmdOutput, limit int) error {
	if payload == nil || limit <= 0 {
		return domain.NewError(domain.ErrInvalidInput, "valid command output and model limit are required")
	}
	stdout, stderr := payload.Stdout, payload.Stderr
	payload.Stdout, payload.Stderr = "", ""
	base, err := json.Marshal(payload)
	if err != nil {
		return domain.NewError(domain.ErrInternal, "encode command output metadata", domain.WithCause(err))
	}
	remaining := limit - len(base)
	if remaining < 0 {
		return domain.NewError(domain.ErrBudget, "command output metadata exceeds model output limit")
	}
	stdoutBudget := remaining / 2
	stderrBudget := remaining - stdoutBudget
	if len(stdout) < stdoutBudget {
		stderrBudget += stdoutBudget - len(stdout)
		stdoutBudget = len(stdout)
	}
	if len(stderr) < stderrBudget {
		stdoutBudget += stderrBudget - len(stderr)
		stderrBudget = len(stderr)
	}
	payload.Stdout = boundedHeadTailString(stdout, stdoutBudget)
	payload.Stderr = boundedHeadTailString(stderr, stderrBudget)
	for {
		encoded, err := json.Marshal(payload)
		if err != nil {
			return domain.NewError(domain.ErrInternal, "encode bounded command output", domain.WithCause(err))
		}
		if len(encoded) <= limit {
			return nil
		}
		overflow := len(encoded) - limit
		if len(payload.Stderr) >= len(payload.Stdout) && len(payload.Stderr) > 0 {
			payload.Stderr = boundedHeadTailString(payload.Stderr, max(0, len(payload.Stderr)-overflow))
		} else if len(payload.Stdout) > 0 {
			payload.Stdout = boundedHeadTailString(payload.Stdout, max(0, len(payload.Stdout)-overflow))
		} else {
			return domain.NewError(domain.ErrBudget, "command output cannot fit model output limit")
		}
	}
}

func boundedHeadTailString(value string, limit int) string {
	if limit <= 0 {
		return ""
	}
	if len(value) <= limit {
		return value
	}
	const marker = "\n...[output omitted]...\n"
	if limit <= len(marker) {
		return truncateWithMarker(value, limit)
	}
	headBytes := (limit - len(marker)) * 3 / 8
	tailBytes := limit - len(marker) - headBytes
	head := value[:headBytes]
	for len(head) > 0 && !utf8.ValidString(head) {
		head = head[:len(head)-1]
	}
	tail := value[len(value)-tailBytes:]
	for len(tail) > 0 && !utf8.ValidString(tail) {
		tail = tail[1:]
	}
	return head + marker + tail
}

// artifactPathResolver is implemented by the concrete artifact store;
// it gives the model a directly readable path instead of an opaque ID.
type artifactPathResolver interface {
	PathForRef(ref domain.ArtifactRef) (string, bool)
}

// artifactPathFor resolves the on-disk path of a committed artifact, or
// empty when the store cannot resolve paths or the ref is absent.
func artifactPathFor(store domain.ArtifactStore, ref *domain.ArtifactRef) string {
	if store == nil || ref == nil {
		return ""
	}
	resolver, ok := store.(artifactPathResolver)
	if !ok {
		return ""
	}
	path, found := resolver.PathForRef(*ref)
	if !found {
		return ""
	}
	return path
}

func (t *RunCmdTool) beginOutputArtifacts(ctx context.Context) (domain.StagedArtifact, domain.StagedArtifact, error) {
	if t.artifacts == nil {
		return nil, nil, nil
	}
	stdout, err := t.artifacts.Begin(ctx)
	if err != nil {
		return nil, nil, domain.NewError(domain.ErrUnavailable, "begin stdout artifact", domain.WithCause(err))
	}
	stderr, err := t.artifacts.Begin(ctx)
	if err != nil {
		_ = stdout.Abort()
		return nil, nil, domain.NewError(domain.ErrUnavailable, "begin stderr artifact", domain.WithCause(err))
	}
	return stdout, stderr, nil
}

func commitOutputArtifacts(
	ctx context.Context,
	stdout, stderr domain.StagedArtifact,
) (*domain.ArtifactRef, *domain.ArtifactRef, error) {
	var stdoutRef, stderrRef *domain.ArtifactRef
	if stdout != nil && stdout.TotalBytes() > 0 {
		ref, err := stdout.Commit(ctx)
		if err != nil {
			return nil, nil, fmt.Errorf("commit stdout artifact: %w", err)
		}
		stdoutRef = &ref
	} else if stdout != nil {
		_ = stdout.Abort()
	}
	if stderr != nil && stderr.TotalBytes() > 0 {
		ref, err := stderr.Commit(ctx)
		if err != nil {
			return stdoutRef, nil, fmt.Errorf("commit stderr artifact: %w", err)
		}
		stderrRef = &ref
	} else if stderr != nil {
		_ = stderr.Abort()
	}
	return stdoutRef, stderrRef, nil
}

func stageTruncated(stage domain.StagedArtifact) bool {
	return stage != nil && stage.Truncated()
}

func contentResultWithArtifacts(
	callID domain.ToolCallID,
	status domain.ToolStatus,
	startedAt time.Time,
	payload any,
	stdoutRef, stderrRef *domain.ArtifactRef,
) domain.ToolResult {
	content, err := json.Marshal(payload)
	if err != nil {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	parts := []domain.ContentPart{{Kind: domain.PartText, Text: string(content)}}
	metadata := map[string]string{}
	// stdout/stderr artifacts are captured process output — declare them as
	// text so renderers don't mistake them for images.
	if stdoutRef != nil {
		stdoutRef.MediaType = "text/plain"
		parts = append(parts, domain.ContentPart{Kind: domain.PartArtifact, Artifact: stdoutRef})
		metadata["stdout_artifact_id"] = stdoutRef.ID.String()
		metadata["stdout_artifact_size"] = fmt.Sprintf("%d", stdoutRef.Size)
	}
	if stderrRef != nil {
		stderrRef.MediaType = "text/plain"
		parts = append(parts, domain.ContentPart{Kind: domain.PartArtifact, Artifact: stderrRef})
		metadata["stderr_artifact_id"] = stderrRef.ID.String()
		metadata["stderr_artifact_size"] = fmt.Sprintf("%d", stderrRef.Size)
	}
	if len(metadata) == 0 {
		metadata = nil
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     status,
		Content:    parts,
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
		Metadata:   metadata,
	}
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	status := domain.ToolStatusError
	code := string(domain.ErrInternal)
	message := "internal tool error"
	retryable := false

	switch {
	case errors.Is(err, context.Canceled):
		status = domain.ToolStatusCancelled
		code = string(domain.ErrCancelled)
		message = "operation cancelled"
	case errors.Is(err, context.DeadlineExceeded):
		status = domain.ToolStatusTimeout
		code = string(domain.ErrTimeout)
		message = "operation timed out"
	default:
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			code = string(agentErr.Code)
			message = agentErr.Message
			retryable = agentErr.Retryable
			switch agentErr.Code {
			case domain.ErrCancelled:
				status = domain.ToolStatusCancelled
			case domain.ErrTimeout:
				status = domain.ToolStatusTimeout
			}
		}
	}

	return domain.ToolResult{
		CallID: callID,
		Status: status,
		Error: &domain.ToolError{
			Code:      code,
			Message:   message,
			Retryable: retryable,
		},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

func sameDefinition(left, right domain.ToolDefinition) bool {
	if left.Name != right.Name || left.Description != right.Description || left.Source != right.Source {
		return false
	}
	if string(left.InputSchema) != string(right.InputSchema) || string(left.OutputSchema) != string(right.OutputSchema) {
		return false
	}
	if len(left.Capabilities) != len(right.Capabilities) {
		return false
	}
	for i := range left.Capabilities {
		if left.Capabilities[i] != right.Capabilities[i] {
			return false
		}
	}
	return true
}

func cloneRawMessage(raw json.RawMessage) json.RawMessage {
	if raw == nil {
		return nil
	}
	return append(json.RawMessage(nil), raw...)
}

func cloneStringMap(values map[string]string) map[string]string {
	if len(values) == 0 {
		return map[string]string{}
	}
	out := make(map[string]string, len(values))
	for key, value := range values {
		out[key] = value
	}
	return out
}

func hasOnlyWorkspaceRoot(paths []string, root string) bool {
	if len(paths) != 1 {
		return false
	}
	return filepath.Clean(paths[0]) == filepath.Clean(root)
}

func sanitizeUTF8(data []byte) string {
	return string(bytes.ToValidUTF8(data, []byte("?")))
}

func durationMilliseconds(d time.Duration) int64 {
	if d <= 0 {
		return 0
	}
	return d.Milliseconds()
}

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func buildApprovalDesc(args runCmdArgs, prepared domain.PreparedCall, root string) string {
	parts := []string{"Run"}
	command := append([]string{args.Program}, args.Args...)
	quoted := make([]string, 0, len(command))
	for _, item := range command {
		quoted = append(quoted, shellQuote(item))
	}
	parts = append(parts, strings.Join(quoted, " "))

	envKeys := sortedEnvKeys(args.Env)
	if len(envKeys) > 0 {
		parts = append(parts, "env["+strings.Join(envKeys, ", ")+"]")
	} else {
		parts = append(parts, "env[none]")
	}
	parts = append(parts, "cwd="+shellQuote(args.WorkingDir))
	parts = append(parts, fmt.Sprintf("timeout=%dms", args.TimeoutMs))
	if process.IsShellProgram(args.Program) {
		parts = append(parts, "shell=parsed")
	}
	if args.SandboxPermissions == sandboxRequireEscalated {
		parts = append(parts, "network=full")
		parts = append(parts, "ESCALATED(no-sandbox)["+args.Justification+"]")
	} else if args.NeedsNetwork {
		parts = append(parts, "network=requested-in-sandbox")
		if args.Justification != "" {
			parts = append(parts, "note["+args.Justification+"]")
		}
	} else {
		parts = append(parts, "network=loopback-only")
		if args.Justification != "" {
			parts = append(parts, "note["+args.Justification+"]")
		}
	}

	// Surface workspace-external absolute paths referenced by argv (e.g.
	// 'ls ~/.loom'): ReadPaths stays the workspace-root enforcement
	// contract, so without this hint the summary under-reports what the
	// command may read. Display-only; enforcement is the sandbox's job.
	if refs := referencedExternalPaths(args, root); len(refs) > 0 {
		const maxRefCount = 6
		shown := refs
		marker := ""
		if len(refs) > maxRefCount {
			shown = refs[:maxRefCount]
			marker = ", …"
		}
		parts = append(parts, "refs["+strings.Join(shown, ", ")+marker+"]")
	}

	base := strings.Join(parts, "; ")
	// Display a prefix of the signed HMAC ArgsHash: compact, yet correlates
	// with the full hash recorded in permission audit events.
	argsHash := prepared.ArgsHash
	if len(argsHash) > approvalDescHashPrefixBytes {
		argsHash = argsHash[:approvalDescHashPrefixBytes]
	}
	suffix := fmt.Sprintf("; args_hash=%s", argsHash)
	truncated := truncateWithMarker(base, maxApprovalDescBytes-len(suffix))
	return truncated + suffix
}

// referencedExternalPaths best-effort extracts absolute paths outside the
// workspace from argv (shell -c payloads are split on shell metacharacters
// first). Used only to keep the approval summary honest about what a
// command may touch; it is not an enforcement boundary.
func referencedExternalPaths(args runCmdArgs, root string) []string {
	tokens := append([]string(nil), args.Args...)
	if process.IsShellProgram(args.Program) {
		var split []string
		for _, a := range args.Args {
			split = append(split, strings.FieldsFunc(a, func(r rune) bool {
				switch r {
				case ' ', '\t', '\n', '"', '\'', ';', '|', '&', '(', ')', '<', '>', '=', ',', '`':
					return true
				}
				return false
			})...)
		}
		tokens = append(tokens, split...)
	}
	rootClean := filepath.Clean(root)
	seen := map[string]bool{}
	out := make([]string, 0, 4)
	for _, tok := range tokens {
		if !filepath.IsAbs(tok) {
			continue
		}
		cleaned := filepath.Clean(tok)
		if cleaned == rootClean || strings.HasPrefix(cleaned, rootClean+string(filepath.Separator)) {
			continue // workspace 内路径已由 ReadPaths 声明
		}
		if seen[cleaned] {
			continue
		}
		seen[cleaned] = true
		out = append(out, cleaned)
	}
	sort.Strings(out)
	return out
}

func sortedEnvKeys(env map[string]string) []string {
	keys := make([]string, 0, len(env))
	for key := range env {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func shellQuote(value string) string {
	if value == "" {
		return "''"
	}
	var b strings.Builder
	b.Grow(len(value) + 2)
	b.WriteByte('\'')
	for _, r := range value {
		if r == '\'' {
			b.WriteString(`'"'"'`)
			continue
		}
		b.WriteRune(r)
	}
	b.WriteByte('\'')
	return b.String()
}

func truncateWithMarker(value string, maxBytes int) string {
	if maxBytes <= 0 {
		return "[truncated]"
	}
	if len(value) <= maxBytes {
		return value
	}
	const marker = "...[truncated]"
	if maxBytes <= len(marker) {
		return marker[:maxBytes]
	}
	trimmed := value[:maxBytes-len(marker)]
	for !utf8.ValidString(trimmed) && len(trimmed) > 0 {
		trimmed = trimmed[:len(trimmed)-1]
	}
	return trimmed + marker
}
