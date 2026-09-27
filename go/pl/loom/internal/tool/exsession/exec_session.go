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

// Session actions of the exec_session tool.
const (
	actionStart = "start"
	actionWrite = "write"
	actionPoll  = "poll"
	actionKill  = "kill"
)

// sessionArgs is the model-visible schema of the unified exec_session
// tool: action selects the operation, the remaining fields are per-action
// (command/env/working_dir for start, session_id/chars for the rest).
type sessionArgs struct {
	Action             string            `json:"action,omitempty"`
	Command            string            `json:"command,omitempty"`
	SessionID          string            `json:"session_id,omitempty"`
	Chars              string            `json:"chars,omitempty"`
	WorkingDir         string            `json:"working_dir,omitempty"`
	Env                map[string]string `json:"env,omitempty"`
	YieldTimeMs        int64             `json:"yield_time_ms,omitempty"`
	MaxOutputBytes     int64             `json:"max_output_bytes,omitempty"`
	SandboxPermissions string            `json:"sandbox_permissions,omitempty"`
	NeedsGUIOpen       bool              `json:"needs_gui_open,omitempty"`
	Justification      string            `json:"justification,omitempty"`
}

// commandSpec extracts the start-action command fields into the shared
// commandArgs shape validateCommandArgs understands.
func (a sessionArgs) commandSpec() commandArgs {
	return commandArgs{
		Command:            a.Command,
		WorkingDir:         a.WorkingDir,
		Env:                a.Env,
		YieldTimeMs:        a.YieldTimeMs,
		MaxOutputBytes:     a.MaxOutputBytes,
		SandboxPermissions: a.SandboxPermissions,
		NeedsGUIOpen:       a.NeedsGUIOpen,
		Justification:      a.Justification,
	}
}

// ExecSessionTool manages long-running background sessions through one
// action-dispatching tool: start spawns a session, write feeds its stdin,
// poll reads incremental output, kill terminates it. The prepare/verify
// protocol is the shared toolkit.BaseTool skeleton; risk is graded per
// action (the browser pattern) because a start can escalate to R3 while
// write/poll/kill on an already-approved session are R1.
type ExecSessionTool struct {
	base      toolkit.BaseTool
	validator *workspacepkg.PathValidator
	manager   *Manager
}

// NewExecSessionTool creates the exec_session tool bound to the shared
// session manager.
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
		Description: "Manage long-running background sessions inside the sandbox: dev servers, watch-mode test " +
			"runners, REPLs, database consoles; for one-shot commands that run to completion, prefer run_cmd. " +
			"Actions: 'start' launches a command via 'sh -c' like run_cmd (e.g. {\"action\":\"start\",\"command\":\"npm run dev\",\"working_dir\":\"web\"}) " +
			"and returns a session_id early with status='running'; 'write' sends shell-style input to the session's stdin " +
			"(remember the trailing newline; control characters work too, e.g. '\\u0003' for Ctrl-C); 'poll' waits up to " +
			"yield_time_ms for new output (default 5000 when polling, 250 after a write); 'kill' terminates the session. " +
			"run_cmd's sandbox rules and scoped flags apply to start — needs_gui_open covers a session opening URLs/apps, " +
			"sandbox_permissions='require_escalated' with a justification runs the session OUTSIDE the sandbox. " +
			"Every call returns the merged stdout/stderr produced since the previous call, the status " +
			"('running', 'exited', or 'killed') and the exit code once finished. Keep polling a session you started " +
			"until it exits or kill it when no longer needed — sessions are killed automatically after 30 minutes " +
			"without interaction.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["start","write","poll","kill"],"description":"The session operation (default: inferred — command present means start, session_id with chars means write, session_id alone means poll)."},"command":{"type":"string","minLength":1,"maxLength":32768,"description":"The shell command to run as a session, via 'sh -c' (required for start)."},"session_id":{"type":"string","minLength":1,"maxLength":64,"description":"The session to drive (required for write/poll/kill)."},"chars":{"type":"string","maxLength":8192,"description":"Input to write to the session's stdin (write only)."},"working_dir":{"type":"string","minLength":1,"maxLength":4096,"default":".","description":"Run directory, relative to the workspace root (start only)."},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192},"description":"Extra environment variables (sandbox-filtered allowlist; start only)."},"yield_time_ms":{"type":"integer","minimum":0,"maximum":300000,"description":"Milliseconds to wait for output before returning (start default 1000, write 250, poll 5000)."},"max_output_bytes":{"type":"integer","minimum":0,"maximum":65536,"default":16384,"description":"Maximum bytes of merged output returned."},"sandbox_permissions":{"type":"string","enum":["use_default","require_escalated"],"default":"use_default","description":"'require_escalated' runs the session OUTSIDE the sandbox after explicit approval; requires justification (start only)."},"needs_gui_open":{"type":"boolean","description":"Allow the session to open URLs/apps (macOS 'open', Apple Events) inside the sandbox after a lightweight approval (start only)."},"justification":{"type":"string","minLength":1,"maxLength":240,"description":"Short note shown at approval time; required with require_escalated."}},"required":[]}`),
		// Deliberately capability-free (static R0): risk is graded PER
		// ACTION in Prepare (start is R2, R3 when escalated; write/poll/kill
		// are R1) — and the loop's execution-time drift guard rejects a
		// prepared risk BELOW the definition's static tier. The per-action
		// elevation is covered by the prepared-call signature, the same
		// shape as the browser tool's riskForAction. Source=builtin is the
		// audit marker.
		Source: domain.ToolSourceBuiltin,
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

