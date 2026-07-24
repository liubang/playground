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
	"fmt"
	"log/slog"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
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

	pendingEvents    []domain.Event
	persistedVersion int64
}

// NewRun creates a new Run in the preparing phase.
func NewRun(sessionID domain.SessionID, limits domain.Limits, clock domain.Clock) *Run {
	return &Run{
		ID:        domain.NewRunID(),
		SessionID: sessionID,
		State:     domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		Limits:    limits,
		Clock:     clock,
	}
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
	}
}

// ContinueRun starts a new active run in an existing session from a complete
// terminal checkpoint. The continuation preserves transcript, plan, and usage,
// while using optimistic persistence from the supplied session version.
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
	if checkpoint != nil {
		plan = checkpoint.Plan
		usage = checkpoint.Usage
	}
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
		}
	}
	run := RestoreRun(domain.NewRunID(), sessionID,
		domain.RunState{Lifecycle: domain.LifecycleActive, Phase: domain.PhasePreparing},
		plan, usage, limits, append([]domain.Message(nil), messages...), sessionVersion, clock)
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

// AddAssistantMessage appends an assistant message.
func (r *Run) AddAssistantMessage(msg domain.Message) domain.Event {
	r.normalizeMessage(&msg)
	r.Messages = append(r.Messages, msg)
	r.Version++
	evt := r.newEvent(domain.EventModelResponseCompleted, domain.MessageEventPayload{Message: msg})
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// RecordToolResult records a tool result message.
func (r *Run) RecordToolResult(result domain.ToolResult) domain.Event {
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
	}
	evt := r.newEvent(domain.EventToolExecutionCompleted, payload)
	r.pendingEvents = append(r.pendingEvents, evt)
	return evt
}

// CheckBudget evaluates usage against limits.
func (r *Run) CheckBudget() domain.CheckResult {
	return r.Usage.Check(r.Limits)
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
	ArgsHash     string               `json:"args_hash"`
	ReadPaths    []string             `json:"read_paths,omitempty"`
	WritePaths   []string             `json:"write_paths,omitempty"`
	ApprovalDesc string               `json:"approval_desc,omitempty"`
	Recovery     *domain.RecoverySpec `json:"recovery,omitempty"`
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
}

type permissionResolvedPayload struct {
	CallID   domain.ToolCallID `json:"call_id"`
	ArgsHash string            `json:"args_hash"`
	Decision domain.Decision   `json:"decision"`
}

