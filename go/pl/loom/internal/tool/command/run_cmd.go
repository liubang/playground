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
	"crypto/hmac"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxApprovalDescBytes              = 512
	approvalDescHashPrefixBytes       = 12
	minTimeoutMs                int64 = 1
	maxTimeoutMs                int64 = 10 * 60 * 1000
	maxOutputBytes              int64 = 1 << 20
	defaultModelOutputBytes           = 64 * 1024
	maxJustificationBytes             = 240
	maxWritablePaths                  = 8
)

type rawRunCmdArgs struct {
	Command            *string            `json:"command"`
	WorkingDir         *string            `json:"working_dir"`
	Env                *map[string]string `json:"env"`
	TimeoutMs          *int64             `json:"timeout_ms"`
	MaxOutputBytes     *int64             `json:"max_output_bytes"`
	SandboxPermissions *string            `json:"sandbox_permissions"`
	NeedsNetwork       *bool              `json:"needs_network"`
	NeedsGUIOpen       *bool              `json:"needs_gui_open"`
	WritablePaths      *[]string          `json:"writable_paths"`
	Justification      *string            `json:"justification"`
}

type runCmdArgs struct {
	Command            string            `json:"command"`
	WorkingDir         string            `json:"working_dir"`
	Env                map[string]string `json:"env"`
	TimeoutMs          int64             `json:"timeout_ms"`
	MaxOutputBytes     int64             `json:"max_output_bytes"`
	SandboxPermissions string            `json:"sandbox_permissions"`
	NeedsNetwork       bool              `json:"needs_network,omitempty"`
	NeedsGUIOpen       bool              `json:"needs_gui_open,omitempty"`
	WritablePaths      []string          `json:"writable_paths,omitempty"`
	Justification      string            `json:"justification,omitempty"`
}

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
	signer           toolkit.Signer
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
		Description: "Execute a shell command inside a sandbox and return its stdout, stderr, exit code and timing. " +
			"The command runs via 'sh -c' — write it exactly as you would type it in a terminal: pipes, redirection, '&&' chaining, globs and quoting all work. " +
			"Examples: {\"command\":\"go test ./...\"} · {\"command\":\"python3 plot.py\",\"working_dir\":\"scripts\",\"timeout_ms\":300000} · {\"command\":\"curl -sI https://example.com\",\"needs_network\":true}. " +
			"Set working_dir explicitly instead of wrapping the command in 'cd ... &&' — it keeps the audit trail accurate. " +
			"Only 'command' is required: working_dir defaults to '.', env to empty, timeout_ms to 120000, max_output_bytes to 65536. " +
			"Output beyond the limit is stored as an artifact with a head/tail preview. " +
			"Inside the sandbox, env entries are filtered by a security allowlist; dropped keys are reported in the output's 'note' field (escalated runs inherit the full user environment). " +
			"Commands run sandboxed without user approval; only danger-listed patterns and sandbox-escape grants ever prompt. " +
			"The sandbox denies outbound network/DNS, GUI opens, and writes outside the workspace and temp dir: grant the matching capability when a command needs it — needs_network, needs_gui_open or writable_paths (each a lightweight, rememberable approval) — and reserve sandbox_permissions='require_escalated' (with a justification) for failures none of those explain. " +
			"Never hand a sandbox-blocked command to the user before offering the matching approval.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"command":{"type":"string","minLength":1,"maxLength":32768,"description":"The shell command to execute, exactly as you would type it in a terminal (e.g. \"go test ./...\" or \"ls -la | head -20\"). Runs via 'sh -c', so pipes, redirection, '&&' chaining, globs and quoting all work."},"working_dir":{"type":"string","minLength":1,"maxLength":4096,"description":"Directory to run the command in, relative to the workspace root (default '.'). Set this instead of prefixing the command with 'cd ... &&'."},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192},"description":"Extra environment variables. Inside the sandbox they are filtered by a security allowlist; dropped keys are reported in the output's note field."},"timeout_ms":{"type":"integer","minimum":1,"maximum":600000,"description":"Kill the command after this many milliseconds (default 120000)."},"max_output_bytes":{"type":"integer","minimum":1,"maximum":1048576,"description":"Maximum bytes of stdout/stderr returned inline (default 65536); output beyond the limit is stored as an artifact with a head/tail preview."},"sandbox_permissions":{"type":"string","enum":["use_default","require_escalated"],"description":"'require_escalated' runs OUTSIDE the sandbox with the full user environment after explicit approval and requires a justification; use it only when needs_network/needs_gui_open/writable_paths cannot explain the failure (TTY needs, credential files the sandbox hides, Security-framework TLS); never combine it with the scoped fields."},"needs_network":{"type":"boolean","description":"Grant outbound network and DNS inside the sandbox after a lightweight approval (credentials stay unreadable); do not combine with require_escalated."},"needs_gui_open":{"type":"boolean","description":"Allow opening URLs/apps (macOS 'open', Apple Events) inside the sandbox after a lightweight approval; do not combine with require_escalated."},"writable_paths":{"type":"array","maxItems":8,"items":{"type":"string","minLength":1,"maxLength":4096},"description":"Extra absolute directories ('~/' expands) the command may write after a lightweight approval; use it for outside-workspace write targets that cannot be stated literally in the command (shell variables, command substitution) and come back denied — literal targets already get a scoped one-shot approval; never credential locations or their ancestors; do not combine with require_escalated."},"justification":{"type":"string","minLength":1,"maxLength":240,"description":"Short note shown to the user at approval time; required with sandbox_permissions='require_escalated', informational otherwise."}},"required":["command"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}

	signer, err := toolkit.NewSigner()
	if err != nil {
		return nil, err
	}
	return &RunCmdTool{
		def:              def,
		validator:        validator,
		runner:           runner,
		artifacts:        artifacts,
		modelOutputBytes: modelOutputBytes,
		signer:           signer,
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

	rawArgs, err := toolkit.DecodeStrict[rawRunCmdArgs](call.Arguments)
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
			Arguments: toolkit.CloneRawMessage(canonical),
		},
		Definition: t.def,
		Risk:       riskForArgs(args, t.def.Risk()),
		ReadPaths:  []string{root},
		WritePaths: []string{root},
		// The typed execution contract the policy layer classifies on
		// (REVIEW M17/A2); covered by the signature below.
		ExecRequest: &domain.ExecRequest{
			Argv:          []string{"sh", "-c", args.Command},
			Escalated:     args.SandboxPermissions == toolkit.SandboxRequireEscalated,
			NeedsNetwork:  args.NeedsNetwork,
			NeedsGUIOpen:  args.NeedsGUIOpen,
			WritablePaths: append([]string(nil), args.WritablePaths...),
		},
	}
	// Sign before rendering the description so the displayed args_hash
	// correlates with the ArgsHash recorded in permission events.
	prepared.ArgsHash = t.signPrepared(prepared)
	prepared.ApprovalDesc = buildApprovalDesc(args, prepared, t.validator.Root())
	return prepared, nil
}

