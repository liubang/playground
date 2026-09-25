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
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// ExecSessionTool starts a long-running command as a background session,
// returning a session_id the model drives with write_stdin. The
// prepare/verify protocol is the shared toolkit.BaseTool skeleton.
type ExecSessionTool struct {
	base      toolkit.BaseTool
	validator *workspacepkg.PathValidator
	manager   *Manager
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
	def := domain.ToolDefinition{
		Name: "exec_session",
		// The sandbox policy itself is documented once on run_cmd; this
		// description carries only what is session-specific.
		Description: "Start a long-running command as a background session inside the sandbox and return a session_id " +
			"without waiting for it to finish: dev servers, watch-mode test runners, REPLs, database consoles; " +
			"for one-shot commands that run to completion, prefer run_cmd. " +
			"Runs via 'sh -c' like run_cmd (e.g. {\"command\":\"npm run dev\",\"working_dir\":\"web\"}); " +
			"run_cmd's sandbox rules and scoped flags apply — needs_gui_open covers a session opening URLs/apps, " +
			"sandbox_permissions='require_escalated' with a justification runs the session OUTSIDE the sandbox. " +
			"The call returns early with status='running' and the output produced so far; drive the session afterwards " +
			"with write_stdin (send input, or poll with empty chars). stdout and stderr are merged in arrival order. " +
			"Sessions are killed automatically after 30 minutes without write_stdin interaction — poll long-lived " +
			"services periodically if they must stay up.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"command":{"type":"string","minLength":1,"maxLength":32768,"description":"The shell command to run as a session, via 'sh -c'."},"working_dir":{"type":"string","minLength":1,"maxLength":4096,"default":".","description":"Run directory, relative to the workspace root."},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192},"description":"Extra environment variables (sandbox-filtered allowlist)."},"yield_time_ms":{"type":"integer","minimum":0,"maximum":300000,"default":1000,"description":"Milliseconds to wait for the first output before returning (0 returns immediately)."},"max_output_bytes":{"type":"integer","minimum":0,"maximum":65536,"default":16384,"description":"Maximum bytes of merged output returned."},"sandbox_permissions":{"type":"string","enum":["use_default","require_escalated"],"default":"use_default","description":"'require_escalated' runs the session OUTSIDE the sandbox after explicit approval; requires justification."},"needs_gui_open":{"type":"boolean","description":"Allow the session to open URLs/apps (macOS 'open', Apple Events) inside the sandbox after a lightweight approval."},"justification":{"type":"string","minLength":1,"maxLength":240,"description":"Short note shown at approval time; required with require_escalated."}},"required":["command"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}
	base, err := toolkit.NewBaseTool(def)
	if err != nil {
		return nil, err
	}
	return &ExecSessionTool{base: base, validator: validator, manager: manager}, nil
}

func (t *ExecSessionTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

func (t *ExecSessionTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[commandArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if _, err := validateCommandArgs(t.validator, &args); err != nil {
		return domain.PreparedCall{}, err
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	risk := riskForCommand(args, t.base.Def.Risk())
	prepared, err := t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{
		ReadPaths:  []string{t.validator.Root()},
		WritePaths: []string{t.validator.Root()},
		Risk:       &risk,
		// Same typed execution contract as run_cmd: argv rules, the
		// danger screen, and session memory apply to sessions too.
		ExecRequest: &domain.ExecRequest{
			Argv:         []string{"sh", "-c", args.Command},
			Escalated:    args.SandboxPermissions == toolkit.SandboxRequireEscalated,
			NeedsGUIOpen: args.NeedsGUIOpen,
		},
	})
	if err != nil {
		return domain.PreparedCall{}, err
	}
	approvalDesc := fmt.Sprintf("Start session %s; cwd=%s", args.Command, args.WorkingDir)
	if args.SandboxPermissions == toolkit.SandboxRequireEscalated {
		approvalDesc += "; ESCALATED(no-sandbox)[" + args.Justification + "]"
	}
	prepared.ApprovalDesc = approvalDesc
	return prepared, nil
}

func (t *ExecSessionTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCallStructural(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[commandArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	// The risk tier is derived from the signed arguments (escalated ⇒ R3),
	// not assumed from the definition default — same discipline as run_cmd.
	if prepared.Risk != riskForCommand(args, t.base.Def.Risk()) {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call risk mismatch"))
	}
	absoluteDir, err := validateCommandArgs(t.validator, &args)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	yieldMs := args.YieldTimeMs
	if yieldMs == 0 {
		yieldMs = defaultStartYieldMs
	}
	grant := process.Grant{
		Unsandboxed:   prepared.Grant.Unsandboxed,
		NetworkFull:   prepared.Grant.NetworkFull,
		WritablePaths: prepared.Grant.WritablePaths,
		GUIOpen:       prepared.Grant.GUIOpen,
	}
	entry, err := t.manager.Start(ctx, process.CommandSpec{
		Program: "sh",
		Args:    []string{"-c", args.Command},
		Cwd:     absoluteDir,
		Env:     args.Env,
	}, grant, args.Command)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, classifyStartError(err))
	}

	awaitYield(ctx, entry, yieldMs)
	output := drainSession(ctx, t.manager, entry, args.MaxOutputBytes)
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, output)
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