// resolveAction fills in the action when the model omitted it: a command
// means start, a session_id with chars means write, a session_id alone
// means poll.
func resolveAction(args *sessionArgs) error {
	if args.Action == "" {
		switch {
		case args.Command != "":
			args.Action = actionStart
		case args.SessionID != "" && args.Chars != "":
			args.Action = actionWrite
		case args.SessionID != "":
			args.Action = actionPoll
		default:
			return domain.NewError(domain.ErrInvalidInput, "action is required (start|write|poll|kill)")
		}
	}
	switch args.Action {
	case actionStart:
		if args.Command == "" {
			return domain.NewError(domain.ErrInvalidInput, "command is required for action=start")
		}
	case actionWrite, actionPoll, actionKill:
		if args.SessionID == "" {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("session_id is required for action=%s", args.Action))
		}
		if len(args.Chars) > maxCharsBytes {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("chars exceeds %d bytes", maxCharsBytes))
		}
		if args.Action == actionPoll && args.Chars != "" {
			return domain.NewError(domain.ErrInvalidInput, "chars is not allowed for action=poll (use action=write)")
		}
		if args.YieldTimeMs < 0 || args.YieldTimeMs > maxYieldMs {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("yield_time_ms must be between 0 and %d", maxYieldMs))
		}
		if args.MaxOutputBytes < 0 || args.MaxOutputBytes > maxMaxOutputBytes {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 0 and %d", maxMaxOutputBytes))
		}
	default:
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unknown action %q (want start|write|poll|kill)", args.Action))
	}
	return nil
}

// riskForArgs grades the call's risk by action: only start spawns a new
// process (R2 inside the sandbox, R3 when escalated); write/poll/kill
// drive an already-approved session and stay at R1.
func riskForArgs(args sessionArgs) domain.RiskLevel {
	if args.Action == actionStart {
		return riskForCommand(args.commandSpec(), domain.R2)
	}
	return domain.R1
}

