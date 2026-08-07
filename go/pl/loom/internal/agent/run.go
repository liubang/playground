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

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
)

// Run represents an in-memory projection of a single agent run.
type Run struct {
	ID        domain.RunID
	SessionID domain.SessionID
	State     domain.RunState
	Plan      domain.Plan
	Usage     domain.Usage
	Limits    domain.Limits
	Messages  []domain.Message
	Version   int64
	Clock     domain.Clock
	// Goal is the cross-turn objective set via the update_goal tool; nil
	// when none is active. It persists through checkpoints like Plan.
	Goal *domain.Goal

	pendingEvents    []domain.Event
	persistedVersion int64
	// turnStartedAt anchors the per-prompt wall-clock observability
	// counter (Usage.WallTime is display-only, never a budget dimension).
	turnStartedAt time.Time
	// WrapUpPending marks the soft-landing wrap-up turn
	// (docs/CONTEXT_DESIGN.md §4.4.2): the run budget (dimension name) or
	// the goal token budget (wrapUpGoalTokens) is exhausted and the model
	// gets exactly one final turn to summarize before termination. Empty
	// means no wrap-up is pending. In-memory only; crash recovery re-arms
	// it from the budget.wrapup_started event or the transcript tail.
	WrapUpPending string
	// TraceID is the observability backend's trace identifier for this run,
	// set by the loop right after StartRun ("" when tracing is disabled).
	// In-memory only — it travels to persistence via the stamped message
	// metadata (AddAssistantMessage), which is what late-arriving user
	// feedback uses to find the trace again.
	TraceID string
}

// NewRun creates a new Run in the preparing phase.
func NewRun(sessionID domain.SessionID, limits domain.Limits, clock domain.Clock) *Run {
	return &Run{
		ID:            domain.NewRunID(),
		SessionID:     sessionID,
		State:         domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		Limits:        limits,
		Clock:         clock,
		turnStartedAt: clock.Now(),
	}
}

// RecordDelegation marks the run as a delegated sub-agent run: the
// run.created event carries the parent session and the delegate_task
// call that spawned it, forming the persistent delegation edge
// (docs/SUBAGENT_DESIGN.md §6.1) — the loom equivalent of codex's
// agent-graph-store spawn edge, carried by the existing event stream.
func (r *Run) RecordDelegation(parent domain.SessionID, callID domain.ToolCallID, role string) {
	r.appendEvent(domain.EventRunCreated, struct {
		RunID           domain.RunID      `json:"run_id"`
		Delegated       bool              `json:"delegated"`
		ParentSessionID domain.SessionID  `json:"parent_session_id"`
		ParentToolCall  domain.ToolCallID `json:"parent_tool_call_id"`
		Role            string            `json:"role,omitempty"`
	}{RunID: r.ID, Delegated: true, ParentSessionID: parent, ParentToolCall: callID, Role: role})
}

// RestoreRun creates a Run from a checkpoint.
func RestoreRun(id domain.RunID, sessionID domain.SessionID, state domain.RunState, plan domain.Plan, usage domain.Usage, limits domain.Limits, msgs []domain.Message, version int64, clock domain.Clock) *Run {
	return &Run{
		ID:               id,
		SessionID:        sessionID,
		State:            state,
		Plan:             plan,
		Usage:            usage,
		Limits:           limits,
		Messages:         msgs,
		Version:          version,
		Clock:            clock,
		persistedVersion: version,
		// A restored run's wall-time window anchors at restore time; the
		// original start is unknowable from the checkpoint.
		turnStartedAt: clock.Now(),
	}
}

// ContinueRun starts a new active run in an existing session from a complete
// terminal checkpoint. The continuation preserves transcript and plan, while
// using optimistic persistence from the supplied session version.
//
// Budget semantics: limits are a PER-PROMPT runaway cap, not a session-level
// spending account. The checkpoint's cumulative usage is discarded and the
// continuation starts with a fresh budget window (see ResetUsageForNewTurn);
// otherwise a long session's accumulated input tokens would brick every
// subsequent prompt at the loop-entry hard check.
func ContinueRun(checkpoint domain.Checkpoint, messages []domain.Message, sessionVersion int64, limits domain.Limits, clock domain.Clock) (*Run, error) {
	if checkpoint.ID.IsZero() || checkpoint.SessionID.IsZero() {
		return nil, domain.NewError(domain.ErrInvalidInput, "checkpoint and session IDs are required")
	}
	if err := checkpoint.State.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid checkpoint state", domain.WithCause(err))
	}
	if checkpoint.State.Lifecycle != domain.LifecycleTerminal {
		return nil, domain.NewError(domain.ErrConflict, "only a terminal session can be continued safely")
	}
	if checkpoint.Sequence != sessionVersion {
		return nil, domain.NewError(domain.ErrConflict,
			fmt.Sprintf("checkpoint sequence %d does not match session version %d", checkpoint.Sequence, sessionVersion))
	}
	if clock == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "clock is required")
	}
	for i, message := range messages {
		if err := message.Validate(); err != nil {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("invalid restored message at index %d", i), domain.WithCause(err))
		}
		if message.Sequence != int64(i+1) {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("restored message sequence %d at index %d, want %d", message.Sequence, i, i+1))
		}
	}
	run := RestoreRun(domain.NewRunID(), checkpoint.SessionID,
		domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		checkpoint.Plan, checkpoint.Usage, limits, append([]domain.Message(nil), messages...), sessionVersion, clock)
	run.ResetUsageForNewTurn()
	// A goal survives the prompt boundary: the continuation keeps pursuing
	// the same objective (a budget-limited/closed goal stays closed).
	if checkpoint.Goal != nil {
		goal := *checkpoint.Goal
		run.Goal = &goal
	}
	// A terminal checkpoint can still carry dangling tool calls (a run that
	// died between routing and execution). Providers reject replayed
	// transcripts with unresolved calls, so close them like crash recovery
	// does — explicitly marked as never executed and never replayed.
	for _, call := range unresolvedToolCalls(run.Messages) {
		run.RecordToolResult(domain.ToolResult{
			CallID: call.ID, Status: domain.ToolStatusError,
			Error: &domain.ToolError{
				Code:      "interrupted",
				Message:   "tool call had no recorded outcome when the run ended; it was not executed and was not replayed",
				Retryable: false,
			},
			StartedAt: clock.Now(), FinishedAt: clock.Now(),
		})
	}
	run.appendEvent(domain.EventRunCreated, struct {
		RunID        domain.RunID        `json:"run_id"`
		ContinuesRun bool                `json:"continues_run"`
		CheckpointID domain.CheckpointID `json:"checkpoint_id"`
	}{RunID: run.ID, ContinuesRun: true, CheckpointID: checkpoint.ID})
	return run, nil
}

// RecoverRun creates a continuation from an interrupted session. Pending calls
// that never started, and interrupted read-only calls, are closed with explicit
// tool errors. A started R2+ call has an uncertain side effect and blocks
// automatic recovery.
type FileStateReader interface {
	SHA256(path string) (string, error)
}

func RecoverRun(sessionID domain.SessionID, checkpoint *domain.Checkpoint, messages []domain.Message, events []domain.Event, sessionVersion int64, limits domain.Limits, clock domain.Clock, files FileStateReader) (*Run, error) {
	if sessionID.IsZero() || sessionVersion < 0 || clock == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "valid session, version, and clock are required")
	}
	if checkpoint != nil {
		if checkpoint.SessionID != sessionID || checkpoint.Sequence > sessionVersion {
			return nil, domain.NewError(domain.ErrConflict, "checkpoint does not match the recoverable session version")
		}
		if checkpoint.State.Lifecycle == domain.LifecycleTerminal && checkpoint.Sequence == sessionVersion {
			return ContinueRun(*checkpoint, messages, sessionVersion, limits, clock)
		}
	}

	started := make(map[domain.ToolCallID]toolCallAuditPayload)
	completed := make(map[domain.ToolCallID]struct{})
	for i, event := range events {
		if event.SessionID != sessionID || event.Sequence != int64(i+1) {
			return nil, domain.NewError(domain.ErrInvalidInput, "event timeline is not contiguous for the session")
		}
		switch event.Type {
		case domain.EventToolExecutionStarted:
			var payload toolCallAuditPayload
			if err := json.Unmarshal(event.Payload, &payload); err != nil || payload.CallID.IsZero() {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool execution start payload", domain.WithCause(err))
			}
			started[payload.CallID] = payload
		case domain.EventToolExecutionCompleted:
			var payload toolExecutionCompletedPayload
			if err := json.Unmarshal(event.Payload, &payload); err != nil || payload.CallID.IsZero() {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool execution completion payload", domain.WithCause(err))
			}
			completed[payload.CallID] = struct{}{}
		}
	}
	if int64(len(events)) != sessionVersion {
		return nil, domain.NewError(domain.ErrConflict, "event timeline does not match the session version")
	}

	unresolved := unresolvedToolCalls(messages)
	reconciled := make(map[domain.ToolCallID]domain.ToolResult)
	for _, call := range unresolved {
		if audit, ok := started[call.ID]; ok {
			if _, done := completed[call.ID]; done {
				return nil, domain.NewError(domain.ErrConflict,
					fmt.Sprintf("tool call %s (%s) completed without a persisted result", call.ID, audit.Tool))
			}
			if audit.Risk > domain.R1 {
				result, resolved, err := reconcileFileOperation(call, audit, clock, files)
				if err != nil {
					return nil, err
				}
				if !resolved {
					return nil, domain.NewError(domain.ErrConflict,
						fmt.Sprintf("tool call %s (%s) has an uncertain non-idempotent outcome; inspect the side effect manually", call.ID, audit.Tool))
				}
				reconciled[call.ID] = result
			}
		}
	}

	plan := domain.Plan{}
	usage := domain.Usage{}
	var goal *domain.Goal
	if checkpoint != nil {
		plan = checkpoint.Plan
		usage = checkpoint.Usage
		goal = checkpoint.Goal
	}
	wrapUpDimension := ""
	for _, event := range events {
		if checkpoint != nil && event.Sequence <= checkpoint.Sequence {
			continue
		}
		switch event.Type {
		case domain.EventBudgetUpdated:
			if err := json.Unmarshal(event.Payload, &usage); err != nil {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid budget update payload", domain.WithCause(err))
			}
		case domain.EventPlanRevised:
			if err := json.Unmarshal(event.Payload, &plan); err != nil {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid plan revision payload", domain.WithCause(err))
			}
		case domain.EventGoalUpdated:
			var updated domain.Goal
			if err := json.Unmarshal(event.Payload, &updated); err != nil {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid goal update payload", domain.WithCause(err))
			}
			goal = &updated
		case domain.EventBudgetWrapupStarted:
			// A crash during the soft-landing wrap-up re-arms it so the run
			// still ends with a conclusion (docs/CONTEXT_DESIGN.md §4.4.2).
			var payload domain.BudgetWrapupPayload
			if err := json.Unmarshal(event.Payload, &payload); err != nil {
				return nil, domain.NewError(domain.ErrInvalidInput, "invalid budget wrap-up payload", domain.WithCause(err))
			}
			wrapUpDimension = payload.Dimension
		}
	}
	run := RestoreRun(domain.NewRunID(), sessionID,
		domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		plan, usage, limits, append([]domain.Message(nil), messages...), sessionVersion, clock)
	if goal != nil {
		cloned := *goal
		run.Goal = &cloned
	}
	if wrapUpDimension != "" {
		run.WrapUpPending = wrapUpDimension
	} else if n := len(run.Messages); n > 0 {
		// Fallback without the event (lost tail): an unanswered wrap-up
		// instruction at the transcript tail means the same thing.
		if last := run.Messages[n-1]; last.Role == domain.RoleUser && last.Metadata["kind"] == "budget_wrapup" {
			run.WrapUpPending = dimensionTokens
		}
	}
	run.appendEvent(domain.EventRunCreated, struct {
		RunID       domain.RunID `json:"run_id"`
		Recovery    bool         `json:"recovery"`
		Interrupted bool         `json:"interrupted"`
	}{RunID: run.ID, Recovery: true, Interrupted: true})
	for _, call := range unresolved {
		if result, ok := reconciled[call.ID]; ok {
			run.RecordToolResult(result)
			continue
		}
		message := "tool call was interrupted before execution; prior approval is invalidated and the call was not replayed"
		retryable := false
		if audit, ok := started[call.ID]; ok {
			if _, done := completed[call.ID]; !done && audit.Risk <= domain.R1 {
				message = "read-only tool execution was interrupted; retry explicitly if still needed"
				retryable = true
			}
		}
		run.RecordToolResult(domain.ToolResult{
			CallID: call.ID, Status: domain.ToolStatusError,
			Error:     &domain.ToolError{Code: "interrupted", Message: message, Retryable: retryable},
			StartedAt: clock.Now(), FinishedAt: clock.Now(),
		})
	}
	return run, nil
}