// riskForArgs elevates to R3 when the call crosses the default sandbox's
// boundary: require_escalated runs outside it with full user privileges,
// and writable_paths widens its write boundary beyond the workspace (the
// path validator no longer bounds the blast radius, so policy — not the
// sandbox default — is the gate). Shell invocations keep the base risk:
// the sandbox confines them the same as plain argv, and the permission
// layer's danger screen analyzes the script AST (per-subcommand
// screening, pipe-into-shell and sensitive-redirect detection) —
// composition alone is not a risk.
func riskForArgs(args runCmdArgs, base domain.RiskLevel) domain.RiskLevel {
	if args.SandboxPermissions == toolkit.SandboxRequireEscalated || len(args.WritablePaths) > 0 {
		return domain.R3
	}
	return base
}

func (t *RunCmdTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if !hasOnlyWorkspaceRoot(prepared.ReadPaths, t.validator.Root()) || !hasOnlyWorkspaceRoot(prepared.WritePaths, t.validator.Root()) {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call workspace bindings are invalid"))
	}

	args, err := toolkit.DecodeStrict[runCmdArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	_, resolvedDir, err := validateCanonicalArgs(t.validator, args)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	stdoutStage, stderrStage, err := t.beginOutputArtifacts(ctx)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
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
	//
	// The grant is policy-produced and therefore trusted, but it is NOT
	// covered by the ArgsHash signature — re-validate the writable paths
	// against the sensitive-location boundary here so a misassembled or
	// hand-rolled verdict can never open a credential directory to writes
	// (defense in depth; the cheap check runs once per execution).
	for _, p := range prepared.Grant.WritablePaths {
		if workspacepkg.CoversSensitiveLocation(filepath.Clean(p)) {
			return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "grant writable path covers a credential location"))
		}
	}
	grant := process.Grant{
		Unsandboxed:   prepared.Grant.Unsandboxed,
		NetworkFull:   prepared.Grant.NetworkFull,
		WritablePaths: prepared.Grant.WritablePaths,
		GUIOpen:       prepared.Grant.GUIOpen,
	}
	runnerResult, err := t.runner.RunWithGrant(ctx, process.CommandSpec{
		Program:      "sh",
		Args:         []string{"-c", args.Command},
		Cwd:          resolvedDir.Absolute,
		Env:          cloneStringMap(args.Env),
		Timeout:      time.Duration(args.TimeoutMs) * time.Millisecond,
		OutputLimit:  max(int64(1), previewLimit/2),
		StdoutWriter: stdoutStage,
		StderrWriter: stderrStage,
	}, grant)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, classifyRunError(err))
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
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(
			domain.ErrUnavailable, "command completed but captured output could not be committed",
			domain.WithCause(err),
		))
	}
	payload := runCmdOutput{
		Stdout:                  toolkit.SanitizeUTF8(runnerResult.Stdout),
		Stderr:                  toolkit.SanitizeUTF8(runnerResult.Stderr),
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
			escalationDowngradeNote(args.SandboxPermissions == toolkit.SandboxRequireEscalated, prepared.Grant.Unsandboxed),
			commandNotFoundNote(
				string(runnerResult.Stderr),
				runnerResult.ExitCode,
				args.SandboxPermissions == toolkit.SandboxRequireEscalated,
				args.Env,
			),
			sandboxGuidanceNote(
				string(runnerResult.Stderr),
				runnerResult.TimedOut,
				args.SandboxPermissions == toolkit.SandboxRequireEscalated,
			),
			droppedEnvNote(runnerResult.DroppedEnvKeys),
		),
	}
	if err := boundCommandOutput(&payload, t.modelOutputBytes); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
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
	args, err := toolkit.DecodeStrict[runCmdArgs](prepared.Call.Arguments)
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
		Arguments:   toolkit.CloneRawMessage(prepared.Call.Arguments),
		ReadPaths:   append([]string(nil), prepared.ReadPaths...),
		WritePaths:  append([]string(nil), prepared.WritePaths...),
		Risk:        prepared.Risk,
		Definition:  prepared.Definition,
		ExecRequest: prepared.ExecRequest,
	}
	return t.signer.SignFingerprint(fingerprint)
}