func (t *ExecSessionTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[sessionArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if err := resolveAction(&args); err != nil {
		return domain.PreparedCall{}, err
	}

	if args.Action == actionStart {
		cmd := args.commandSpec()
		if _, err = validateCommandArgs(t.validator, &cmd); err != nil {
			return domain.PreparedCall{}, err
		}
		// Fold the normalized command fields back into the canonical args.
		args.Command, args.WorkingDir = cmd.Command, cmd.WorkingDir
		args.SandboxPermissions, args.Justification = cmd.SandboxPermissions, cmd.Justification
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	risk := riskForArgs(args)
	opts := toolkit.PrepareOptions{Risk: &risk}
	if args.Action == actionStart {
		opts.ReadPaths = []string{t.validator.Root()}
		opts.WritePaths = []string{t.validator.Root()}
		// Same typed execution contract as run_cmd: argv rules, the
		// danger screen, and session memory apply to sessions too.
		opts.ExecRequest = &domain.ExecRequest{
			Argv:         []string{"sh", "-c", args.Command},
			Escalated:    args.SandboxPermissions == toolkit.SandboxRequireEscalated,
			NeedsGUIOpen: args.NeedsGUIOpen,
		}
	}
	prepared, err := t.base.PrepareCall(ctx, call, canonical, opts)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	prepared.ApprovalDesc = approvalDescFor(args)
	return prepared, nil
}

func approvalDescFor(args sessionArgs) string {
	switch args.Action {
	case actionStart:
		desc := fmt.Sprintf("Start session %s; cwd=%s", args.Command, args.WorkingDir)
		if args.SandboxPermissions == toolkit.SandboxRequireEscalated {
			desc += "; ESCALATED(no-sandbox)[" + args.Justification + "]"
		}
		return desc
	case actionWrite:
		return fmt.Sprintf("Write %d bytes to session %s", len(args.Chars), args.SessionID)
	case actionPoll:
		return fmt.Sprintf("Poll session %s", args.SessionID)
	default:
		return fmt.Sprintf("Kill session %s", args.SessionID)
	}
}

func (t *ExecSessionTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCallStructural(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[sessionArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	// The risk tier is derived from the signed arguments (start ⇒ R2/R3,
	// the rest ⇒ R1), not assumed from the definition default — same
	// discipline as run_cmd.
	if err := resolveAction(&args); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if prepared.Risk != riskForArgs(args) {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call risk mismatch"))
	}

	switch args.Action {
	case actionStart:
		return t.executeStart(ctx, prepared, args, startedAt)
	case actionKill:
		return t.executeKill(ctx, prepared, args, startedAt)
	default:
		return t.executeDrive(ctx, prepared, args, startedAt)
	}
}

func (t *ExecSessionTool) executeStart(ctx context.Context, prepared domain.PreparedCall, args sessionArgs, startedAt time.Time) domain.ToolResult {
	cmd := args.commandSpec()
	absoluteDir, err := validateCommandArgs(t.validator, &cmd)
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
		Args:    []string{"-c", cmd.Command},
		Cwd:     absoluteDir,
		Env:     cmd.Env,
	}, grant, cmd.Command)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, classifyStartError(err))
	}

	awaitYield(ctx, entry, yieldMs)
	output := drainSession(ctx, t.manager, entry, args.MaxOutputBytes)
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, output)
}

// executeDrive implements write and poll: feed optional input, wait the
// yield budget, drain the incremental output.
func (t *ExecSessionTool) executeDrive(ctx context.Context, prepared domain.PreparedCall, args sessionArgs, startedAt time.Time) domain.ToolResult {
	entry, ok := t.manager.Get(args.SessionID)
	if !ok {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, unknownSessionError(args.SessionID))
	}
	if args.Chars != "" {
		if err := entry.session.Write(args.Chars); err != nil {
			return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "cannot write to session", domain.WithCause(err)))
		}
	}

	yieldMs := args.YieldTimeMs
	if yieldMs == 0 {
		if args.Chars == "" {
			yieldMs = defaultPollYieldMs
		} else {
			yieldMs = defaultWriteYieldMs
		}
	}
	awaitYield(ctx, entry, yieldMs)
	output := drainSession(ctx, t.manager, entry, args.MaxOutputBytes)
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, output)
}

func (t *ExecSessionTool) executeKill(ctx context.Context, prepared domain.PreparedCall, args sessionArgs, startedAt time.Time) domain.ToolResult {
	entry, ok := t.manager.Kill(args.SessionID)
	if !ok {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, unknownSessionError(args.SessionID))
	}
	// Give the process group a moment to reap so the drain reports the
	// final status instead of a still-running snapshot.
	yieldMs := args.YieldTimeMs
	if yieldMs == 0 {
		yieldMs = defaultStartYieldMs
	}
	awaitYield(ctx, entry, yieldMs)
	output := drainSession(ctx, t.manager, entry, args.MaxOutputBytes)
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, output)
}

func unknownSessionError(sessionID string) error {
	return domain.NewError(
		domain.ErrInvalidInput,
		fmt.Sprintf("unknown session %q: it never existed or was reaped after 30 minutes idle; start a new one with action=start", sessionID),
	)
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