func reconcileFileOperation(call domain.ToolCall, audit toolCallAuditPayload, clock domain.Clock, files FileStateReader) (domain.ToolResult, bool, error) {
	if audit.Recovery == nil || audit.Recovery.Kind != "file_replace" || files == nil {
		return domain.ToolResult{}, false, nil
	}
	if audit.Recovery.Path == "" || audit.Recovery.ExpectedHash == "" || audit.Recovery.ResultHash == "" {
		return domain.ToolResult{}, false, domain.NewError(domain.ErrInvalidInput, "file recovery evidence is incomplete")
	}
	current, err := files.SHA256(audit.Recovery.Path)
	if err != nil {
		return domain.ToolResult{}, false, domain.NewError(domain.ErrConflict,
			fmt.Sprintf("cannot inspect interrupted file operation %s", call.ID), domain.WithCause(err))
	}
	now := clock.Now()
	switch current {
	case audit.Recovery.ResultHash:
		return domain.ToolResult{
			CallID: call.ID, Status: domain.ToolStatusSuccess, StartedAt: now, FinishedAt: now,
			Metadata: map[string]string{"recovery": "confirmed_applied", "path": audit.Recovery.Path, "new_hash": current},
		}, true, nil
	case audit.Recovery.ExpectedHash:
		return domain.ToolResult{
			CallID: call.ID, Status: domain.ToolStatusError, StartedAt: now, FinishedAt: now,
			Error:    &domain.ToolError{Code: "interrupted_not_applied", Message: "file write was not applied; retry explicitly if still needed", Retryable: true},
			Metadata: map[string]string{"recovery": "confirmed_not_applied", "path": audit.Recovery.Path},
		}, true, nil
	default:
		return domain.ToolResult{}, false, domain.NewError(domain.ErrConflict,
			fmt.Sprintf("file %q changed to an unexpected hash after interrupted tool call %s", audit.Recovery.Path, call.ID))
	}
}

func unresolvedToolCalls(messages []domain.Message) []domain.ToolCall {
	resolved := make(map[domain.ToolCallID]struct{})
	for _, message := range messages {
		for _, part := range message.Parts {
			if part.Kind == domain.PartToolResult && part.ToolResult != nil {
				resolved[part.ToolResult.CallID] = struct{}{}
			}
		}
	}
	var unresolved []domain.ToolCall
	for _, message := range messages {
		for _, call := range message.ToolCalls() {
			if _, ok := resolved[call.ID]; !ok {
				unresolved = append(unresolved, call)
			}
		}
	}
	return unresolved
}

// TransitionTo moves the run to the given phase, returning events.
func (r *Run) TransitionTo(phase domain.Phase) ([]domain.Event, error) {
	newState, err := r.State.Transition(phase)
	if err != nil {
		return nil, err
	}
	r.State = newState
	r.Version++
	evt := r.newEvent(domain.EventRunStateChanged, r.State)
	r.pendingEvents = append(r.pendingEvents, evt)
	return []domain.Event{evt}, nil
}

// Suspend suspends the run.
func (r *Run) Suspend(reason domain.SuspensionReason) ([]domain.Event, error) {
	newState, err := r.State.Suspend(reason)
	if err != nil {
		return nil, err
	}
	r.State = newState
	r.Version++
	evt := r.newEvent(domain.EventRunStateChanged, r.State)
	r.pendingEvents = append(r.pendingEvents, evt)
	return []domain.Event{evt}, nil
}

// Resume resumes a suspended run back to active.
func (r *Run) Resume() ([]domain.Event, error) {
	newState, err := r.State.Resume()
	if err != nil {
		return nil, err
	}
	r.State = newState
	r.Version++
	evt := r.newEvent(domain.EventRunStateChanged, r.State)
	r.pendingEvents = append(r.pendingEvents, evt)
	return []domain.Event{evt}, nil
}

// Terminate terminates the run.
func (r *Run) Terminate(outcome domain.Outcome) ([]domain.Event, error) {
	newState, err := r.State.Terminate(outcome)
	if err != nil {
		return nil, err
	}
	r.State = newState
	r.Version++

	evtType := domain.EventRunCompleted
	switch outcome {
	case domain.OutcomeFailed:
		evtType = domain.EventRunFailed
	case domain.OutcomeCancelled:
		evtType = domain.EventRunCancelled
	}

	evt := r.newEvent(evtType, r.State)
	r.pendingEvents = append(r.pendingEvents, evt)
	return []domain.Event{evt}, nil
}

// AddUserMessage appends a user message and increments version.
func (r *Run) AddUserMessage(msg domain.Message) domain.Event {
	r.normalizeMessage(&msg)
	r.Messages = append(r.Messages, msg)
	r.Version++
	evt := r.newEvent(domain.EventUserMessageAdded, domain.MessageEventPayload{Message: msg})
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// AddSystemNote appends a system-role notice to the transcript without an
// audit event: like compaction markers, it is runtime bookkeeping persisted
// through checkpoints, not a user or assistant utterance. Version and event
// sequencing are untouched.
func (r *Run) AddSystemNote(text string) {
	msg := domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleSystem, Status: domain.MessageStatusFinal,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		CreatedAt: r.Clock.Now(),
		Metadata:  map[string]string{"kind": "system_note"},
	}
	r.normalizeMessage(&msg)
	r.Messages = append(r.Messages, msg)
}

