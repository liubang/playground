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
// Created: 2026/08/02

package subagent

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// WaitSubagentTool blocks until a previously spawned sub-agent reaches a
// terminal state and returns its conclusion. This is the companion to
// delegate_task with async=true: spawn returns a session reference,
// wait collects the result.
type WaitSubagentTool struct {
	def domain.ToolDefinition
	m   *Manager
	key toolkit.Signer
}

// NewWaitSubagentTool creates the tool bound to the given manager.
func NewWaitSubagentTool(m *Manager) (*WaitSubagentTool, error) {
	if m == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "wait_subagent requires a non-nil manager")
	}
	def := domain.ToolDefinition{
		Name: "wait_subagent",
		Description: "Wait for an asynchronously spawned sub-agent to finish and return its conclusion. " +
			"Use this after delegate_task with async=true returns a child_session_id. " +
			"If the sub-agent is still running, this blocks until it completes (up to the optional timeout). " +
			"If the sub-agent already finished, it returns the result immediately.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"child_session_id":{"type":"string","description":"The session ID returned by the async delegate_task call."},"timeout_seconds":{"type":"integer","minimum":1,"maximum":600,"description":"Optional maximum wait time in seconds. If the sub-agent is still running after this time, returns a timeout status. Omit to wait indefinitely."}},"required":["child_session_id"]}`),
		Source:      domain.ToolSourceSubAgent,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	key, err := toolkit.NewSigner()
	if err != nil {
		return nil, err
	}
	return &WaitSubagentTool{def: def, m: m, key: key}, nil
}

// Definition returns the tool definition.
func (t *WaitSubagentTool) Definition() domain.ToolDefinition { return t.def }

// ConcurrentSafe implements domain.ConcurrentSafely: waiting on different
// sub-agents is safe to do in parallel.
func (t *WaitSubagentTool) ConcurrentSafe() bool { return true }

// Prepare validates and canonicalizes the call; it is side-effect-free.
func (t *WaitSubagentTool) Prepare(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	var args waitArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid wait_subagent arguments", domain.WithCause(err))
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
	desc := fmt.Sprintf("Wait for sub-agent %s", sessionID.String())
	prepared := domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         domain.R1,
		ApprovalDesc: desc,
	}
	prepared.ArgsHash = signPreparedCall(&t.key, prepared)
	return prepared, nil
}

// Execute waits for the sub-agent to reach a terminal state.
func (t *WaitSubagentTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := verifyPreparedCall(&t.key, t.def, domain.R1, prepared); err != nil {
		return waitError(prepared.Call.ID, startedAt, err)
	}
	var args waitArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return waitError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid arguments", domain.WithCause(err)))
	}
	sessionID, err := domain.ParseSessionID(args.ChildSessionID)
	if err != nil {
		return waitError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid child_session_id", domain.WithCause(err)))
	}

	var timeout time.Duration
	if args.TimeoutSeconds > 0 {
		timeout = time.Duration(args.TimeoutSeconds) * time.Second
	}

	result, waitErr := t.m.Wait(ctx, sessionID, timeout)
	if waitErr != nil {
		// Timeout is not a fatal error — the agent is still running.
		var agentErr *domain.AgentError
		if errors.As(waitErr, &agentErr) {
			switch agentErr.Code {
			case domain.ErrTimeout:
				payload := map[string]any{
					"child_session_id": sessionID.String(),
					"status":           "timeout",
					"message":          waitErr.Error(),
				}
				return marshalWaitResult(prepared.Call.ID, startedAt, payload)
			case domain.ErrConflict:
				payload := map[string]any{
					"child_session_id": sessionID.String(),
					"status":           "resumable",
					"message":          waitErr.Error(),
				}
				return marshalWaitResult(prepared.Call.ID, startedAt, payload)
			}
		}
		return waitError(prepared.Call.ID, startedAt, waitErr)
	}

	payload := map[string]any{
		"child_session_id": result.SessionID.String(),
		"role":             string(result.Role),
		"outcome":          string(result.Outcome),
		"status":           "completed",
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

type waitArgs struct {
	ChildSessionID string `json:"child_session_id"`
	TimeoutSeconds int    `json:"timeout_seconds"`
}

func waitError(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	var agentErr *domain.AgentError
	code, message := string(domain.ErrInternal), err.Error()
	if errors.As(err, &agentErr) {
		code, message = string(agentErr.Code), agentErr.Message
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusError,
		Error:      &domain.ToolError{Code: code, Message: message, Retryable: true},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

func marshalWaitResult(callID domain.ToolCallID, startedAt time.Time, payload map[string]any) domain.ToolResult {
	raw, err := json.Marshal(payload)
	if err != nil {
		return domain.ToolResult{
			CallID:     callID,
			Status:     domain.ToolStatusError,
			Error:      &domain.ToolError{Code: "internal", Message: "failed to encode result", Retryable: false},
			StartedAt:  startedAt,
			FinishedAt: time.Now(),
		}
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