// Default values applied when the model omits optional parameters, keeping
// run_cmd calls terse (only 'command' is required).
const (
	defaultTimeoutMs      int64 = 120000
	defaultMaxOutputBytes int64 = 64 << 10
)

func validateArgs(
	validator *workspacepkg.PathValidator,
	raw rawRunCmdArgs,
) (runCmdArgs, resolvedWorkingDir, error) {
	if raw.Command == nil || strings.TrimSpace(*raw.Command) == "" {
		return runCmdArgs{}, resolvedWorkingDir{}, toolkit.MissingCommandError("run_cmd")
	}

	args := runCmdArgs{
		Command:            strings.TrimSpace(*raw.Command),
		Env:                map[string]string{},
		TimeoutMs:          defaultTimeoutMs,
		MaxOutputBytes:     defaultMaxOutputBytes,
		SandboxPermissions: toolkit.SandboxUseDefault,
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
	if raw.WritablePaths != nil {
		args.WritablePaths = append([]string(nil), (*raw.WritablePaths)...)
	}
	if raw.Justification != nil {
		args.Justification = strings.TrimSpace(*raw.Justification)
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

	if raw.WorkingDir != nil {
		args.WorkingDir = *raw.WorkingDir
	}
	return validateCanonicalArgs(validator, args)
}

func validateCanonicalArgs(
	validator *workspacepkg.PathValidator,
	args runCmdArgs,
) (runCmdArgs, resolvedWorkingDir, error) {
	command, err := toolkit.ValidateCommandText("run_cmd", args.Command)
	if err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	args.Command = command
	absoluteDir, displayDir, err := toolkit.ResolveWorkingDir(validator, args.WorkingDir)
	if err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	resolvedDir := resolvedWorkingDir{Absolute: absoluteDir, Display: displayDir}
	args.WorkingDir = resolvedDir.Display
	if args.TimeoutMs < minTimeoutMs || args.TimeoutMs > maxTimeoutMs {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("timeout_ms must be between %d and %d", minTimeoutMs, maxTimeoutMs))
	}
	if args.MaxOutputBytes < 1 || args.MaxOutputBytes > maxOutputBytes {
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 1 and %d", maxOutputBytes))
	}
	if err := toolkit.ValidateEnv(args.Env); err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	args.Env = cloneStringMap(args.Env)

	writable, err := validateWritablePaths(args.WritablePaths)
	if err != nil {
		return runCmdArgs{}, resolvedWorkingDir{}, err
	}
	args.WritablePaths = writable

	switch args.SandboxPermissions {
	case toolkit.SandboxUseDefault:
		// A justification is accepted on any call: it is only an
		// informational note shown to the approver, with no effect on
		// privileges. Rejecting it for sandboxed calls taught models to
		// retry the same call in a loop (observed 11 consecutive
		// prepare_failed results in one session).
		if len(args.Justification) > maxJustificationBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("justification exceeds %d bytes", maxJustificationBytes))
		}
	case toolkit.SandboxRequireEscalated:
		if args.NeedsNetwork || args.NeedsGUIOpen || len(args.WritablePaths) > 0 {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "needs_network/needs_gui_open/writable_paths cannot be combined with sandbox_permissions=require_escalated (escalated runs already have full network, GUI and write access; use the scoped flags for the sandboxed path)")
		}
		if args.Justification == "" {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "justification is required with sandbox_permissions=require_escalated (ask the user a short yes/no question)")
		}
		if len(args.Justification) > maxJustificationBytes {
			return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("justification exceeds %d bytes", maxJustificationBytes))
		}
	default:
		return runCmdArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("sandbox_permissions must be %q or %q", toolkit.SandboxUseDefault, toolkit.SandboxRequireEscalated))
	}
	return args, resolvedDir, nil
}