// AddBudgetNotice appends a graduated budget reminder to the transcript as
// a system-role message and records the auditable EventBudgetNotice in a
// single version step (docs/CONTEXT_DESIGN.md §4.4.1: budget decisions
// that steer the model must be replayable, unlike ephemeral system notes).
func (r *Run) AddBudgetNotice(payload domain.BudgetNoticePayload) domain.Event {
	msg := payload.Message
	if msg.Role == "" {
		msg.Role = domain.RoleSystem
	}
	if msg.Metadata == nil {
		msg.Metadata = map[string]string{"kind": "system_note"}
	}
	if msg.CreatedAt.IsZero() {
		msg.CreatedAt = r.Clock.Now()
	}
	if msg.ID.IsZero() {
		msg.ID = domain.NewMessageID()
	}
	r.normalizeMessage(&msg)
	payload.Message = msg
	r.Messages = append(r.Messages, msg)
	r.Version++
	evt := r.newEvent(domain.EventBudgetNotice, payload)
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// AddAssistantMessage appends an assistant message.
func (r *Run) AddAssistantMessage(msg domain.Message) domain.Event {
	r.normalizeMessage(&msg)
	// Stamp the run/trace identity so both frontends (per-turn feedback
	// targeting) and the feedback endpoint (run_id → trace_id lookup) can
	// correlate this message with its observability trace after restart —
	// the metadata persists with checkpoints and events.
	if msg.Metadata == nil {
		msg.Metadata = make(map[string]string, 2)
	}
	msg.Metadata["run_id"] = r.ID.String()
	if r.TraceID != "" {
		msg.Metadata["trace_id"] = r.TraceID
	}
	r.Messages = append(r.Messages, msg)
	r.Version++
	evt := r.newEvent(domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: msg})
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// RecordToolResult records a tool result message. Text content is
// bounded to MaxToolOutputBytes with a head+tail cut — the single final
// truncation point shared by every tool (docs/CONTEXT_DESIGN.md §4.5),
// so outsized results cannot swamp the transcript between the tools'
// own entry-level limits and compaction.
func (r *Run) RecordToolResult(result domain.ToolResult) domain.Event {
	truncateToolResultContent(&result, r.Limits.MaxToolOutputBytes)
	r.Usage.ToolCalls++
	r.Version++

	part := domain.ContentPart{
		Kind:       domain.PartToolResult,
		ToolResult: &result,
	}
	msg := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleAssistant,
		Parts:     []domain.ContentPart{part},
		CreatedAt: r.Clock.Now(),
	}
	r.normalizeMessage(&msg)
	r.Messages = append(r.Messages, msg)
	resultEvent := r.newEvent(domain.EventToolResultAdded, domain.MessageEventPayload{Message: msg})
	r.pendingEvents = append(r.pendingEvents, resultEvent)
	r.Version++

	payload := toolExecutionCompletedPayload{
		CallID:     result.CallID,
		Status:     result.Status,
		StartedAt:  result.StartedAt,
		FinishedAt: result.FinishedAt,
		Metadata:   cloneMetadata(result.Metadata),
	}
	if result.Error != nil {
		payload.ErrorCode = result.Error.Code
		payload.ErrorMessage = result.Error.Message
	}
	evt := r.newEvent(domain.EventToolExecutionCompleted, payload)
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// toolOutputTruncationMark separates the head and tail of a truncated
// tool output.
const toolOutputTruncationMark = "\n...[middle omitted]...\n"

// truncateToolResultContent applies the ingestion cap to every oversized
// text part of a tool result. The warning header tells the model exactly
// what happened and whether a full-fidelity artifact is attached to the
// result (run_cmd and compacted outputs carry one).
func truncateToolResultContent(result *domain.ToolResult, maxBytes int64) {
	if maxBytes <= 0 {
		return
	}
	for i := range result.Content {
		content := &result.Content[i]
		if content.Kind != domain.PartText || int64(len(content.Text)) <= maxBytes {
			continue
		}
		original := len(content.Text)
		hasArtifact := false
		for _, part := range result.Content {
			if part.Kind == domain.PartArtifact {
				hasArtifact = true
				break
			}
		}
		locator := "unavailable"
		if hasArtifact {
			locator = "see the artifact reference attached to this result"
		}
		warning := fmt.Sprintf("Warning: output truncated (original %s / ~%d tokens, showing first+last portions). Full output: %s.\n",
			humanBytes(original), original/bytesPerTokenEstimate, locator)
		budget := int(maxBytes) - len(warning) - len(toolOutputTruncationMark)
		if budget < 2 {
			content.Text = domain.TruncateAtRuneBoundary(content.Text, int(maxBytes))
			continue
		}
		headLen := budget / 2
		tailLen := budget - headLen
		head := domain.TruncateAtRuneBoundary(content.Text, headLen)
		tail := content.Text[len(content.Text)-tailLen:]
		// The tail cut may split a multi-byte rune at its start.
		for len(tail) > 0 && !utf8.ValidString(tail) {
			tail = tail[1:]
		}
		content.Text = warning + head + toolOutputTruncationMark + tail
	}
}

// touchWallTime folds the elapsed per-prompt window into Usage.WallTime.
// The field is display-only (status bar); it is NOT a budget dimension.
func (r *Run) touchWallTime() {
	if r.Clock == nil || r.turnStartedAt.IsZero() {
		return
	}
	r.Usage.WallTime = r.Clock.Now().Sub(r.turnStartedAt)
}

// CheckBudget evaluates usage against the budget dimensions (session
// tokens, cost). Both the graduated soft notices and the hard wrap-up
// trigger derive from this single check.
func (r *Run) CheckBudget() domain.CheckResult {
	r.touchWallTime()
	return r.Usage.Check(r.Limits)
}

// ResetUsageForNewTurn resets the PER-PROMPT observability counters so a
// fresh user prompt starts with clean display counters. The
// session-cumulative budget counters (tokens, cost) are deliberately
// preserved — they are the session-level budget baseline inherited from
// the checkpoint (docs/CONTEXT_DESIGN.md §4.4.3). It is called by
// frontends at prompt boundaries only; crash recovery (RecoverRun) keeps
// the restored usage so an interrupted run remains accountable for what
// it already consumed.
func (r *Run) ResetUsageForNewTurn() {
	r.Usage.Turns = 0
	r.Usage.ToolCalls = 0
	r.Usage.WallTime = 0
	if r.Clock != nil {
		r.turnStartedAt = r.Clock.Now()
	}
}

// IncrementTurn increments the turn counter and records the complete usage projection.
func (r *Run) IncrementTurn() {
	r.Usage.Turns++
	r.appendEvent(domain.EventBudgetUpdated, r.Usage)
}

func (r *Run) normalizeMessage(msg *domain.Message) {
	msg.Sequence = int64(len(r.Messages) + 1)
	if msg.Status == "" {
		msg.Status = domain.MessageStatusFinal
	}
	if msg.Revision == 0 {
		msg.Revision = 1
	}
	for i := range msg.Parts {
		msg.Parts[i].PartIndex = i
	}
}

func (r *Run) newEvent(evtType domain.EventType, payload any) domain.Event {
	raw, err := domain.MarshalPayload(payload)
	if err != nil {
		panic(fmt.Sprintf("marshal internal event payload: %v", err))
	}
	return domain.Event{
		ID:        domain.NewEventID(),
		Sequence:  r.Version,
		SessionID: r.SessionID,
		Type:      evtType,
		Timestamp: r.Clock.Now(),
		Payload:   raw,
	}
}

func (r *Run) appendEvent(evtType domain.EventType, payload any) domain.Event {
	r.Version++
	evt := r.newEvent(evtType, payload)
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// PendingEvents returns the current batch of events not yet persisted.
func (r *Run) PendingEvents() []domain.Event {
	return r.pendingEvents
}

// PersistedVersion returns the version up to which events have been persisted.
func (r *Run) PersistedVersion() int64 {
	return r.persistedVersion
}

// MarkPersisted marks the given events as persisted at the new version.
func (r *Run) MarkPersisted(newVersion int64, events []domain.Event) {
	r.persistedVersion = newVersion
	// Remove persisted events from pending.
	if len(events) >= len(r.pendingEvents) {
		r.pendingEvents = r.pendingEvents[:0]
	} else {
		r.pendingEvents = r.pendingEvents[len(events):]
	}
}

type toolCallAuditPayload struct {
	CallID       domain.ToolCallID    `json:"call_id"`
	Tool         string               `json:"tool"`
	Risk         domain.RiskLevel     `json:"risk"`
	ArgsHash     string               `json:"args_hash,omitempty"`
	ReadPaths    []string             `json:"read_paths,omitempty"`
	WritePaths   []string             `json:"write_paths,omitempty"`
	ApprovalDesc string               `json:"approval_desc,omitempty"`
	Recovery     *domain.RecoverySpec `json:"recovery,omitempty"`
	// PrepareFailed marks the degraded payload emitted when a call fails
	// during preparation; ArgsRawHash then carries the raw-arguments
	// fingerprint in place of ArgsHash.
	PrepareFailed bool   `json:"prepare_failed,omitempty"`
	ArgsRawHash   string `json:"args_raw_hash,omitempty"`
}

type modelRequestAuditPayload struct {
	RequestID    domain.EventID `json:"request_id"`
	ModelName    string         `json:"model_name"`
	ManifestID   string         `json:"manifest_id"`
	ManifestHash string         `json:"manifest_hash"`
	PromptHash   string         `json:"prompt_hash"`
}

type modelRequestFailedPayload struct {
	RequestID domain.EventID `json:"request_id"`
	Stage     string         `json:"stage"`
	Code      string         `json:"code"`
	// Message carries the underlying error text (rate limit, network
	// failure, ...), so the failure is diagnosable from the event log
	// alone instead of requiring server logs.
	Message string `json:"message,omitempty"`
}

type permissionResolvedPayload struct {
	CallID   domain.ToolCallID `json:"call_id"`
	ArgsHash string            `json:"args_hash"`
	Decision domain.Decision   `json:"decision"`
}

type toolExecutionCompletedPayload struct {
	CallID    domain.ToolCallID `json:"call_id"`
	Status    domain.ToolStatus `json:"status"`
	ErrorCode string            `json:"error_code,omitempty"`
	// ErrorMessage carries the human-readable failure reason so frontends
	// can show it inline; the code alone (e.g. "unavailable") reads like a
	// denial and left users guessing what actually happened.
	ErrorMessage string            `json:"error_message,omitempty"`
	StartedAt    time.Time         `json:"started_at"`
	FinishedAt   time.Time         `json:"finished_at"`
	Metadata     map[string]string `json:"metadata,omitempty"`
}

type fileChangedPayload struct {
	CallID  domain.ToolCallID `json:"call_id"`
	Path    string            `json:"path"`
	OldHash string            `json:"old_hash"`
	NewHash string            `json:"new_hash"`
	Size    int64             `json:"size"`
}

type fileChangeResult struct {
	Path    string `json:"path"`
	OldHash string `json:"old_hash"`
	NewHash string `json:"new_hash"`
	Size    int64  `json:"size"`
}

// Policy evaluates the authorization verdict for a prepared tool call
// (docs/PERMISSION_DESIGN.md §4.1). Evaluation is call-aware (not just
// risk-level) so declarative rules can match on the concrete argv/paths
// before the risk baseline applies, and the verdict carries the execution
// grant alongside the decision.
type Policy interface {
	Evaluate(call domain.PreparedCall) domain.Verdict
}

// DefaultPolicy applies the baseline R0/R1 allow, R2/R3 ask, R4 deny policy.
type DefaultPolicy struct{}

func (DefaultPolicy) Evaluate(call domain.PreparedCall) domain.Verdict {
	v := domain.Verdict{Source: "baseline", Reason: "risk baseline"}
	switch call.Risk {
	case domain.R0, domain.R1:
		v.Decision = domain.DecisionAllow
	case domain.R2, domain.R3:
		v.Decision = domain.DecisionAsk
	default:
		v.Decision = domain.DecisionDeny
	}
	return v
}

// PromptBuilder builds the ephemeral system prompt prepended to every model
// request. The prompt is request-scoped only: it is never persisted into the
// session transcript, and its content is audited through the context manifest
// rule references.
type PromptBuilder interface {
	Build(ctx context.Context) (string, []domain.ContextRuleRef, error)
}

// Loop drives the main agent loop for a Run.
type Loop struct {
	Run          *Run
	Model        domain.Model
	ModelName    string
	Store        domain.SessionStore
	Approver     domain.Approver
	Policy       Policy
	Registry     *ToolRegistry
	Logger       *slog.Logger
	StreamHooks  StreamHooks
	SystemPrompt PromptBuilder
	// Artifacts externalizes compacted tool outputs; nil disables masking.
	Artifacts domain.ArtifactStore
	// Condenser configures context compaction; the zero value applies
	// documented defaults.
	Condenser Condenser
	// Recorder is the observability sink for the run (Langfuse tracing); nil
	// selects a no-op recorder. Prompt is the user submission that started
	// this run, reported as the trace input. Workspace is reported as a
	// trace tag.
	Recorder  trace.Recorder
	Prompt    string
	Workspace string
	// Window carries the model-derived context thresholds (compaction
	// trigger/target, notice levels). Context occupancy — never cumulative
	// token usage — drives compaction. The zero value disables
	// occupancy-driven decisions; forced compaction after a provider
	// overflow still works.
	Window WindowModel
	// Runaway tunes behavior-based runaway detection (repeated calls,
	// consecutive failures, stalls). The zero value applies
	// domain.DefaultRunawayConfig.
	Runaway domain.RunawayConfig
	// Reasoning carries the selected model's reasoning (thinking) intent
	// into every model call; the zero value lets the provider decide.
	Reasoning domain.ReasoningSpec
	// GoalCell receives update_goal tool mutations; the loop drains it
	// after each tool batch. Nil disables goal tracking.
	GoalCell *GoalCell
	// PlanCell receives update_plan snapshots; the loop drains it after each
	// tool batch and replaces the run's plan. Nil disables plan tracking.
	PlanCell *PlanCell
	// SteerCell receives user messages submitted while the turn is busy; the
	// loop drains it in prepare (before every model call) and appends them as
	// regular user messages. Nil disables steering.
	SteerCell *SteerCell
	// CostInputUSDPerMTok / CostOutputUSDPerMTok price metered token usage
	// into Usage.CostUSD so the max_estimated_cost_usd budget dimension can
	// fire. Zero disables cost accounting.
	CostInputUSDPerMTok  float64
	CostOutputUSDPerMTok float64

	prepared map[domain.ToolCallID]domain.PreparedCall
	// traceRun is the active trace handle for the executing run.
	traceRun trace.RunHandle
	// lastCallInput is the provider-metered input token count of the most
	// recent completed model call, republished with context-usage events.
	lastCallInput int64
	// compactFitFailures counts consecutive failures to fit the next
	// request into the context window (provider context-overflow, or a
	// compaction that left occupancy above the window); two in a row is
	// fatal — the window genuinely cannot hold the work.
	compactFitFailures int
	// noticeFired tracks the highest fired level per budget-notice
	// dimension (occupancy/wall_time/cost_usd/runaway) so each graduated
	// reminder fires at most once per prompt; compaction re-arms occupancy.
	noticeFired map[string]int
	// pendingNotices queues runaway warnings detected at pairing-unsafe
	// points (tool routing); prepare drains them before the next call.
	pendingNotices []domain.BudgetNoticePayload
	// lastCallSig/repeatedCallCount track consecutive identical tool calls
	// (runaway repeated-call detection).
	lastCallSig       string
	repeatedCallCount int
	// consecutiveExecFailures counts execution-phase tool failures in a
	// row (prepare failures excluded by design).
	consecutiveExecFailures int
	// seenCallSigs records every (tool, args_hash) signature executed this
	// run; a first-seen signature is a progress signal (new information
	// fetched), which keeps read-only research tasks from stalling out.
	seenCallSigs map[string]struct{}
	// stallTurns counts consecutive turns without any progress signal.
	stallTurns int
	// progressThisTurn is set by any progress signal and consumed by the
	// next prepare.
	progressThisTurn bool
	// lastProgressAt anchors the stall watchdog: the ACTIVE time since
	// the last progress signal. Approval waits shift it forward so user
	// thinking time never counts (docs/CONTEXT_DESIGN.md §4.4.3).
	lastProgressAt time.Time
	// ForceCompact demands compaction before the next model call. It is set
	// internally after a provider context-overflow rejection, and may be set
	// by the caller (e.g. a manual /compact request) to force a pass ahead
	// of the automatic pressure triggers.
	ForceCompact bool
	// CompactTriggerHint labels the next compaction pass's trigger
	// ("manual"/"overflow"/"downshift") for the audit event; empty means
	// automatic pressure.
	CompactTriggerHint string
	// lastCompactEst is the transcript estimate right after the most recent
	// compaction pass. Re-compacting is pointless until the transcript grows
	// past it: on an unchanged transcript the condenser cannot make progress
	// (e.g. everything sits inside the keep-recent window), and the loop
	// would otherwise spin forever, spamming compaction events/checkpoints.
	lastCompactEst int
	// planReconcileUsed bounds the unfinished-plan closing nudge to one
	// extra turn per run: advisory bookkeeping is routinely forgotten on the
	// final step, and an unbounded nudge would loop forever.
	planReconcileUsed bool
	// maxOutputStops counts consecutive output-cap truncations; hitting
	// maxOutputContinuationLimit arms the salvage wrap-up instead of
	// paying another full generation just to be cut off again
	// (docs/SUBAGENT_DESIGN.md §12).
	maxOutputStops int
	// planRevisedThisRun gates the closing nudge to plans this run actually
	// touched: a stale plan inherited from an earlier turn must not hijack
	// an unrelated prompt with a reconcile turn.
	planRevisedThisRun bool
}

// ToolRegistry looks up tools by name. A registry may have a parent:
// lookups fall through to it, while registrations always land in the
// local map — registering a name that exists in the parent shadows it
// for this registry without mutating the shared parent (used by
// per-session overlays, see docs/SERVE_DESIGN.md §4.2).
type ToolRegistry struct {
	mu     sync.RWMutex
	tools  map[string]domain.Tool
	parent *ToolRegistry
}

// NewToolRegistry creates a new registry.
func NewToolRegistry() *ToolRegistry {
	return &ToolRegistry{tools: make(map[string]domain.Tool)}
}

// NewOverlayRegistry returns a registry whose lookups fall through to
// parent. Registrations land in the overlay only; the shared parent is
// never mutated after bootstrap.
func NewOverlayRegistry(parent *ToolRegistry) *ToolRegistry {
	return &ToolRegistry{tools: make(map[string]domain.Tool), parent: parent}
}

// Register adds a validated tool to the registry without allowing
// replacement. In an overlay, a name already present in the parent is
// shadowed locally; only a duplicate in the local map is an error.
func (r *ToolRegistry) Register(t domain.Tool) error {
	if t == nil {
		return fmt.Errorf("register nil tool")
	}
	def := t.Definition()
	if err := def.Validate(); err != nil {
		return fmt.Errorf("invalid tool definition: %w", err)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, exists := r.tools[def.Name]; exists {
		return fmt.Errorf("tool %q already registered", def.Name)
	}
	r.tools[def.Name] = t
	return nil
}

// Lookup returns a tool by name: local registrations first, then the
// parent chain.
func (r *ToolRegistry) Lookup(name string) (domain.Tool, bool) {
	r.mu.RLock()
	t, ok := r.tools[name]
	parent := r.parent
	r.mu.RUnlock()
	if ok {
		return t, true
	}
	if parent != nil {
		return parent.Lookup(name)
	}
	return nil, false
}

// List returns all registered tool definitions: the parent's set merged
// with local registrations, local shadowing parent entries by name.
func (r *ToolRegistry) List() []domain.ToolDefinition {
	merged := make(map[string]domain.ToolDefinition)
	if r.parent != nil {
		for _, def := range r.parent.List() {
			merged[def.Name] = def
		}
	}
	r.mu.RLock()
	for name, t := range r.tools {
		merged[name] = t.Definition()
	}
	r.mu.RUnlock()
	out := make([]domain.ToolDefinition, 0, len(merged))
	for _, def := range merged {
		out = append(out, def)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// runSuccessScore maps the terminal run state to the Langfuse run_success
// score that feeds success-rate dashboards. User cancellations are neutral
// and never scored: a cancelled context is not a failure signal and must
// not poison the success rate.
//
// Cancellation is detected two ways because the provider stream degrades
// errors to strings in domain.ModelEvent (see openAIStream.finishWithError
// and StreamAggregator.Apply), so a mid-stream cancel resurfaces as a
// chain-less error whose text happens to read "context canceled": match
// the wrapped chain when it survives, and fall back to the run context's
// own terminal state when it does not.
func runSuccessScore(ctx context.Context, execErr error, outcome domain.Outcome) (value float64, comment string, scored bool) {
	switch {
	case errors.Is(execErr, context.Canceled):
		return 0, "", false
	case ctx != nil && errors.Is(ctx.Err(), context.Canceled):
		return 0, "", false
	case execErr != nil:
		return 0, execErr.Error(), true
	case outcome == domain.OutcomeFailed || outcome == domain.OutcomeBudgetExhausted:
		return 0, string(outcome), true
	case outcome == domain.OutcomeSucceeded || outcome == domain.OutcomeCompletedUnverified:
		return 1, "", true
	}
	return 0, "", false
}

// Execute runs the agent loop to completion (or until cancelled).
func (l *Loop) Execute(ctx context.Context) (execErr error) {
	recorder := l.Recorder
	if recorder == nil {
		recorder = trace.Noop()
	}
	ctx, l.traceRun = recorder.StartRun(ctx, trace.RunMeta{
		SessionID: l.Run.SessionID.String(),
		RunID:     l.Run.ID.String(),
		Model:     l.ModelName,
		Prompt:    l.Prompt,
		Workspace: l.Workspace,
	})
	// Publish the trace identity onto the run so every assistant message
	// persisted this turn carries it (see AddAssistantMessage).
	l.Run.TraceID = l.traceRun.TraceID()
	defer func() {
		result := trace.RunResult{
			Outcome: string(l.Run.State.Outcome),
			Output:  lastAssistantText(l.Run.Messages),
		}
		if execErr != nil {
			result.Error = execErr.Error()
		}
		l.traceRun.End(result)
		if value, comment, scored := runSuccessScore(ctx, execErr, l.Run.State.Outcome); scored {
			l.traceRun.Score(ctx, "run_success", value, comment)
		}
	}()

	if err := l.flushEvents(ctx); err != nil {
		l.terminate(ctx, domain.OutcomeFailed)
		return err
	}
	for {
		if err := ctx.Err(); err != nil {
			l.terminate(ctx, domain.OutcomeCancelled)
			return err
		}

		if l.Run.State.Phase == domain.PhasePreparing {
			// Hard budget check, only at this transcript-pairing-safe
			// boundary and only outside an active wrap-up: a breach enters
			// the soft-landing wrap-up turn instead of killing the run
			// mid-work (docs/CONTEXT_DESIGN.md §4.4.2). Budgets cover
			// scarce resources only (session tokens, cost); context
			// pressure is absorbed by compaction below, and stalls are
			// caught by the watchdog right after.
			if l.Run.WrapUpPending == "" {
				if check := l.Run.CheckBudget(); check.HasHard() {
					l.startBudgetWrapUp(check.HardBreaches)
				}
				l.checkStallTimeout()
			}
			// Single compaction decision point: before each model call,
			// compact when occupancy pressure or a forced retry after
			// provider context-overflow demands it.
			if l.shouldCompact() {
				if _, err := l.Run.TransitionTo(domain.PhaseCompacting); err != nil {
					return err
				}
			}
		}

		switch l.Run.State.Phase {
		case domain.PhasePreparing:
			if err := l.prepare(ctx); err != nil {
				return err
			}
		case domain.PhaseCallingModel:
			if err := l.callModel(ctx); err != nil {
				return err
			}
		case domain.PhaseAwaitingApproval:
			if err := l.awaitApproval(ctx); err != nil {
				return err
			}
		case domain.PhaseExecutingTools:
			if err := l.executeTools(ctx); err != nil {
				return err
			}
		case domain.PhaseCompacting:
			if err := l.compact(ctx); err != nil {
				return err
			}
		default:
			if l.Run.State.Lifecycle == domain.LifecycleTerminal {
				return nil
			}
			return fmt.Errorf("unexpected phase: %s", l.Run.State.Phase)
		}

		if err := l.flushEvents(ctx); err != nil {
			l.terminate(ctx, domain.OutcomeFailed)
			return err
		}
		if l.Run.State.Lifecycle == domain.LifecycleTerminal {
			return nil
		}
	}
}

func (l *Loop) prepare(ctx context.Context) error {
	if _, err := l.Run.TransitionTo(domain.PhaseCallingModel); err != nil {
		return err
	}
	l.Run.IncrementTurn()
	l.drainSteer()
	// Notices must not be appended between an assistant tool-call message
	// and its routing/results: a trailing note would make the
	// call-routing readers miss the calls (they scan for the most recent
	// message with tool calls) and, worse, leave dangling calls that
	// providers reject. Injecting here — right before the next model
	// call, after tool results are recorded — keeps the transcript
	// pairing-safe by construction. Runaway warnings detected during
	// routing are queued for exactly this point.
	l.drainPendingNotices()
	l.injectBudgetNotices()
	l.trackStall()
	return nil
}

// drainSteer injects queued user messages as regular user messages at the
// only transcript-pairing-safe point: after the previous tool results were
// recorded and before the next model call (the same guarantee the budget
// notice below relies on). Each message becomes a durable
// EventUserMessageAdded on the next flush.
func (l *Loop) drainSteer() {
	if l.SteerCell == nil {
		return
	}
	for _, text := range l.SteerCell.Take() {
		l.Run.AddUserMessage(domain.Message{
			ID:        domain.NewMessageID(),
			Role:      domain.RoleUser,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
			CreatedAt: l.Run.Clock.Now(),
		})
	}
}

func (l *Loop) callModel(ctx context.Context) error {
	modelName := l.ModelName
	if modelName == "" {
		modelName = "default"
	}
	messages, rules := l.effectiveMessages(ctx)
	manifest, err := buildContextManifest(messages, rules)
	if err != nil {
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("build context manifest: %w", err)
	}
	req := domain.ModelRequest{
		ID:              domain.NewEventID(),
		ModelName:       modelName,
		Messages:        messages,
		Tools:           l.Registry.List(),
		MaxTokens:       l.Run.Limits.MaxOutputTokens,
		Reasoning:       l.Reasoning,
		ContextManifest: manifest,
	}

	startedAt := l.Run.Clock.Now()
	l.Run.appendEvent(domain.EventModelRequestStarted, modelRequestAuditPayload{
		RequestID: req.ID, ModelName: modelName, ManifestID: manifest.ID,
		ManifestHash: manifest.Hash, PromptHash: manifest.PromptHash,
	})
	if err := l.flushEvents(ctx); err != nil {
		l.terminate(ctx, domain.OutcomeFailed)
		return err
	}
	stream, err := l.Model.Stream(ctx, req)
	if err != nil {
		l.recordGeneration(ctx, trace.GenerationRecord{
			RequestID: req.ID.String(), Turn: l.Run.Usage.Turns, Model: modelName,
			Input: messages, StartTime: startedAt, EndTime: l.Run.Clock.Now(), Err: err,
		})
		if errors.Is(err, context.Canceled) {
			// Cancellation is a user action, not a request failure: skip the
			// request_failed audit event and terminate as cancelled.
			l.terminate(ctx, domain.OutcomeCancelled)
			return fmt.Errorf("model stream: %w", err)
		}
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "start", Code: errorCodeForAudit(err), Message: err.Error(),
		})
		if isContextOverflowError(err) {
			return l.handleContextOverflow(ctx, err)
		}
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream: %w", err)
	}
	defer stream.Close()

	agg := NewStreamAggregator(l.Run.Clock, l.StreamHooks).WithIDRewrite(l.toolCallIDTaken())
	aggErr := consumeStream(stream, agg)
	if aggErr != nil {
		if agg.HasPartialContent() {
			l.Run.AddAssistantMessage(agg.InterruptedMessage())
		}
		l.recordGeneration(ctx, trace.GenerationRecord{
			RequestID: req.ID.String(), Turn: l.Run.Usage.Turns, Model: modelName,
			Input: messages, StartTime: startedAt, EndTime: l.Run.Clock.Now(), Err: aggErr,
		})
		if errors.Is(aggErr, context.Canceled) {
			// Same cancellation routing as the start stage: cancelled, not
			// failed, and no request_failed event.
			l.terminate(ctx, domain.OutcomeCancelled)
			return fmt.Errorf("model stream consumption: %w", aggErr)
		}
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "stream", Code: errorCodeForAudit(aggErr), Message: aggErr.Error(),
		})
		if isContextOverflowError(aggErr) {
			return l.handleContextOverflow(ctx, aggErr)
		}
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream consumption: %w", aggErr)
	}
	response, stop, inputTokens, outputTokens, err := agg.Finalize()
	if err != nil {
		if agg.HasPartialContent() {
			l.Run.AddAssistantMessage(agg.InterruptedMessage())
		}
		l.recordGeneration(ctx, trace.GenerationRecord{
			RequestID: req.ID.String(), Turn: l.Run.Usage.Turns, Model: modelName,
			Input: messages, StartTime: startedAt, EndTime: l.Run.Clock.Now(), Err: err,
		})
		if errors.Is(err, context.Canceled) {
			l.terminate(ctx, domain.OutcomeCancelled)
			return fmt.Errorf("model stream finalization: %w", err)
		}
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "finalize", Code: errorCodeForAudit(err), Message: err.Error(),
		})
		if isContextOverflowError(err) {
			return l.handleContextOverflow(ctx, err)
		}
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream finalization: %w", err)
	}
	if rewritten := agg.RewrittenIDs(); len(rewritten) > 0 && l.Logger != nil {
		l.Logger.Warn("rewrote colliding provider tool call ids", "count", len(rewritten))
	}
	if text := strings.Join(response.TextParts(), ""); strings.TrimSpace(text) != "" {
		l.markProgress()
	}
	l.recordGeneration(ctx, trace.GenerationRecord{
		RequestID: req.ID.String(), Turn: l.Run.Usage.Turns, Model: modelName,
		Input: messages, Output: response, StopReason: string(stop),
		InputTokens: inputTokens, OutputTokens: outputTokens,
		StartTime: startedAt, EndTime: l.Run.Clock.Now(),
	})
	// Record the terminal stream facts on the persisted message so that event
	// consumers (runtime-event bridge, session inspection) can recover the real
	// stop reason and correlate the response with its request.
	if response.Metadata == nil {
		response.Metadata = make(map[string]string, 2)
	}
	response.Metadata["request_id"] = req.ID.String()
	response.Metadata["stop_reason"] = string(stop)
	l.accountUsage(inputTokens, outputTokens)
	l.Run.AddAssistantMessage(response)
	l.Run.appendEvent(domain.EventBudgetUpdated, l.Run.Usage)
	l.lastCallInput = inputTokens
	// A completed call proves the request fit the window.
	l.compactFitFailures = 0
	l.reportContextUsage()

	if len(response.ToolCalls()) == 0 {
		return l.determineCompletion(ctx, stop)
	}
	return l.routeToolCalls(ctx)
}