type toolExecutionCompletedPayload struct {
	CallID     domain.ToolCallID `json:"call_id"`
	Status     domain.ToolStatus `json:"status"`
	ErrorCode  string            `json:"error_code,omitempty"`
	StartedAt  time.Time         `json:"started_at"`
	FinishedAt time.Time         `json:"finished_at"`
	Metadata   map[string]string `json:"metadata,omitempty"`
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

// Policy evaluates the authorization decision for a prepared tool call.
type Policy interface {
	Evaluate(risk domain.RiskLevel) domain.Decision
}

// DefaultPolicy applies the baseline R0/R1 allow, R2/R3 ask, R4 deny policy.
type DefaultPolicy struct{}

func (DefaultPolicy) Evaluate(risk domain.RiskLevel) domain.Decision {
	switch risk {
	case domain.R0, domain.R1:
		return domain.DecisionAllow
	case domain.R2, domain.R3:
		return domain.DecisionAsk
	default:
		return domain.DecisionDeny
	}
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

	prepared map[domain.ToolCallID]domain.PreparedCall
}

// ToolRegistry looks up tools by name.
type ToolRegistry struct {
	mu    sync.RWMutex
	tools map[string]domain.Tool
}

// NewToolRegistry creates a new registry.
func NewToolRegistry() *ToolRegistry {
	return &ToolRegistry{tools: make(map[string]domain.Tool)}
}

// Register adds a validated tool to the registry without allowing replacement.
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

// Lookup returns a tool by name.
func (r *ToolRegistry) Lookup(name string) (domain.Tool, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	t, ok := r.tools[name]
	return t, ok
}

// List returns all registered tool definitions.
func (r *ToolRegistry) List() []domain.ToolDefinition {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]domain.ToolDefinition, 0, len(r.tools))
	for _, t := range r.tools {
		out = append(out, t.Definition())
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// Execute runs the agent loop to completion (or until cancelled).
func (l *Loop) Execute(ctx context.Context) error {
	if err := l.flushEvents(ctx); err != nil {
		return err
	}
	for {
		if err := ctx.Err(); err != nil {
			l.terminate(ctx, domain.OutcomeCancelled)
			return err
		}

		// Budget check
		if check := l.Run.CheckBudget(); check.HasHard() {
			l.terminate(ctx, domain.OutcomeBudgetExhausted)
			return fmt.Errorf("budget exhausted: %v", check.HardBreaches)
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
	return nil
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
		ContextManifest: manifest,
	}

	l.Run.appendEvent(domain.EventModelRequestStarted, modelRequestAuditPayload{
		RequestID: req.ID, ModelName: modelName, ManifestID: manifest.ID,
		ManifestHash: manifest.Hash, PromptHash: manifest.PromptHash,
	})
	if err := l.flushEvents(ctx); err != nil {
		return err
	}
	stream, err := l.Model.Stream(ctx, req)
	if err != nil {
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "start", Code: errorCodeForAudit(err),
		})
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream: %w", err)
	}
	defer stream.Close()

	agg := NewStreamAggregator(l.Run.Clock, l.StreamHooks)
	aggErr := consumeStream(stream, agg)
	if aggErr != nil {
		if agg.HasPartialContent() {
			l.Run.AddAssistantMessage(agg.InterruptedMessage())
		}
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "stream", Code: errorCodeForAudit(aggErr),
		})
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream consumption: %w", aggErr)
	}
	response, stop, inputTokens, outputTokens, err := agg.Finalize()
	if err != nil {
		if agg.HasPartialContent() {
			l.Run.AddAssistantMessage(agg.InterruptedMessage())
		}
		l.Run.appendEvent(domain.EventModelRequestFailed, modelRequestFailedPayload{
			RequestID: req.ID, Stage: "finalize", Code: errorCodeForAudit(err),
		})
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream finalization: %w", err)
	}
	// Record the terminal stream facts on the persisted message so that event
	// consumers (runtime-event bridge, session inspection) can recover the real
	// stop reason and correlate the response with its request.
	if response.Metadata == nil {
		response.Metadata = make(map[string]string, 2)
	}
	response.Metadata["request_id"] = req.ID.String()
	response.Metadata["stop_reason"] = string(stop)
	l.Run.Usage.InputTokens += inputTokens
	l.Run.Usage.OutputTokens += outputTokens
	l.Run.AddAssistantMessage(response)
	l.Run.appendEvent(domain.EventBudgetUpdated, l.Run.Usage)

	if len(response.ToolCalls()) == 0 {
		return l.determineCompletion(ctx, stop)
	}
	return l.routeToolCalls(ctx)
}

// effectiveMessages returns the transcript with the ephemeral system prompt
// prepended, together with the prompt's audit rule references. A build
// failure degrades to the bare transcript rather than failing the turn.
func (l *Loop) effectiveMessages(ctx context.Context) ([]domain.Message, []domain.ContextRuleRef) {
	messages := append([]domain.Message(nil), l.Run.Messages...)
	if l.SystemPrompt == nil {
		return messages, nil
	}
	text, rules, err := l.SystemPrompt.Build(ctx)
	if err != nil {
		if l.Logger != nil {
			l.Logger.Warn("build system prompt failed; continuing without it", "error", err)
		}
		return messages, nil
	}
	if strings.TrimSpace(text) == "" {
		return messages, nil
	}
	system := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleSystem,
		Status:    domain.MessageStatusFinal,
		Revision:  1,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		CreatedAt: l.Run.Clock.Now(),
	}
	return append([]domain.Message{system}, messages...), rules
}

