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
	"errors"
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// ExecSessionTool starts a long-running command as a background session,
// returning a session_id the model drives with write_stdin.
type ExecSessionTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	manager   *Manager
	signer    signer
}

// NewExecSessionTool creates the exec_session tool bound to the session
// manager shared with write_stdin.
func NewExecSessionTool(validator *workspacepkg.PathValidator, manager *Manager) (*ExecSessionTool, error) {
	if validator == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if manager == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "session manager is required")
	}
	s, err := newSigner()
	if err != nil {
		return nil, err
	}
	def := domain.ToolDefinition{
		Name: "exec_session",
		Description: "Start a long-running command as a background session inside the sandbox and return a session_id, " +
			"without waiting for it to finish. Use it for processes that stay alive or need interaction: dev servers, " +
			"watch-mode test runners, REPLs, database consoles. For one-shot commands that run to completion, prefer run_cmd. " +
			"Always set working_dir explicitly instead of wrapping the command in 'cd ... &&'. " +
			"Only 'program' is required: working_dir defaults to '.', yield_time_ms to 1000 (how long to wait for the first " +
			"output before returning; 0 returns immediately, max 300000), max_output_bytes to 16384. " +
			"The call returns early with status='running' and the output produced so far; drive the session afterwards with " +
			"write_stdin (send input, or poll with empty chars). stdout and stderr are merged in arrival order like a terminal. " +
			"The sandbox denies outbound network (loopback allowed) and writes outside the workspace and temp dir; " +
			"sandbox_permissions='require_escalated' (with a short justification question) runs the session OUTSIDE the " +
			"sandbox with the full user environment after explicit approval. " +
			"Sessions are killed automatically after 30 minutes without any write_stdin interaction — poll long-lived " +
			"services periodically if they must stay up.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"program":{"type":"string","minLength":1,"maxLength":4096},"args":{"type":"array","maxItems":256,"items":{"type":"string","maxLength":8192}},"working_dir":{"type":"string","minLength":1,"maxLength":4096},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192}},"yield_time_ms":{"type":"integer","minimum":0,"maximum":300000},"max_output_bytes":{"type":"integer","minimum":0,"maximum":65536},"sandbox_permissions":{"type":"string","enum":["use_default","require_escalated"]},"justification":{"type":"string","minLength":1,"maxLength":240}},"required":["program"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"session_id":{"type":"string"},"command":{"type":"string"},"status":{"type":"string","enum":["running","exited","killed"]},"exit_code":{"type":"integer"},"signal":{"type":"string"},"output":{"type":"string"},"output_dropped_bytes":{"type":"integer"},"stdout_bytes":{"type":"integer"},"stderr_bytes":{"type":"integer"},"duration_ms":{"type":"integer"},"isolation":{"type":"string"},"stdout_artifact_path":{"type":"string"},"stderr_artifact_path":{"type":"string"},"note":{"type":"string"}},"required":["session_id","command","status","exit_code","output","stdout_bytes","stderr_bytes","duration_ms","isolation"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	return &ExecSessionTool{def: def, validator: validator, manager: manager, signer: s}, nil
}

func (t *ExecSessionTool) Definition() domain.ToolDefinition {
	return t.def
}

func (t *ExecSessionTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	args, err := decodeStrict[commandArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if _, err := validateCommandArgs(t.validator, &args); err != nil {
		return domain.PreparedCall{}, err
	}
	canonicalBytes, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	// Keep the RawMessage type: a plain []byte would be base64-encoded by
	// json.Marshal inside the signature payload, breaking Prepare/Execute
	// signature symmetry.
	canonical := json.RawMessage(canonicalBytes)

	risk := riskForCommand(args, t.def.Risk())
	approvalDesc := fmt.Sprintf("Start session %s; cwd=%s", displayArgv(args.Program, args.Args), args.WorkingDir)
	if args.SandboxPermissions == sandboxRequireEscalated {
		approvalDesc += "; ESCALATED(no-sandbox)[" + args.Justification + "]"
	}
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: canonical,
		},
		Definition:   t.def,
		Risk:         risk,
		ApprovalDesc: approvalDesc,
		ReadPaths:    []string{t.validator.Root()},
		WritePaths:   []string{t.validator.Root()},
		// Same typed execution contract as run_cmd: argv rules, the
		// danger screen, and session memory apply to sessions too.
		ExecRequest: &domain.ExecRequest{
			Argv:      append([]string{args.Program}, args.Args...),
			Escalated: args.SandboxPermissions == sandboxRequireEscalated,
		},
	}
	prepared.ArgsHash = t.signer.sign(prepared.Call.ID.String(), t.def.Name, canonical, prepared.ReadPaths, prepared.WritePaths, risk, prepared.ExecRequest)
	return prepared, nil
}

func (t *ExecSessionTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := decodeStrict[commandArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	absoluteDir, err := validateCommandArgs(t.validator, &args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	yieldMs := args.YieldTimeMs
	if yieldMs == 0 {
		yieldMs = defaultStartYieldMs
	}
	grant := process.Grant{
		Unsandboxed:   prepared.Grant.Unsandboxed,
		NetworkFull:   prepared.Grant.NetworkFull,
		WritablePaths: prepared.Grant.WritablePaths,
	}
	entry, err := t.manager.Start(ctx, process.CommandSpec{
		Program: args.Program,
		Args:    append([]string(nil), args.Args...),
		Cwd:     absoluteDir,
		Env:     args.Env,
	}, grant, displayArgv(args.Program, args.Args))
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyStartError(err))
	}

	awaitYield(ctx, entry, yieldMs)
	output := drainSession(ctx, t.manager, entry, args.MaxOutputBytes)
	return successResult(prepared.Call.ID, startedAt, output)
}

func (t *ExecSessionTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != t.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Name != t.def.Name || prepared.Definition.Source != t.def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	args, err := decodeStrict[commandArgs](prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments are unreadable")
	}
	if prepared.Risk != riskForCommand(args, t.def.Risk()) {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	expected := t.signer.sign(prepared.Call.ID.String(), t.def.Name, prepared.Call.Arguments, prepared.ReadPaths, prepared.WritePaths, prepared.Risk, prepared.ExecRequest)
	if !t.signer.verify(expected, prepared.ArgsHash) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func classifyStartError(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, process.ErrSandboxRequired), errors.Is(err, process.ErrSandboxUnavailable):
		return domain.NewError(domain.ErrUnavailable, "process sandbox is unavailable", domain.WithCause(err))
	case errors.Is(err, context.Canceled):
		return domain.NewError(domain.ErrCancelled, "operation cancelled", domain.WithCause(err))
	default:
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			return err
		}
		return domain.NewError(domain.ErrUnavailable, "session start failed", domain.WithCause(err))
	}
}