// reportContextUsage publishes the current transcript estimate so frontends
// can show live context-window occupancy. It is a no-op without the hook.
func (l *Loop) reportContextUsage() {
	if l.StreamHooks.OnContextUsage != nil {
		l.StreamHooks.OnContextUsage(estTokens(l.Run.Messages), l.lastCallInput)
	}
}

// effectiveMessages returns the transcript with the ephemeral system prompt
// prepended, together with the prompt's audit rule references. A build
// failure degrades to the bare transcript rather than failing the turn.
func (l *Loop) effectiveMessages(ctx context.Context) ([]domain.Message, []domain.ContextRuleRef) {
	messages := append([]domain.Message(nil), l.Run.Messages...)
	var rules []domain.ContextRuleRef
	if l.SystemPrompt != nil {
		text, refs, err := l.SystemPrompt.Build(ctx)
		switch {
		case err != nil:
			if l.Logger != nil {
				l.Logger.Warn("build system prompt failed; continuing without it", "error", err)
			}
		case strings.TrimSpace(text) != "":
			system := domain.Message{
				ID:        domain.NewMessageID(),
				Role:      domain.RoleSystem,
				Status:    domain.MessageStatusFinal,
				Revision:  1,
				Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
				CreatedAt: l.Run.Clock.Now(),
			}
			messages = append([]domain.Message{system}, messages...)
			rules = refs
		}
	}
	// Re-inject the live task plan right after the system prompt. The note
	// is rebuilt per request and never persisted, so the model keeps plan
	// awareness across context compactions and crash recovery — a summary
	// replacement that drops message history cannot lose the plan.
	if plan := l.Run.Plan; len(plan.Items) > 0 && !plan.IsComplete() {
		note := domain.Message{
			ID:        domain.NewMessageID(),
			Role:      domain.RoleSystem,
			Status:    domain.MessageStatusFinal,
			Revision:  1,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: planStatusNote(plan)}},
			CreatedAt: l.Run.Clock.Now(),
		}
		insertAt := 0
		if len(messages) > 0 && messages[0].Role == domain.RoleSystem {
			insertAt = 1
		}
		messages = append(messages[:insertAt:insertAt], append([]domain.Message{note}, messages[insertAt:]...)...)
	}
	return messages, rules
}

