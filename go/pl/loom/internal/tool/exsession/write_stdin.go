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
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// writeStdinArgs is the model-visible schema of write_stdin.
type writeStdinArgs struct {
	SessionID      string `json:"session_id"`
	Chars          string `json:"chars,omitempty"`
	YieldTimeMs    int64  `json:"yield_time_ms,omitempty"`
	MaxOutputBytes int64  `json:"max_output_bytes,omitempty"`
}

const maxCharsBytes = 8192

// WriteStdinTool feeds input to a running exec session and polls its output.
type WriteStdinTool struct {
	def     domain.ToolDefinition
	manager *Manager
	signer  signer
}

// NewWriteStdinTool creates the write_stdin tool bound to the shared
// session manager.
func NewWriteStdinTool(manager *Manager) (*WriteStdinTool, error) {
	if manager == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "session manager is required")
	}
	s, err := newSigner()
	if err != nil {
		return nil, err
	}
	def := domain.ToolDefinition{
		Name: "write_stdin",
		Description: "Write characters to a running exec_session's stdin and return the output produced since the last " +
			"call. Send shell-style input (remember the trailing newline); control characters work too (e.g. '\\u0003' " +
			"for Ctrl-C). With empty chars the call is a pure poll that waits up to yield_time_ms for new output " +
			"(default 5000 when polling, 250 after a write; max 300000). " +
			"The result reports status ('running', 'exited', or 'killed'), the exit code once finished, and the merged " +
			"stdout/stderr produced since the previous call. Keep polling a session you started until it exits or is no " +
			"longer needed — it stays alive otherwise.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"session_id":{"type":"string","minLength":1,"maxLength":64},"chars":{"type":"string","maxLength":8192},"yield_time_ms":{"type":"integer","minimum":0,"maximum":300000},"max_output_bytes":{"type":"integer","minimum":0,"maximum":65536}},"required":["session_id"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"session_id":{"type":"string"},"command":{"type":"string"},"status":{"type":"string","enum":["running","exited","killed"]},"exit_code":{"type":"integer"},"signal":{"type":"string"},"output":{"type":"string"},"output_dropped_bytes":{"type":"integer"},"stdout_bytes":{"type":"integer"},"stderr_bytes":{"type":"integer"},"duration_ms":{"type":"integer"},"isolation":{"type":"string"},"stdout_artifact_path":{"type":"string"},"stderr_artifact_path":{"type":"string"},"note":{"type":"string"}},"required":["session_id","command","status","exit_code","output","stdout_bytes","stderr_bytes","duration_ms","isolation"]}`),
		// Deliberately capability-free (static R0): Prepare pins the
		// per-call risk at R1, which is then an elevation above the
		// definition default. The agent loop's prepared-call drift check
		// fails closed on any tier below the definition default
		// (agent.validatePreparedExecution), so declaring CapProcessExec
		// here (static R2) would reject every write_stdin call.
		Source: domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	return &WriteStdinTool{def: def, manager: manager, signer: s}, nil
}

func (t *WriteStdinTool) Definition() domain.ToolDefinition {
	return t.def
}

func (t *WriteStdinTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	args, err := toolkit.DecodeStrict[writeStdinArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if args.SessionID == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "session_id is required")
	}
	if len(args.Chars) > maxCharsBytes {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("chars exceeds %d bytes", maxCharsBytes))
	}
	if args.YieldTimeMs < 0 || args.YieldTimeMs > maxYieldMs {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("yield_time_ms must be between 0 and %d", maxYieldMs))
	}
	if args.MaxOutputBytes < 0 || args.MaxOutputBytes > maxMaxOutputBytes {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 0 and %d", maxMaxOutputBytes))
	}
	canonicalBytes, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	// Keep the RawMessage type: see exec_session for why a plain []byte
	// would break signature symmetry.
	canonical := json.RawMessage(canonicalBytes)

	// write_stdin starts no new process: the command it drives was approved
	// at exec_session time, so the call itself is low risk.
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: canonical,
		},
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: fmt.Sprintf("Write %d bytes to session %s", len(args.Chars), args.SessionID),
	}
	prepared.ArgsHash = t.signer.sign(prepared.Call.ID.String(), t.def.Name, canonical, domain.R1)
	return prepared, nil
}

func (t *WriteStdinTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := toolkit.DecodeStrict[writeStdinArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	entry, ok := t.manager.Get(args.SessionID)
	if !ok {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(
			domain.ErrInvalidInput,
			fmt.Sprintf("unknown session %q: it never existed or was reaped after 30 minutes idle; start a new one with exec_session", args.SessionID),
		))
	}
	if err := entry.session.Write(args.Chars); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "cannot write to session", domain.WithCause(err)))
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

func (t *WriteStdinTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != t.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Name != t.def.Name || prepared.Definition.Source != t.def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	if prepared.Risk != domain.R1 {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	expected := t.signer.sign(prepared.Call.ID.String(), t.def.Name, prepared.Call.Arguments, domain.R1)
	if !t.signer.verify(expected, prepared.ArgsHash) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}
