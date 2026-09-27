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
// Created: 2026/09/27

package subagent

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// CancelSubagentTool stops a running sub-agent. This is the companion to
// delegate_task with async=true: when a delegated task is no longer
// needed — or has gone off track — cancelling stops its token burn
// against the parent run's budget. A cancelled sub-agent keeps its
// persisted history and can be continued with resume_subagent.
type CancelSubagentTool struct {
	def domain.ToolDefinition
	m   *Manager
	key toolkit.Signer
}

// NewCancelSubagentTool creates the tool bound to the given manager.
func NewCancelSubagentTool(m *Manager) (*CancelSubagentTool, error) {
	if m == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "cancel_subagent requires a non-nil manager")
	}
	def := domain.ToolDefinition{
		Name: "cancel_subagent",
		Description: "Cancel a running sub-agent started by delegate_task with async=true. " +
			"Use this when the delegated task is no longer needed or has gone off track: the sub-agent " +
			"stops consuming this run's token budget, and its partial work stays persisted — resume it " +
			"later with resume_subagent if needed. Cancelling an agent that already finished reports " +
			"status=already_done with its conclusion instead of failing.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"child_session_id":{"type":"string","description":"The session ID returned by the async delegate_task call."}},"required":["child_session_id"]}`),
		Source:      domain.ToolSourceSubAgent,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	key, err := toolkit.NewSigner()
	if err != nil {
		return nil, err
	}
	return &CancelSubagentTool{def: def, m: m, key: key}, nil
}

// Definition returns the tool definition.
func (t *CancelSubagentTool) Definition() domain.ToolDefinition { return t.def }

// ConcurrentSafe implements domain.ConcurrentSafely: cancelling different
// sub-agents is safe to do in parallel.
func (t *CancelSubagentTool) ConcurrentSafe() bool { return true }

// Prepare validates and canonicalizes the call; it is side-effect-free.
func (t *CancelSubagentTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	args, err := toolkit.DecodeLenient[cancelArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid cancel_subagent arguments", domain.WithCause(err))
	}
	sessionID, err := domain.ParseSessionID(args.ChildSessionID)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid child_session_id", domain.WithCause(err))
	}
	args.ChildSessionID = sessionID.String()
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	call.Arguments = canonical
	desc := fmt.Sprintf("Cancel sub-agent %s", sessionID.String())
	prepared := domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: desc,
	}
	prepared.ArgsHash = signPreparedCall(&t.key, prepared)
	return prepared, nil
}

// Execute cancels the sub-agent and reports its terminal state.
func (t *CancelSubagentTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := verifyPreparedCall(&t.key, t.def, domain.R1, prepared); err != nil {
		return waitError(prepared.Call.ID, startedAt, err)
	}
	var args cancelArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return waitError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid arguments", domain.WithCause(err)))
	}
	sessionID, err := domain.ParseSessionID(args.ChildSessionID)
	if err != nil {
		return waitError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid child_session_id", domain.WithCause(err)))
	}

	result, cancelErr := t.m.Cancel(ctx, sessionID)
	status := "cancelled"
	if cancelErr != nil {
		// Already finished (or not running in this process) is not a
		// fatal error — report the durable terminal state.
		var agentErr *domain.AgentError
		if errors.As(cancelErr, &agentErr) && agentErr.Code == domain.ErrConflict {
			status = "already_done"
		} else {
			return waitError(prepared.Call.ID, startedAt, cancelErr)
		}
	}

	payload := map[string]any{
		"child_session_id": result.SessionID.String(),
		"role":             string(result.Role),
		"outcome":          string(result.Outcome),
		"status":           status,
		"conclusion":       result.Conclusion,
		"usage": map[string]any{
			"input_tokens":  result.Usage.InputTokens,
			"output_tokens": result.Usage.OutputTokens,
			"turns":         result.Usage.Turns,
			"tool_calls":    result.Usage.ToolCalls,
		},
	}
	tr := marshalWaitResult(prepared.Call.ID, startedAt, payload)
	// Fold external usage into metadata so the parent loop accounts for it.
	if result.Usage.InputTokens > 0 || result.Usage.OutputTokens > 0 {
		if tr.Metadata == nil {
			tr.Metadata = make(map[string]string)
		}
		tr.Metadata[domain.ToolMetaExternalInputTokens] = strconv.FormatInt(result.Usage.InputTokens, 10)
		tr.Metadata[domain.ToolMetaExternalOutputTokens] = strconv.FormatInt(result.Usage.OutputTokens, 10)
	}
	return tr
}

type cancelArgs struct {
	ChildSessionID string `json:"child_session_id"`
}