// reconcilePlanIfUnfinished gives the model exactly one extra turn to
// close out an unfinished plan before the run terminates. Advisory plans are
// otherwise routinely left with an in_progress (or todo) step after the
// final answer, so a session that succeeded looks like it is still working.
// One nudge per run: if the model still ends with an open plan afterwards,
// accept it and terminate.
func (l *Loop) reconcilePlanIfUnfinished() bool {
	if l.planReconcileUsed || !l.planRevisedThisRun {
		return false
	}
	plan := l.Run.Plan
	if len(plan.Items) == 0 || plan.IsComplete() {
		return false
	}
	l.planReconcileUsed = true
	l.Run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: planReconcilePrompt(plan)}},
		CreatedAt: l.Run.Clock.Now(),
	})
	return true
}

// planReconcilePrompt is the synthetic user message for the closing nudge:
// finish genuinely remaining work, or close the bookkeeping when the
// deliverable is already produced.
func planReconcilePrompt(plan domain.Plan) string {
	var remaining strings.Builder
	for _, item := range plan.Items {
		if item.Status != domain.PlanItemDone {
			fmt.Fprintf(&remaining, "- [%s] %s\n", item.Status, item.Goal)
		}
	}
	return fmt.Sprintf(`The task plan still has unfinished steps:

%s

Before ending your turn, reconcile the plan: if the remaining work is in fact complete (its deliverable is already produced in this conversation), call update_plan to mark those steps done with a brief evidence note — that is closing bookkeeping, not pre-marking. Otherwise keep executing the remaining steps instead of ending.`, strings.TrimRight(remaining.String(), "\n"))
}

func (l *Loop) determineCompletion(ctx context.Context, stop domain.StopReason) error {
	switch stop {
	case domain.StopEndTurn:
		// The budget wrap-up turn ends here: the model produced its final
		// summary without tool calls, so the soft landing is complete.
		if l.inRunBudgetWrapUp() {
			l.terminate(ctx, wrapUpOutcome(l.Run.WrapUpPending))
			return nil
		}
		if l.reconcilePlanIfUnfinished() {
			_, err := l.Run.TransitionTo(domain.PhasePreparing)
			return err
		}
		if l.continueGoalIfActive() {
			_, err := l.Run.TransitionTo(domain.PhasePreparing)
			return err
		}
		l.terminate(ctx, domain.OutcomeSucceeded)
		return nil
	case domain.StopMaxOutput:
		l.maxOutputStops++
		if l.inRunBudgetWrapUp() {
			// Even the salvage turn overflowed the cap: accept whatever
			// text it produced and land — otherwise a persistently
			// verbose model would loop here forever.
			l.terminate(ctx, wrapUpOutcome(l.Run.WrapUpPending))
			return nil
		}
		// The first output-cap truncation lets the model continue — the
		// partial text stays in the transcript and the next call resumes.
		// A consecutive repeat means the model is not converging: force
		// the soft-landing salvage turn (tools denied) so the run ends
		// with a conclusion instead of another paid truncation.
		if l.maxOutputStops >= maxOutputContinuationLimit && l.Run.WrapUpPending == "" {
			l.startBudgetWrapUp([]string{dimensionMaxOutput})
		}
		if _, err := l.Run.TransitionTo(domain.PhasePreparing); err != nil {
			return err
		}
		return nil
	case domain.StopContentFilter:
		l.terminate(ctx, domain.OutcomeFailed)
		return nil
	default:
		if _, err := l.Run.TransitionTo(domain.PhasePreparing); err != nil {
			return err
		}
		return nil
	}
}

func (l *Loop) routeToolCalls(ctx context.Context) error {
	l.prepared = make(map[domain.ToolCallID]domain.PreparedCall)
	calls := lastToolCalls(l.Run.Messages)
	// Soft-landing guard (docs/CONTEXT_DESIGN.md §4.4.2): during the
	// budget wrap-up turn every tool call is denied outright — never
	// routed to approval — so the run terminates with a paired transcript
	// instead of hanging on an approval prompt.
	if l.inRunBudgetWrapUp() {
		for _, tc := range calls {
			l.recordToolError(ctx, tc, "permission_denied", "run is in budget wrap-up; tool calls are disabled")
		}
		l.terminate(ctx, wrapUpOutcome(l.Run.WrapUpPending))
		return nil
	}
	needsApproval := false
	for _, tc := range calls {
		tool, ok := l.Registry.Lookup(tc.Name)
		if !ok {
			l.recordToolError(ctx, tc, "unknown_tool", fmt.Sprintf("tool %q not found", tc.Name))
			if reason := l.trackToolCall(tc.Name, tc.Arguments); reason != "" {
				return l.terminateRunaway(ctx, reason)
			}
			continue
		}
		prepared, err := tool.Prepare(ctx, tc)
		if err != nil {
			rawHash := rawArgsHash(tc.Arguments)
			l.appendPrepareFailureEvents(tc, rawHash)
			l.recordToolError(ctx, tc, "prepare_failed", prepareErrorMessage(tc, err))
			if reason := l.trackToolCall(tc.Name, tc.Arguments); reason != "" {
				return l.terminateRunaway(ctx, reason)
			}
			continue
		}
		l.Run.appendEvent(domain.EventToolCallPrepared, makeToolCallAuditPayload(prepared))
		// Repeat detection hashes the canonical arguments, not the HMAC
		// call signature (which embeds the unique call ID).
		if reason := l.trackToolCall(tc.Name, prepared.Call.Arguments); reason != "" {
			return l.terminateRunaway(ctx, reason)
		}
		verdict := l.policy().Evaluate(prepared)
		// The grant is policy-decided, not model-influenced: it rides the
		// prepared call into Execute (run_cmd maps it onto the sandbox).
		prepared.Grant = verdict.Grant
		switch verdict.Decision {
		case domain.DecisionAllow:
			l.prepared[tc.ID] = prepared
		case domain.DecisionAsk:
			l.prepared[tc.ID] = prepared
			needsApproval = true
		case domain.DecisionDeny:
			l.recordToolError(ctx, tc, "permission_denied", "tool call denied by policy")
		default:
			l.recordToolError(ctx, tc, "permission_denied", "tool call denied by invalid policy decision")
		}
	}

	if needsApproval {
		_, err := l.Run.TransitionTo(domain.PhaseAwaitingApproval)
		return err
	}
	if len(l.prepared) == 0 {
		_, err := l.Run.TransitionTo(domain.PhasePreparing)
		return err
	}
	_, err := l.Run.TransitionTo(domain.PhaseExecutingTools)
	return err
}

// appendPrepareFailureEvents keeps the event stream paired for calls that
// fail during preparation: without them consumers see an execution
// completion with no matching prepared/started events
// (docs/CONTEXT_DESIGN.md §4.6). The degraded payload carries the raw
// args hash so runaway repeat detection and audits can still correlate.
func (l *Loop) appendPrepareFailureEvents(tc domain.ToolCall, argsRawHash string) {
	payload := toolCallAuditPayload{
		CallID:        tc.ID,
		Tool:          tc.Name,
		Risk:          domain.R0,
		ArgsRawHash:   argsRawHash,
		PrepareFailed: true,
	}
	l.Run.appendEvent(domain.EventToolCallPrepared, payload)
	l.Run.appendEvent(domain.EventToolExecutionStarted, payload)
}

// prepareErrorMessage renders the model-facing prepare failure. For
// malformed-arguments placeholders it surfaces the embedded hint instead
// of the internal placeholder field name (docs/CONTEXT_DESIGN.md §4.6).
func prepareErrorMessage(tc domain.ToolCall, err error) string {
	if hint, ok := malformedArgumentsHint(tc.Arguments); ok {
		return hint
	}
	return err.Error()
}

// malformedArgumentsHint extracts the model-facing guidance embedded in
// the malformed-arguments placeholder (stream_hooks.go), so the model
// sees actionable advice instead of `json: unknown field
// "__malformed_arguments"` — a field it never sent.
func malformedArgumentsHint(raw json.RawMessage) (string, bool) {
	var placeholder map[string]json.RawMessage
	if err := json.Unmarshal(raw, &placeholder); err != nil {
		return "", false
	}
	rawBad, hasBad := placeholder["__malformed_arguments"]
	hintRaw, hasHint := placeholder["error"]
	if !hasBad || !hasHint {
		return "", false
	}
	var hint string
	if err := json.Unmarshal(hintRaw, &hint); err != nil || hint == "" {
		return "", false
	}
	var bad string
	if err := json.Unmarshal(rawBad, &bad); err == nil && bad != "" {
		hint += fmt.Sprintf(" (received: %s)", domain.TruncateAtRuneBoundary(bad, 200))
	}
	return hint, true
}

// terminateRunaway closes the dangling calls of the current batch (the
// transcript must stay replayable) and terminates the run as failed.
func (l *Loop) terminateRunaway(ctx context.Context, reason string) error {
	if l.Logger != nil {
		l.Logger.Warn("terminating runaway run", "reason", reason)
	}
	l.closeUnresolvedCalls(ctx, reason+"; the call was not executed")
	l.terminate(ctx, domain.OutcomeFailed)
	return errors.New(reason)
}

func (l *Loop) policy() Policy {
	if l.Policy == nil {
		return DefaultPolicy{}
	}
	return l.Policy
}

