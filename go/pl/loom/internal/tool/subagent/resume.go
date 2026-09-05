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
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// ResumeSubagentTool restarts a terminal (or crash-interrupted) sub-agent
// with a new follow-up task. The recovered session retains its event
// history; the new task becomes a fresh user message appended to the
// transcript.
type ResumeSubagentTool struct {
	def domain.ToolDefinition
	m   *Manager
	key toolkit.Signer
}

// NewResumeSubagentTool creates the tool bound to the given manager.
func NewResumeSubagentTool(m *Manager) (*ResumeSubagentTool, error) {
	if m == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "resume_subagent requires a non-nil manager")
	}
	def := domain.ToolDefinition{
		Name: "resume_subagent",
		Description: "Resume a finished or crash-interrupted sub-agent with a new follow-up task. " +
			"The sub-agent retains its prior event history and workspace context; the new task is appended " +
			"as a fresh user message. Use this to refine a sub-agent's work, continue an interrupted session, " +
			"or iterate on a partially completed task.",
		InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"child_session_id":{"type":"string","description":"The session ID of the sub-agent to resume."},"task":{"type":"string","minLength":1,"maxLength":16384,"description":"Follow-up task description with all necessary context."},"focus":{"type":"array","items":{"type":"string","maxLength":512},"maxItems":16,"description":"Optional paths or symbols to prioritize."},"role":{"type":"string","enum":["researcher","coder"],"description":"Override the sub-agent's role for this resume. Default: reuse the original role."}},"required":["child_session_id","task"]}`),
		Source:      domain.ToolSourceSubAgent,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid tool definition", domain.WithCause(err))
	}
	key, err := toolkit.NewSigner()
	if err != nil {
		return nil, err
	}
	return &ResumeSubagentTool{def: def, m: m, key: key}, nil
}

// Definition returns the tool definition.
func (t *ResumeSubagentTool) Definition() domain.ToolDefinition { return t.def }

// ConcurrentSafe implements domain.ConcurrentSafely: resuming different
// sub-agents is safe to do in parallel.
func (t *ResumeSubagentTool) ConcurrentSafe() bool { return true }

// Prepare validates and canonicalizes the call. It performs one read-only
// store lookup: when the caller does not name a role, the resume reuses
// the role the session was spawned with, so the approval risk must be
// computed from the PERSISTED role — otherwise resuming a coder agent
// would acquire read-write capability under an R1 (read-only) approval.
// The lookup is best-effort: a session that cannot be inspected fails at
// Execute with the real error, and the risk stays at the researcher tier.
func (t *ResumeSubagentTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	var args resumeArgs
	dec := json.NewDecoder(bytes.NewReader(call.Arguments))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&args); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid resume_subagent arguments", domain.WithCause(err))
	}
	sessionID, err := domain.ParseSessionID(args.ChildSessionID)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid child_session_id", domain.WithCause(err))
	}
	args.Task = trimAndValidateTask(args.Task)
	if args.Task == "" {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "task is required")
	}
	role, err := ParseRole(args.Role)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if args.Role == "" {
		role = t.persistedRole(ctx, sessionID, role)
	}
	risk := riskOf(role)
	args.ChildSessionID = sessionID.String()
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	call.Arguments = canonical
	desc := fmt.Sprintf("Resume sub-agent %s (%s)", sessionID.String(), role)
	prepared := domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         risk,
		ApprovalDesc: desc,
	}
	prepared.ArgsHash = signPreparedCall(&t.key, prepared)
	return prepared, nil
}

// Execute resumes the sub-agent with a new task.
func (t *ResumeSubagentTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	var args resumeArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		return resumeError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid arguments", domain.WithCause(err)))
	}
	sessionID, err := domain.ParseSessionID(args.ChildSessionID)
	if err != nil {
		return resumeError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid child_session_id", domain.WithCause(err)))
	}
	role, _ := ParseRole(args.Role) // already validated in Prepare
	if args.Role == "" {
		// Report the role the resume actually runs under (same rule as
		// Prepare and Manager.Resume), not the researcher default.
		role = t.persistedRole(ctx, sessionID, role)
	}
	if err := verifyPreparedCall(&t.key, t.def, riskOf(role), prepared); err != nil {
		return resumeError(prepared.Call.ID, startedAt, err)
	}

	snap, ok := t.m.factory.Models.Get()
	if !ok || snap.Model == nil {
		return resumeError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput,
			"sub-agent is unavailable: no model selection is active for this turn"))
	}

	resumeErr := t.m.Resume(SpawnSpec{
		Task:          args.Task,
		Focus:         args.Focus,
		Role:          role,
		ParentSession: snap.ParentSession,
		ParentCall:    prepared.Call.ID,
	}, sessionID)
	if resumeErr != nil {
		return resumeError(prepared.Call.ID, startedAt, resumeErr)
	}

	payload := map[string]any{
		"child_session_id": sessionID.String(),
		"role":             string(role),
		"status":           "resumed",
	}
	raw, jsonErr := json.Marshal(payload)
	if jsonErr != nil {
		return resumeError(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode resumed result", domain.WithCause(jsonErr)))
	}
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(raw)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
		Metadata: map[string]string{
			"child_session_id": sessionID.String(),
			"spawn_status":     "resumed",
			"role":             string(role),
		},
	}
}

type resumeArgs struct {
	ChildSessionID string   `json:"child_session_id"`
	Task           string   `json:"task"`
	Focus          []string `json:"focus"`
	Role           string   `json:"role"`
}

// persistedRole resolves the role a resume will actually run under when
// the caller did not override it: the delegation edge on the child
// session is the source of truth (Manager.Resume applies the same rule).
// Any inspection failure keeps the fallback — Execute surfaces the error.
func (t *ResumeSubagentTool) persistedRole(ctx context.Context, sessionID domain.SessionID, fallback Role) Role {
	store := t.m.factory.Store
	if store == nil {
		return fallback
	}
	inspection, err := store.InspectSession(ctx, sessionID)
	if err != nil {
		return fallback
	}
	if persisted := roleOf(inspection.Events); persisted != "" && t.m.HasRole(persisted) {
		return persisted
	}
	return fallback
}

func resumeError(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
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

// trimAndValidateTask normalizes and validates a task string.
func trimAndValidateTask(task string) string {
	task = strings.TrimSpace(task)
	if len(task) > maxTaskBytes {
		return task[:maxTaskBytes]
	}
	return task
}