// validateWritablePaths canonicalizes the model-declared writable paths
// and enforces the grant boundary: absolute (a leading "~/" expands
// against the user's home), never a sensitive location or one of its
// ancestors (granting "~" must not open ~/.ssh to a plain write), never
// the filesystem root. The result is sorted and deduplicated so the
// canonical arguments — and with them the ArgsHash signature — are
// deterministic across Prepare and Execute.
func validateWritablePaths(raw []string) ([]string, error) {
	if len(raw) == 0 {
		return nil, nil
	}
	if len(raw) > maxWritablePaths {
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("writable_paths exceeds %d entries", maxWritablePaths))
	}
	home, _ := os.UserHomeDir()
	seen := make(map[string]struct{}, len(raw))
	out := make([]string, 0, len(raw))
	for _, p := range raw {
		if len(p) == 0 || len(p) > toolkit.MaxWorkingDirBytes {
			return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("writable_paths entry is empty or exceeds %d bytes", toolkit.MaxWorkingDirBytes))
		}
		if strings.ContainsRune(p, 0) {
			return nil, domain.NewError(domain.ErrInvalidInput, "writable_paths entry contains null byte")
		}
		expanded := p
		if expanded == "~" {
			expanded = home
		} else if strings.HasPrefix(expanded, "~/") {
			expanded = filepath.Join(home, strings.TrimPrefix(expanded, "~/"))
		}
		if expanded != p && home == "" {
			return nil, domain.NewError(domain.ErrInvalidInput, "writable_paths cannot expand ~: home directory is unavailable")
		}
		if !filepath.IsAbs(expanded) {
			return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("writable_paths entry must be an absolute path: %q", domain.TruncateForErrorEcho(p)))
		}
		canonical := workspacepkg.Canonicalize(expanded)
		if canonical == string(filepath.Separator) {
			return nil, domain.NewError(domain.ErrSecurity, "writable_paths cannot name the filesystem root")
		}
		if workspacepkg.CoversSensitiveLocation(canonical) {
			return nil, domain.NewError(domain.ErrSecurity, fmt.Sprintf("writable_paths entry %q is or covers a credential location; such paths can never be granted — use sandbox_permissions='require_escalated' with a justification instead", domain.TruncateForErrorEcho(p)))
		}
		if _, dup := seen[canonical]; dup {
			continue
		}
		seen[canonical] = struct{}{}
		out = append(out, canonical)
	}
	sort.Strings(out)
	return out, nil
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
		"Instead: locate the binary and call it by its absolute path, or pass env={\"PATH\": \"<dir>:...\"} explicitly; " +
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
		"If it is caused by a denied WRITE OUTSIDE the workspace/temp dir (a tool dropping logs, config or state in its own directory), " +
		"PREFER retrying the SAME command with writable_paths=[that directory] — after a lightweight approval it runs INSIDE the sandbox with exactly those directories writable. " +
		"Only when neither scoped flag explains the failure (TTY, credential files like SSO tokens the sandbox hides), " +
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
	// macOS Security framework TLS fingerprints: runtimes that verify TLS
	// roots through the macOS Security framework (Go's crypto/x509 via
	// securityd; pip's vendored truststore) fail with this signature inside
	// the seatbelt sandbox even with needs_network granted — curl/python-
	// urllib/node/cargo/maven read file-based CA stores (/etc/ssl/cert.pem,
	// JDK cacerts) and are NOT affected. This is a known platform limitation
	// (Claude Code documents it and tells users to exclude such CLIs from
	// the sandbox); no file or mach rule opens it back up. The practical
	// escape is to use curl for the fetch (works fine sandboxed), or to
	// escalate the Go/Python command.
	securityFrameworkTLSPatterns := []string{
		"x509: osstatus", "x509: certificate signed by unknown authority",
		"certificate verification failed", "unable to get local issuer certificate",
		"osstatus", // pip/truststore surfaces bare OSStatus -26276 without the x509: prefix
	}
	for _, p := range securityFrameworkTLSPatterns {
		if strings.Contains(lower, p) {
			return "TLS certificate verification failed inside the macOS sandbox for a Security-framework-based runtime " +
				"(Go programs, pip with vendored truststore): these read trust roots through securityd/keychain, which the seatbelt sandbox denies — " +
				"this is a platform limitation, not a network grant issue (curl/python-urllib/node/cargo/maven read file-based CA stores and are NOT affected). " +
				"PREFER replacing the fetch with curl (e.g. 'curl -sSL <url>') which works sandboxed, or if the Go/Python program itself must run, " +
				"retry the SAME command with sandbox_permissions='require_escalated' and a short justification — after approval it runs OUTSIDE the sandbox with full user privileges. " +
				"Alternatively, for trusted high-frequency commands (go mod download, pip install, gh api), the user can enable the matching rule pack in Settings → 权限与审批 → 规则包, which pre-authorizes them without per-run approval."
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
		"read-only file system", "permission denied", // write outside workspace/temp dir
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
	if errors.Is(err, exec.ErrNotFound) {
		return domain.NewError(domain.ErrInvalidInput,
			"the 'sh' interpreter could not be resolved on PATH ("+os.Getenv("PATH")+"); "+
				"register its directory in tools.path_extra in the loom config",
			domain.WithCause(err))
	}
	if errors.Is(err, process.ErrInvalidCwd) {
		return domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	// Anything else — pipe setup, start, wait, output capture — is an
	// execution failure surfaced as-is (REVIEW A5: the previous substring
	// matches on "stdout pipe"/"start command"/"wait command" were pure
	// aliases of this same fallback).
	return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
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
		return toolkit.ErrorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
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

func sameDefinition(left, right domain.ToolDefinition) bool {
	if left.Name != right.Name || left.Description != right.Description || left.Source != right.Source {
		return false
	}
	if string(left.InputSchema) != string(right.InputSchema) {
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

func durationMilliseconds(d time.Duration) int64 {
	if d <= 0 {
		return 0
	}
	return d.Milliseconds()
}

func buildApprovalDesc(args runCmdArgs, prepared domain.PreparedCall, root string) string {
	parts := []string{"Run"}
	parts = append(parts, shellQuote(args.Command))

	envKeys := sortedEnvKeys(args.Env)
	if len(envKeys) > 0 {
		parts = append(parts, "env["+strings.Join(envKeys, ", ")+"]")
	} else {
		parts = append(parts, "env[none]")
	}
	parts = append(parts, "cwd="+shellQuote(args.WorkingDir))
	parts = append(parts, fmt.Sprintf("timeout=%dms", args.TimeoutMs))
	// Every call is a shell invocation now; the permission layer's danger
	// screen AST-parses the script, so the summary always carries the marker.
	parts = append(parts, "shell=parsed")
	if args.SandboxPermissions == toolkit.SandboxRequireEscalated {
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
	if len(args.WritablePaths) > 0 {
		parts = append(parts, "writable=["+strings.Join(args.WritablePaths, ", ")+"]")
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
	tokens := strings.FieldsFunc(args.Command, func(r rune) bool {
		switch r {
		case ' ', '\t', '\n', '"', '\'', ';', '|', '&', '(', ')', '<', '>', '=', ',', '`':
			return true
		}
		return false
	})
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