func (l *Loop) awaitApproval(ctx context.Context) error {
	if l.Approver == nil {
		return fmt.Errorf("approver required for risky tool calls")
	}
	// Approval waits are user thinking time, not agent activity: pause
	// the stall watchdog by shifting its baseline forward on the way out
	// (docs/CONTEXT_DESIGN.md §4.4.3).
	suspendStart := l.Run.Clock.Now()
	defer func() {
		if !l.lastProgressAt.IsZero() {
			l.lastProgressAt = l.lastProgressAt.Add(l.Run.Clock.Since(suspendStart))
		}
	}()
	for _, tc := range lastToolCalls(l.Run.Messages) {
		prepared, ok := l.prepared[tc.ID]
		if !ok || l.policy().Evaluate(prepared).Decision != domain.DecisionAsk {
			continue
		}
		// The durable permission event ID is the approval ID. Reusing it for
		// the live request binds the UI decision to the persisted audit fact.
		approvalEvent := l.Run.appendEvent(domain.EventPermissionRequested, makeToolCallAuditPayload(prepared))
		if l.traceRun != nil {
			l.traceRun.RecordEvent(ctx, "approval.requested", map[string]string{
				"tool": prepared.Call.Name, "risk": fmt.Sprintf("R%d", int(prepared.Risk)),
				"call_id": tc.ID.String(), "description": prepared.ApprovalDesc,
			})
		}
		if l.Store != nil {
			if err := l.flushEvents(ctx); err != nil {
				l.terminate(ctx, domain.OutcomeFailed)
				return err
			}
		}
		decision, err := l.Approver.RequestApproval(ctx, domain.ApprovalRequest{
			ID:          approvalEvent.ID,
			Call:        prepared,
			Description: prepared.ApprovalDesc,
		})
		if err != nil {
			// Close every call that never reached a decision before failing the
			// run: providers reject replayed transcripts containing tool calls
			// without results, so an approval error must not strand them.
			l.closeUnresolvedCalls(ctx, fmt.Sprintf("approval request failed before execution (%v); the call was not executed", err))
			l.terminate(ctx, domain.OutcomeFailed)
			return fmt.Errorf("request approval for %s: %w", tc.ID, err)
		}
		l.Run.appendEvent(domain.EventPermissionResolved, permissionResolvedPayload{
			CallID:   prepared.Call.ID,
			ArgsHash: prepared.ArgsHash,
			Decision: decision,
		})
		if l.traceRun != nil {
			l.traceRun.RecordEvent(ctx, "approval.resolved", map[string]string{
				"tool": prepared.Call.Name, "call_id": tc.ID.String(), "decision": string(decision),
			})
		}
		if decision != domain.DecisionAllow {
			delete(l.prepared, tc.ID)
			l.recordToolError(ctx, tc, "permission_denied", "tool call denied by policy")
		}
	}
	if len(l.prepared) == 0 {
		_, err := l.Run.TransitionTo(domain.PhasePreparing)
		return err
	}
	_, err := l.Run.TransitionTo(domain.PhaseExecutingTools)
	return err
}

// maxConcurrentToolExecs bounds parallel executions within one
// concurrent-safe segment: delegated child runs are full model loops,
// so unbounded fan-out would multiply provider pressure
// (docs/SUBAGENT_DESIGN.md §11).
const maxConcurrentToolExecs = 4

// maxOutputContinuationLimit is how many consecutive output-cap
// truncations a run tolerates before the salvage wrap-up fires: one
// free continuation, then conclude.
const maxOutputContinuationLimit = 2

// preparedExec is one validated call queued for execution.
type preparedExec struct {
	prepared domain.PreparedCall
	tool     domain.Tool
}

func (l *Loop) executeTools(ctx context.Context) error {
	var batch []preparedExec
	for _, tc := range lastToolCalls(l.Run.Messages) {
		// Do not replay tool executions that already produced a result.
		if l.isToolResultRecorded(tc.ID) {
			continue
		}

		prepared, ok := l.prepared[tc.ID]
		if !ok {
			continue
		}

		tool, ok := l.Registry.Lookup(prepared.Call.Name)
		if !ok {
			l.recordToolError(ctx, tc, string(domain.ErrSecurity), "tool registry drift detected before execution")
			continue
		}
		if err := validatePreparedExecution(tc, prepared, tool.Definition()); err != nil {
			l.recordToolExecutionError(ctx, tc, err)
			continue
		}
		if err := verifyPreparedFreshness(ctx, tool, tc, prepared); err != nil {
			l.recordToolExecutionError(ctx, tc, err)
			continue
		}
		batch = append(batch, preparedExec{prepared: prepared, tool: tool})
	}

	// Consecutive concurrent-safe calls form parallel segments; everything
	// else executes one at a time, in batch order. Segment boundaries keep
	// ordering semantics strict: a write never overlaps a read, and every
	// segment runs after the previous one fully completed
	// (docs/SUBAGENT_DESIGN.md §11).
	for _, segment := range segmentBatch(batch) {
		var err error
		switch {
		case len(segment) > 1:
			err = l.executeSegmentParallel(ctx, segment)
		case len(segment) == 1:
			err = l.executeOne(ctx, segment[0])
		}
		if err != nil {
			return err
		}
	}

	// After tools, prepare the next turn; the loop entry decides compaction.
	l.prepared = nil
	l.drainGoalUpdates()
	l.drainPlanUpdates()
	l.reportContextUsage()
	_, err := l.Run.TransitionTo(domain.PhasePreparing)
	return err
}

// segmentBatch splits the batch into maximal runs of consecutive
// concurrent-safe calls. Singletons and unsafe runs execute serially.
func segmentBatch(batch []preparedExec) [][]preparedExec {
	var segments [][]preparedExec
	for _, item := range batch {
		if domain.ToolConcurrentSafe(item.tool) && len(segments) > 0 {
			if last := segments[len(segments)-1]; domain.ToolConcurrentSafe(last[0].tool) {
				segments[len(segments)-1] = append(last, item)
				continue
			}
		}
		segments = append(segments, []preparedExec{item})
	}
	return segments
}

// executeOne runs a single call with the classic per-call durability:
// the started event is persisted right before execution.
func (l *Loop) executeOne(ctx context.Context, item preparedExec) error {
	l.Run.appendEvent(domain.EventToolExecutionStarted, makeToolCallAuditPayload(item.prepared))
	if l.Store != nil {
		if err := l.flushEvents(ctx); err != nil {
			// Same dangling-call guarantee as the approval path: close the
			// not-yet-executed calls so the transcript stays replayable.
			l.closeUnresolvedCalls(ctx, fmt.Sprintf("execution interrupted before completion (%v); the call may not have run", err))
			l.terminate(ctx, domain.OutcomeFailed)
			return err
		}
	}
	result := item.tool.Execute(ctx, item.prepared)
	l.recordToolOutcome(ctx, item, result)
	if reason := l.trackExecResult(result); reason != "" {
		return l.terminateRunaway(ctx, reason)
	}
	return nil
}

// executeSegmentParallel fans one concurrent-safe segment out with
// bounded parallelism. The durability guarantee matches the serial path:
// EVERY started event is persisted before ANY side effect begins, so
// crash recovery reconciles the whole segment from the same evidence.
// The run projection stays single-threaded — results are recorded
// serially in call order, keeping the transcript, the event sequence,
// and the runaway counters deterministic.
func (l *Loop) executeSegmentParallel(ctx context.Context, segment []preparedExec) error {
	for _, item := range segment {
		l.Run.appendEvent(domain.EventToolExecutionStarted, makeToolCallAuditPayload(item.prepared))
	}
	if l.Store != nil {
		if err := l.flushEvents(ctx); err != nil {
			l.closeUnresolvedCalls(ctx, fmt.Sprintf("execution interrupted before completion (%v); the call may not have run", err))
			l.terminate(ctx, domain.OutcomeFailed)
			return err
		}
	}

	results := make([]domain.ToolResult, len(segment))
	sem := make(chan struct{}, maxConcurrentToolExecs)
	var wg sync.WaitGroup
	for i, item := range segment {
		wg.Add(1)
		go func() {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			results[i] = item.tool.Execute(ctx, item.prepared)
		}()
	}
	wg.Wait()

	for i, item := range segment {
		l.recordToolOutcome(ctx, item, results[i])
		if reason := l.trackExecResult(results[i]); reason != "" {
			return l.terminateRunaway(ctx, reason)
		}
	}
	return nil
}

// recordToolOutcome records one execution's result through the standard
// serial chain: transcript message, trace record, external usage fold,
// file-change audit. Runaway accounting (trackExecResult) is the
// caller's job so it can control ordering and early termination.
func (l *Loop) recordToolOutcome(ctx context.Context, item preparedExec, result domain.ToolResult) {
	l.Run.RecordToolResult(result)
	l.recordTool(ctx, item.prepared, result)
	l.foldExternalUsage(result)
	if changed, ok := extractFileChanged(result, item.prepared); ok {
		l.markProgress()
		l.Run.appendEvent(domain.EventFileChanged, changed)
		// Record the file change directly into the ledger with beforeContent
		// from the PreparedCall's Recovery spec. This captures the original
		// content for rewind, which the event stream alone cannot carry.
		if l.Store != nil {
			beforeExisted := changed.OldHash != ""
			var beforeContent []byte
			if item.prepared.Recovery != nil {
				beforeContent = item.prepared.Recovery.BeforeContent
			}
			if err := l.Store.RecordFileChange(ctx, l.Run.SessionID, changed.Path, beforeExisted, changed.OldHash, beforeContent, changed.NewHash); err != nil {
				if l.Logger != nil {
					l.Logger.Warn("record file change in ledger", "path", changed.Path, "error", err)
				}
			}
		}
	}
}

// shouldCompact decides whether the run compacts before the next model
// call. Compaction is unbounded in count and triggers on two pressures:
//   - forced: a provider context-overflow (or manual request) demands a
//     pass on a fresh window;
//   - occupancy pressure: the calibrated next-request size reaches the
//     window-derived trigger. The byte-estimate alone never triggers —
//     only the provider-calibrated occupancy does
//     (docs/CONTEXT_DESIGN.md §4.2).
func (l *Loop) shouldCompact() bool {
	if l.ForceCompact {
		return true
	}
	if !l.Window.Usable() {
		return false
	}
	if l.lastCompactEst > 0 && estTokens(l.Run.Messages) <= l.lastCompactEst {
		// Nothing grew since the last pass; another pass cannot shrink the
		// transcript either, so let the run proceed to the next model call.
		return false
	}
	return l.contextOccupancy() >= l.Window.CompactTrigger
}

// contextOccupancy estimates the size of the next model request in
// provider-token scale: the metered input of the last completed call (which
// covered system prompt, tools and the whole transcript) plus a byte/4
// estimate of everything appended since. Before the first call — and right
// after a compaction reset — it is the pure transcript estimate.
func (l *Loop) contextOccupancy() int64 {
	if l.lastCallInput == 0 {
		return int64(estTokens(l.Run.Messages))
	}
	occupancy := l.lastCallInput
	messages := l.Run.Messages
	lastAssistant := -1
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == domain.RoleAssistant {
			lastAssistant = i
			break
		}
	}
	return occupancy + int64(estTokens(messages[lastAssistant+1:]))
}

// contextOverflowNeedles fingerprints provider context-window rejections
// across OpenAI-compatible APIs.
var contextOverflowNeedles = []string{
	"context length", "context_length", "context window", "maximum context",
	"prompt is too long", "too many tokens", "request too large",
	"input is too long", "reduce the length", "exceeds the context",
	"context overflow",
}

func isContextOverflowError(err error) bool {
	if err == nil {
		return false
	}
	msg := strings.ToLower(err.Error())
	for _, needle := range contextOverflowNeedles {
		if strings.Contains(msg, needle) {
			return true
		}
	}
	return false
}