func (l *Loop) determineCompletion(ctx context.Context, stop domain.StopReason) error {
	switch stop {
	case domain.StopEndTurn:
		l.terminate(ctx, domain.OutcomeSucceeded)
		return nil
	case domain.StopMaxOutput:
		// Model hit output limit — let it continue
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
	needsApproval := false
	for _, tc := range l.Run.Messages[len(l.Run.Messages)-1].ToolCalls() {
		tool, ok := l.Registry.Lookup(tc.Name)
		if !ok {
			l.recordToolError(tc.ID, "unknown_tool", fmt.Sprintf("tool %q not found", tc.Name))
			continue
		}
		prepared, err := tool.Prepare(ctx, tc)
		if err != nil {
			l.recordToolError(tc.ID, "prepare_failed", err.Error())
			continue
		}
		l.Run.appendEvent(domain.EventToolCallPrepared, makeToolCallAuditPayload(prepared))
		switch l.policy().Evaluate(prepared.Risk) {
		case domain.DecisionAllow:
			l.prepared[tc.ID] = prepared
		case domain.DecisionAsk:
			l.prepared[tc.ID] = prepared
			needsApproval = true
		case domain.DecisionDeny:
			l.recordToolError(tc.ID, "permission_denied", "tool call denied by policy")
		default:
			l.recordToolError(tc.ID, "permission_denied", "tool call denied by invalid policy decision")
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
	lastMsg := l.Run.Messages[len(l.Run.Messages)-1]
	for _, tc := range lastMsg.ToolCalls() {
		prepared, ok := l.prepared[tc.ID]
		if !ok || l.policy().Evaluate(prepared.Risk) != domain.DecisionAsk {
			continue
		}
		// The durable permission event ID is the approval ID. Reusing it for
		// the live request binds the UI decision to the persisted audit fact.
		approvalEvent := l.Run.appendEvent(domain.EventPermissionRequested, makeToolCallAuditPayload(prepared))
		if l.Store != nil {
			if err := l.flushEvents(ctx); err != nil {
				return err
			}
		}
		decision, err := l.Approver.RequestApproval(ctx, domain.ApprovalRequest{
			ID:          approvalEvent.ID,
			Call:        prepared,
			Description: prepared.ApprovalDesc,
		})
		if err != nil {
			return fmt.Errorf("request approval for %s: %w", tc.ID, err)
		}
		l.Run.appendEvent(domain.EventPermissionResolved, permissionResolvedPayload{
			CallID:   prepared.Call.ID,
			ArgsHash: prepared.ArgsHash,
			Decision: decision,
		})
		if decision != domain.DecisionAllow {
			delete(l.prepared, tc.ID)
			l.recordToolError(tc.ID, "permission_denied", "tool call denied by policy")
		}
	}
	if len(l.prepared) == 0 {
		_, err := l.Run.TransitionTo(domain.PhasePreparing)
		return err
	}
	_, err := l.Run.TransitionTo(domain.PhaseExecutingTools)
	return err
}

func (l *Loop) executeTools(ctx context.Context) error {
	lastMsg := l.Run.Messages[len(l.Run.Messages)-1]
	for _, tc := range lastMsg.ToolCalls() {
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
			l.recordToolError(tc.ID, string(domain.ErrSecurity), "tool registry drift detected before execution")
			continue
		}
		if err := validatePreparedExecution(tc, prepared, tool.Definition()); err != nil {
			l.recordToolExecutionError(tc.ID, err)
			continue
		}
		if err := verifyPreparedFreshness(ctx, tool, tc, prepared); err != nil {
			l.recordToolExecutionError(tc.ID, err)
			continue
		}

		l.Run.appendEvent(domain.EventToolExecutionStarted, makeToolCallAuditPayload(prepared))
		if l.Store != nil {
			if err := l.flushEvents(ctx); err != nil {
				return err
			}
		}

		result := tool.Execute(ctx, prepared)
		l.Run.RecordToolResult(result)
		if changed, ok := extractFileChanged(result, prepared); ok {
			l.Run.appendEvent(domain.EventFileChanged, changed)
		}
	}

	// After tools, either compact (if near budget) or prepare next turn.
	l.prepared = nil
	if check := l.Run.CheckBudget(); check.HasSoft() {
		_, err := l.Run.TransitionTo(domain.PhaseCompacting)
		return err
	}
	_, err := l.Run.TransitionTo(domain.PhasePreparing)
	return err
}

func (l *Loop) compact(ctx context.Context) error {
	// Phase 0: no-op compact, just continue
	_, err := l.Run.TransitionTo(domain.PhasePreparing)
	return err
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
		Plan: l.Run.Plan, Usage: l.Run.Usage, CreatedAt: l.Run.Clock.Now(),
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

func (l *Loop) recordToolError(id domain.ToolCallID, code, message string) {
	l.Run.RecordToolResult(domain.ToolResult{
		CallID:     id,
		Status:     domain.ToolStatusError,
		Error:      &domain.ToolError{Code: code, Message: message},
		StartedAt:  l.Run.Clock.Now(),
		FinishedAt: l.Run.Clock.Now(),
	})
}

func (l *Loop) recordToolExecutionError(id domain.ToolCallID, err error) {
	var agentErr *domain.AgentError
	if domain.As(err, &agentErr) {
		l.recordToolError(id, string(agentErr.Code), agentErr.Message)
		return
	}
	l.recordToolError(id, string(domain.ErrInternal), err.Error())
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
	if domain.As(err, &agentErr) {
		return string(agentErr.Code)
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
	if prepared.Risk != current.Risk() || prepared.Risk != prepared.Definition.Risk() {
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