// handleContextOverflow degrades a provider context-window rejection into a
// forced compaction plus retry instead of killing the run. Two consecutive
// failures to fit mean the window genuinely cannot hold the request.
func (l *Loop) handleContextOverflow(ctx context.Context, cause error) error {
	l.compactFitFailures++
	if l.compactFitFailures >= 2 {
		l.terminate(ctx, domain.OutcomeBudgetExhausted)
		return fmt.Errorf("model rejected the request for context size twice in a row (last: %v); start a new session or check the model's context_window configuration", cause)
	}
	l.ForceCompact = true
	l.CompactTriggerHint = "overflow"
	if l.Logger != nil {
		l.Logger.Warn("provider rejected request for context size; forcing compaction", "error", cause)
	}
	_, err := l.Run.TransitionTo(domain.PhasePreparing)
	return err
}

// summarizeForCompaction asks the model to write a handoff summary of the
// current transcript (checkpoint compaction). The call is internal: no
// tools are offered and stream hooks are suppressed so the UI never sees
// the bookkeeping turn.
func (l *Loop) summarizeForCompaction(ctx context.Context) (string, error) {
	messages := append([]domain.Message(nil), l.Run.Messages...)
	messages = append(messages, domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser, Status: domain.MessageStatusFinal,
		Revision: 1, Sequence: int64(len(messages) + 1),
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: CompactionSummonPrompt}},
		CreatedAt: l.Run.Clock.Now(),
	})
	modelName := l.ModelName
	if modelName == "" {
		modelName = "default"
	}
	startedAt := l.Run.Clock.Now()
	req := domain.ModelRequest{
		ID: domain.NewEventID(), ModelName: modelName, Messages: messages,
		MaxTokens: l.Run.Limits.MaxOutputTokens,
	}
	stream, err := l.Model.Stream(ctx, req)
	if err != nil {
		return "", err
	}
	defer stream.Close()
	agg := NewStreamAggregator(l.Run.Clock, StreamHooks{})
	if err := consumeStream(stream, agg); err != nil {
		return "", err
	}
	response, _, inputTokens, outputTokens, err := agg.Finalize()
	if err != nil {
		return "", err
	}
	l.accountUsage(inputTokens, outputTokens)
	l.recordGeneration(ctx, trace.GenerationRecord{
		RequestID: req.ID.String(), Turn: l.Run.Usage.Turns, Model: modelName,
		Input: messages, Output: response, StopReason: "compaction",
		InputTokens: inputTokens, OutputTokens: outputTokens,
		StartTime: startedAt, EndTime: l.Run.Clock.Now(),
	})
	text := strings.Join(response.TextParts(), "")
	if strings.TrimSpace(text) == "" {
		return "", fmt.Errorf("model produced an empty compaction summary")
	}
	return text, nil
}

// accountUsage folds one model call's metered tokens into the run budget:
// cumulative token counts, the estimated cost (when rates are configured —
// without it the cost_usd limit can never fire), the goal meter, and the
// wall-time window.
func (l *Loop) accountUsage(inputTokens, outputTokens int64) {
	l.Run.Usage.InputTokens += inputTokens
	l.Run.Usage.OutputTokens += outputTokens
	if l.CostInputUSDPerMTok > 0 || l.CostOutputUSDPerMTok > 0 {
		l.Run.Usage.CostUSD = float64(l.Run.Usage.InputTokens)*l.CostInputUSDPerMTok/1e6 +
			float64(l.Run.Usage.OutputTokens)*l.CostOutputUSDPerMTok/1e6
	}
	l.Run.touchWallTime()
	if l.Run.Goal != nil && l.Run.Goal.Status == domain.GoalStatusActive {
		l.Run.Goal.TokensUsed += inputTokens + outputTokens
	}
}

// foldExternalUsage accounts externally-metered token usage reported in
// a tool result's metadata (domain.ToolMetaExternalInputTokens /
// ToolMetaExternalOutputTokens) into the run's budget counters. It is
// the delegate_task contract: a sub-agent run consumes tokens outside
// the parent's own model calls, and folding them back keeps delegation
// budget-transparent instead of a loophole (docs/SUBAGENT_DESIGN.md
// §5.2). The accounting path is the same one model calls use, so the
// cost estimate and the goal meter stay consistent.
func (l *Loop) foldExternalUsage(result domain.ToolResult) {
	if result.Metadata == nil {
		return
	}
	inputTokens, _ := strconv.ParseInt(result.Metadata[domain.ToolMetaExternalInputTokens], 10, 64)
	outputTokens, _ := strconv.ParseInt(result.Metadata[domain.ToolMetaExternalOutputTokens], 10, 64)
	if inputTokens <= 0 && outputTokens <= 0 {
		return
	}
	l.accountUsage(inputTokens, outputTokens)
	l.Run.appendEvent(domain.EventBudgetUpdated, l.Run.Usage)
}

// closeUnresolvedCalls records an interrupted error result for every tool
// call of the latest assistant message that has no recorded result yet. It is
// the guard that keeps the transcript replayable (providers reject dangling
// tool calls) when a run fails between routing and execution.
func (l *Loop) closeUnresolvedCalls(ctx context.Context, message string) {
	for _, tc := range lastToolCalls(l.Run.Messages) {
		if l.isToolResultRecorded(tc.ID) {
			continue
		}
		l.recordToolError(ctx, tc, "interrupted", message)
	}
}

// lastToolCalls returns the tool calls of the most recent message carrying
// any. Bookkeeping messages (budget notices, compaction markers) may be
// appended after the assistant message that owns the calls, so readers must
// never assume it sits at the transcript tail.
func lastToolCalls(messages []domain.Message) []domain.ToolCall {
	for i := len(messages) - 1; i >= 0; i-- {
		if calls := messages[i].ToolCalls(); len(calls) > 0 {
			return calls
		}
	}
	return nil
}

func (l *Loop) compact(ctx context.Context) error {
	cond := l.Condenser.withDefaults()
	if !cond.Window.Usable() {
		cond.Window = l.Window
	}
	trigger := l.CompactTriggerHint
	if trigger == "" {
		trigger = "auto"
	}
	phase := "pre_turn"
	if l.Run.Usage.Turns > 0 {
		phase = "mid_turn"
	}
	occupancyBefore := l.contextOccupancy()
	tokensBefore := estTokens(l.Run.Messages)
	result := cond.Condense(ctx, &l.Run.Messages, l.Artifacts)
	tokensAfter := estTokens(l.Run.Messages)

	// Level 3: when mechanical masking cannot reach the target, ask the
	// model for a handoff summary and rebuild the transcript around it.
	// Failure degrades to the masked history — compaction must not kill
	// the run.
	summarized := false
	summaryBytes := 0
	truncatedUserMessages := 0
	if tokensAfter > cond.target() {
		summary, err := l.summarizeForCompaction(ctx)
		// The summarize request itself can overflow: drop the oldest
		// pairing-safe span and retry (codex remove_first_item style).
		for attempts := 0; err != nil && isContextOverflowError(err) && attempts < maxCompactionSummaryRetries; attempts++ {
			if !l.dropOldestForCompactionRetry() {
				break
			}
			summary, err = l.summarizeForCompaction(ctx)
		}
		if err != nil {
			if l.Logger != nil {
				l.Logger.Warn("summarizing compaction failed; keeping masked history", "error", err)
			}
		} else {
			var dropped int
			l.Run.Messages, dropped = buildSummaryReplacement(l.Run.Messages, summary, l.Run.Clock.Now(), cond.userMessageBudget())
			truncatedUserMessages = dropped
			summarized = true
			summaryBytes = len(summary)
			tokensAfter = estTokens(l.Run.Messages)
		}
	}

	// Fresh window: the next request's size is the pure estimate again,
	// and occupancy notices re-arm (other dimensions never do). Remember
	// the post-pass estimate so shouldCompact only re-fires on real
	// transcript growth.
	l.lastCallInput = 0
	l.ForceCompact = false
	l.CompactTriggerHint = ""
	if l.noticeFired != nil {
		delete(l.noticeFired, dimensionOccupancy)
	}
	l.lastCompactEst = estTokens(l.Run.Messages)

	l.Run.appendEvent(domain.EventContextCompacted, contextCompactedPayload{
		Trigger:               trigger,
		Phase:                 phase,
		MaskedOutputs:         len(result.outputs),
		MaskedBytes:           result.bytesMasked,
		ArchivedMessages:      result.archived,
		EstTokensBefore:       tokensBefore,
		EstTokensAfter:        tokensAfter,
		OccupancyBefore:       occupancyBefore,
		OccupancyAfter:        int64(tokensAfter),
		Summarized:            summarized,
		SummaryBytes:          summaryBytes,
		TruncatedUserMessages: truncatedUserMessages,
		Outputs:               result.outputs,
	})
	if l.Logger != nil && (len(result.outputs) > 0 || result.archived > 0 || summarized) {
		l.Logger.Info("context compacted",
			"trigger", trigger,
			"phase", phase,
			"masked_outputs", len(result.outputs),
			"masked_bytes", result.bytesMasked,
			"archived_messages", result.archived,
			"summarized", summarized,
			"est_tokens_before", tokensBefore,
			"est_tokens_after", tokensAfter)
	}
	if l.traceRun != nil {
		l.traceRun.RecordEvent(ctx, "context.compacted", map[string]string{
			"trigger":           trigger,
			"phase":             phase,
			"masked_outputs":    fmt.Sprintf("%d", len(result.outputs)),
			"masked_bytes":      fmt.Sprintf("%d", result.bytesMasked),
			"archived_messages": fmt.Sprintf("%d", result.archived),
			"summarized":        fmt.Sprintf("%t", summarized),
			"est_tokens_before": fmt.Sprintf("%d", tokensBefore),
			"est_tokens_after":  fmt.Sprintf("%d", tokensAfter),
		})
	}

	// Fit check: a compaction that leaves occupancy at or above the window
	// counts as a fit failure; the next successful call resets it.
	if l.Window.Usable() && l.contextOccupancy() >= l.Window.Effective {
		l.compactFitFailures++
		if l.compactFitFailures >= 2 {
			l.terminate(ctx, domain.OutcomeBudgetExhausted)
			return fmt.Errorf("context still occupies ~%d tokens after repeated compactions (effective window %d); start a new session", l.contextOccupancy(), l.Window.Effective)
		}
	}
	_, err := l.Run.TransitionTo(domain.PhasePreparing)
	return err
}

// maxCompactionSummaryRetries bounds the drop-oldest retries when the
// compaction summarize request itself overflows the window.
const maxCompactionSummaryRetries = 3

// dropOldestForCompactionRetry drops the smallest pairing-safe prefix of
// the transcript so the summarize request can fit the window. It reports
// whether anything was dropped. Messages dropped here have already passed
// Level-1 masking and (when eligible) Level-2 archival in this pass.
func (l *Loop) dropOldestForCompactionRetry() bool {
	msgs := l.Run.Messages
	if len(msgs) <= 1 {
		return false
	}
	cut := pairingSafeCut(msgs, 1, len(msgs)-1)
	if cut == 0 {
		return false
	}
	remaining := append([]domain.Message(nil), msgs[cut:]...)
	for i := range remaining {
		remaining[i].Sequence = int64(i + 1)
	}
	l.Run.Messages = remaining
	if l.Logger != nil {
		l.Logger.Warn("compaction summarize overflow; dropped oldest messages", "dropped", cut)
	}
	return true
}

// ManagedPromptInfo is implemented by prompt builders backed by Langfuse
// Prompt Management; generations link to the exact managed revision.
type ManagedPromptInfo interface {
	ManagedPromptInfo() (name string, version int, ok bool)
}

// recordGeneration reports a completed model call to the trace recorder,
// enriching it with the managed-prompt identity when present.
func (l *Loop) recordGeneration(ctx context.Context, rec trace.GenerationRecord) {
	if mp, ok := l.SystemPrompt.(ManagedPromptInfo); ok {
		if name, version, managed := mp.ManagedPromptInfo(); managed {
			rec.PromptName, rec.PromptVersion = name, version
		}
	}
	if l.traceRun != nil {
		l.traceRun.RecordGeneration(ctx, rec)
	}
}

// recordTool reports a completed tool execution to the trace recorder.
func (l *Loop) recordTool(ctx context.Context, prepared domain.PreparedCall, result domain.ToolResult) {
	if l.traceRun == nil {
		return
	}
	rec := trace.ToolRecord{
		CallID:    prepared.Call.ID.String(),
		Name:      prepared.Call.Name,
		Risk:      fmt.Sprintf("R%d", int(prepared.Risk)),
		Arguments: prepared.Call.Arguments,
		Status:    string(result.Status),
		Preview:   toolResultTracePreview(result, 500),
		StartTime: result.StartedAt,
		EndTime:   result.FinishedAt,
	}
	if result.Error != nil {
		rec.Code = result.Error.Code
		rec.Error = result.Error.Message
	}
	l.traceRun.RecordTool(ctx, rec)
}

// toolResultTracePreview returns a short excerpt of the first text part for
// the tool span output. The cut backs off to a rune boundary: multi-byte
// UTF-8 characters (e.g. CJK at 3 bytes) must never be split — span
// attributes go through OTLP protobuf validation, which rejects invalid
// UTF-8, and a replacement char mid-text renders as mojibake in the UI.
func toolResultTracePreview(result domain.ToolResult, maxLen int) string {
	for _, cp := range result.Content {
		if cp.Kind != domain.PartText {
			continue
		}
		text := strings.TrimSpace(cp.Text)
		if len(text) > maxLen {
			text = cutAtRuneBoundary(text, maxLen) + "…"
		}
		return text
	}
	return ""
}

// cutAtRuneBoundary returns the longest prefix of s within maxBytes that
// does not split a multi-byte UTF-8 character.
func cutAtRuneBoundary(s string, maxBytes int) string {
	return domain.TruncateAtRuneBoundary(s, maxBytes)
}

// lastAssistantText returns the text of the most recent assistant message,
// used as the trace output when the run ends.
func lastAssistantText(messages []domain.Message) string {
	for i := len(messages) - 1; i >= 0; i-- {
		if messages[i].Role == domain.RoleAssistant {
			text := strings.Join(messages[i].TextParts(), "")
			if strings.TrimSpace(text) != "" {
				return text
			}
		}
	}
	return ""
}

func (l *Loop) terminate(ctx context.Context, outcome domain.Outcome) {
	if l.Run.State.Lifecycle == domain.LifecycleTerminal {
		return
	}
	if _, err := l.Run.Terminate(outcome); err != nil {
		if l.Logger != nil {
			l.Logger.Error("terminate failed", "error", err)
		}
		return
	}
	persistCtx := ctx
	cancel := func() {}
	if ctx == nil {
		persistCtx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
	} else if ctx.Err() != nil {
		persistCtx, cancel = context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	}
	defer cancel()
	if err := l.flushEvents(persistCtx); err != nil && l.Logger != nil {
		l.Logger.Error("persist terminal event failed", "error", err)
	}
}

func (l *Loop) flushEvents(ctx context.Context) error {
	if l.Store == nil || len(l.Run.pendingEvents) == 0 {
		return nil
	}
	events := append([]domain.Event(nil), l.Run.pendingEvents...)
	newVersion := l.Run.persistedVersion + int64(len(events))
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: l.Run.SessionID, Sequence: newVersion,
		State: l.Run.State, Messages: append([]domain.Message(nil), l.Run.Messages...),
		Plan: l.Run.Plan, Usage: l.Run.Usage, Goal: cloneGoal(l.Run.Goal), CreatedAt: l.Run.Clock.Now(),
	}
	if err := l.Store.AppendEventsAndCheckpoint(ctx, l.Run.SessionID, l.Run.persistedVersion, events, checkpoint); err != nil {
		return fmt.Errorf("append events and checkpoint: %w", err)
	}
	l.Run.persistedVersion = newVersion
	l.Run.pendingEvents = l.Run.pendingEvents[:0]
	return nil
}

func (l *Loop) isToolResultRecorded(callID domain.ToolCallID) bool {
	for _, msg := range l.Run.Messages {
		for _, p := range msg.Parts {
			if p.Kind == domain.PartToolResult && p.ToolResult != nil && p.ToolResult.CallID == callID {
				return true
			}
		}
	}
	return false
}

// toolCallIDTaken builds a conflict detector over the transcript: it
// reports whether a tool call id already appears as a call or a result.
// Providers whose tool-call ids are per-turn counters (kimi's
// "run_cmd_0", ...) collide across turns; the StreamAggregator rewrites
// such ids so loom-internal pairing stays globally unique.
func (l *Loop) toolCallIDTaken() func(string) bool {
	taken := make(map[string]struct{})
	for _, msg := range l.Run.Messages {
		for _, tc := range msg.ToolCalls() {
			taken[tc.ID.String()] = struct{}{}
		}
		for _, p := range msg.Parts {
			if p.Kind == domain.PartToolResult && p.ToolResult != nil {
				taken[p.ToolResult.CallID.String()] = struct{}{}
			}
		}
	}
	return func(id string) bool {
		_, ok := taken[id]
		return ok
	}
}

// recordToolError persists an error result for a call that never reached
// execution (denied, unknown tool, prepare failure, interruption) and emits
// the matching trace observation: policy denials are security-relevant and
// must not be invisible in Langfuse just because no tool ran.
func (l *Loop) recordToolError(ctx context.Context, tc domain.ToolCall, code, message string) {
	now := l.Run.Clock.Now()
	l.Run.RecordToolResult(domain.ToolResult{
		CallID:     tc.ID,
		Status:     domain.ToolStatusError,
		Error:      &domain.ToolError{Code: code, Message: message},
		StartedAt:  now,
		FinishedAt: now,
	})
	if l.traceRun != nil {
		l.traceRun.RecordTool(ctx, trace.ToolRecord{
			CallID: tc.ID.String(), Name: tc.Name, Arguments: tc.Arguments,
			Status: string(domain.ToolStatusError), Code: code, Error: message,
			StartTime: now, EndTime: now,
		})
	}
}

func (l *Loop) recordToolExecutionError(ctx context.Context, tc domain.ToolCall, err error) {
	var agentErr *domain.AgentError
	if errors.As(err, &agentErr) {
		l.recordToolError(ctx, tc, string(agentErr.Code), agentErr.Message)
		return
	}
	l.recordToolError(ctx, tc, string(domain.ErrInternal), err.Error())
}

func makeToolCallAuditPayload(prepared domain.PreparedCall) toolCallAuditPayload {
	return toolCallAuditPayload{
		CallID:       prepared.Call.ID,
		Tool:         prepared.Definition.Name,
		Risk:         prepared.Risk,
		ArgsHash:     prepared.ArgsHash,
		ReadPaths:    cloneStrings(prepared.ReadPaths),
		WritePaths:   cloneStrings(prepared.WritePaths),
		ApprovalDesc: prepared.ApprovalDesc,
		Recovery:     cloneRecoverySpec(prepared.Recovery),
	}
}

func cloneRecoverySpec(spec *domain.RecoverySpec) *domain.RecoverySpec {
	if spec == nil {
		return nil
	}
	copy := *spec
	return &copy
}

func errorCodeForAudit(err error) string {
	var agentErr *domain.AgentError
	if errors.As(err, &agentErr) {
		return string(agentErr.Code)
	}
	switch {
	case errors.Is(err, context.DeadlineExceeded):
		return string(domain.ErrTimeout)
	case errors.Is(err, context.Canceled):
		return string(domain.ErrCancelled)
	}
	return string(domain.ErrInternal)
}

func validatePreparedExecution(original domain.ToolCall, prepared domain.PreparedCall, current domain.ToolDefinition) error {
	if prepared.Call.ID != original.ID {
		return domain.NewError(domain.ErrSecurity, "prepared call id mismatch")
	}
	if prepared.Call.Name != original.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call name mismatch")
	}
	if prepared.Definition.Name != current.Name || prepared.Definition.Name != prepared.Call.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call definition name mismatch")
	}
	if prepared.Definition.Source != current.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call definition source drift detected")
	}
	// A tool may legitimately elevate the risk tier above the definition's
	// static default based on the call arguments — run_cmd escalates shell
	// or no-sandbox invocations from R2 to R3, and the approval policy
	// already evaluated that elevated tier, so elevation only ever tightens
	// approval. A tier below the definition default is never legitimate: it
	// would weaken the policy the call was prepared (and possibly approved)
	// under. Exact per-argument risk integrity is enforced downstream by the
	// ArgsHash HMAC that Execute re-verifies.
	if prepared.Risk < current.Risk() || prepared.Risk < prepared.Definition.Risk() {
		return domain.NewError(domain.ErrSecurity, "prepared call risk drift detected")
	}
	if !sameCapabilities(prepared.Definition.Capabilities, current.Capabilities) {
		return domain.NewError(domain.ErrSecurity, "prepared call capabilities drift detected")
	}
	// Note: the assistant's raw arguments are intentionally NOT compared
	// byte-for-byte with the prepared (canonical) arguments. Normalization is
	// the Prepare phase's job (e.g. mapping any path spelling onto the
	// workspace-relative display form), so a literal comparison would reject
	// every legitimately normalized call. Semantic freshness is enforced by
	// verifyPreparedFreshness below, and integrity by the ArgsHash HMAC that
	// Execute re-verifies.
	return nil
}

// verifyPreparedFreshness re-runs Prepare on the original tool call and
// compares the canonical arguments. Normalization during Prepare is
// deterministic, so equal canonical forms prove the prepared call still
// reflects the assistant's request; a mismatch means the environment changed
// after approval (or the prepared call drifted) and execution must fail
// closed. The model will see the tool error and may re-issue the call,
// producing a fresh approval bound to current state.
func verifyPreparedFreshness(ctx context.Context, tool domain.Tool, original domain.ToolCall, prepared domain.PreparedCall) error {
	fresh, err := tool.Prepare(ctx, original)
	if err != nil {
		return err
	}
	matched, err := canonicalJSONEqual(fresh.Call.Arguments, prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "failed to canonicalize tool call arguments", domain.WithCause(err))
	}
	if !matched {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments no longer match the current environment")
	}
	return nil
}

func canonicalJSONEqual(left, right json.RawMessage) (bool, error) {
	leftCanonical, err := canonicalizeJSON(left)
	if err != nil {
		return false, err
	}
	rightCanonical, err := canonicalizeJSON(right)
	if err != nil {
		return false, err
	}
	return leftCanonical == rightCanonical, nil
}

func canonicalizeJSON(raw json.RawMessage) (string, error) {
	var value any
	if err := json.Unmarshal(raw, &value); err != nil {
		return "", err
	}
	canonical, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	return string(canonical), nil
}

func extractFileChanged(result domain.ToolResult, prepared domain.PreparedCall) (fileChangedPayload, bool) {
	if result.Status != domain.ToolStatusSuccess || len(prepared.WritePaths) == 0 {
		return fileChangedPayload{}, false
	}
	for _, part := range result.Content {
		if part.Kind != domain.PartText {
			continue
		}
		var decoded fileChangeResult
		if err := json.Unmarshal([]byte(part.Text), &decoded); err != nil {
			continue
		}
		if decoded.Path == "" {
			continue
		}
		return fileChangedPayload{
			CallID:  result.CallID,
			Path:    decoded.Path,
			OldHash: decoded.OldHash,
			NewHash: decoded.NewHash,
			Size:    decoded.Size,
		}, true
	}
	return fileChangedPayload{}, false
}

func cloneStrings(values []string) []string {
	if len(values) == 0 {
		return nil
	}
	out := make([]string, len(values))
	copy(out, values)
	return out
}

func cloneMetadata(values map[string]string) map[string]string {
	if len(values) == 0 {
		return nil
	}
	out := make(map[string]string, len(values))
	for key, value := range values {
		out[key] = value
	}
	return out
}

func sameCapabilities(left, right []domain.Capability) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}

func buildContextManifest(messages []domain.Message, rules []domain.ContextRuleRef) (domain.ContextManifest, error) {
	data, err := json.Marshal(messages)
	if err != nil {
		return domain.ContextManifest{}, err
	}
	sum := sha256.Sum256(data)
	ranges := make([]domain.ContextMessageRange, 0, len(messages))
	for _, msg := range messages {
		ranges = append(ranges, domain.ContextMessageRange{
			MessageID: msg.ID,
			Sequence:  msg.Sequence,
			StartPart: 0,
			EndPart:   len(msg.Parts),
		})
	}
	return domain.NewContextManifest(domain.ContextManifest{
		Rules:         rules,
		MessageRanges: ranges,
		Tokenizer:     domain.TokenizerRef{Name: "provider"},
		PromptHash:    hex.EncodeToString(sum[:]),
	})
}
