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
	"io"
	"log/slog"
	"sort"
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

// IncrementTurn increments the turn counter.
func (r *Run) IncrementTurn() {
	r.Usage.Turns++
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

type toolCallAuditPayload struct {
	CallID       domain.ToolCallID `json:"call_id"`
	Tool         string            `json:"tool"`
	Risk         domain.RiskLevel  `json:"risk"`
	ArgsHash     string            `json:"args_hash"`
	ReadPaths    []string          `json:"read_paths,omitempty"`
	WritePaths   []string          `json:"write_paths,omitempty"`
	ApprovalDesc string            `json:"approval_desc,omitempty"`
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

// Loop drives the main agent loop for a Run.
type Loop struct {
	Run       *Run
	Model     domain.Model
	ModelName string
	Store     domain.SessionStore
	Approver  domain.Approver
	Registry  *ToolRegistry
	Logger    *slog.Logger

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
	manifest, err := buildContextManifest(l.Run.Messages)
	if err != nil {
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("build context manifest: %w", err)
	}
	req := domain.ModelRequest{
		ID:              domain.NewEventID(),
		ModelName:       modelName,
		Messages:        append([]domain.Message(nil), l.Run.Messages...),
		Tools:           l.Registry.List(),
		MaxTokens:       l.Run.Limits.MaxOutputTokens,
		ContextManifest: manifest,
	}

	stream, err := l.Model.Stream(ctx, req)
	if err != nil {
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream: %w", err)
	}
	defer stream.Close()

	response, err := aggregateStream(stream, l.Run.Clock)
	if err != nil {
		if len(response.Message.Parts) > 0 {
			l.Run.AddAssistantMessage(response.Message)
		}
		l.terminate(ctx, domain.OutcomeFailed)
		return fmt.Errorf("model stream aggregation: %w", err)
	}
	l.Run.Usage.InputTokens += response.InputTokens
	l.Run.Usage.OutputTokens += response.OutputTokens
	l.Run.AddAssistantMessage(response.Message)

	if len(response.Message.ToolCalls()) == 0 {
		return l.determineCompletion(ctx, response.StopReason)
	}
	return l.routeToolCalls(ctx)
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
		switch prepared.Risk {
		case domain.R0, domain.R1:
			l.prepared[tc.ID] = prepared
		case domain.R2, domain.R3:
			l.prepared[tc.ID] = prepared
			needsApproval = true
		default:
			l.recordToolError(tc.ID, "permission_denied", "tool call denied by policy")
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

func (l *Loop) awaitApproval(ctx context.Context) error {
	if l.Approver == nil {
		return fmt.Errorf("approver required for risky tool calls")
	}
	lastMsg := l.Run.Messages[len(l.Run.Messages)-1]
	for _, tc := range lastMsg.ToolCalls() {
		prepared, ok := l.prepared[tc.ID]
		if !ok || prepared.Risk < domain.R2 {
			continue
		}
		l.Run.appendEvent(domain.EventPermissionRequested, makeToolCallAuditPayload(prepared))
		if l.Store != nil {
			if err := l.flushEvents(ctx); err != nil {
				return err
			}
		}
		decision, err := l.Approver.RequestApproval(ctx, domain.ApprovalRequest{
			ID:          domain.NewEventID(),
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
	if err := l.flushEvents(ctx); err != nil && l.Logger != nil {
		l.Logger.Error("persist terminal event failed", "error", err)
	}
}

func (l *Loop) flushEvents(ctx context.Context) error {
	if l.Store == nil || len(l.Run.pendingEvents) == 0 {
		return nil
	}
	events := append([]domain.Event(nil), l.Run.pendingEvents...)
	if err := l.Store.AppendEvents(ctx, l.Run.SessionID, l.Run.persistedVersion, events); err != nil {
		return fmt.Errorf("append events: %w", err)
	}
	l.Run.persistedVersion += int64(len(events))
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
	}
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
	matched, err := canonicalJSONEqual(original.Arguments, prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "failed to canonicalize tool call arguments", domain.WithCause(err))
	}
	if !matched {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments no longer match assistant tool call")
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

func buildContextManifest(messages []domain.Message) (domain.ContextManifest, error) {
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
		MessageRanges: ranges,
		Tokenizer:     domain.TokenizerRef{Name: "provider"},
		PromptHash:    hex.EncodeToString(sum[:]),
	})
}

type streamResponse struct {
	Message      domain.Message
	StopReason   domain.StopReason
	InputTokens  int64
	OutputTokens int64
}

// aggregateStream validates and collects canonical model events.
func aggregateStream(stream domain.ModelStream, clock domain.Clock) (response streamResponse, err error) {
	var text string
	defer func() {
		if err == nil || text == "" || len(response.Message.Parts) > 0 {
			return
		}
		response.Message = domain.Message{
			ID:        domain.NewMessageID(),
			Role:      domain.RoleAssistant,
			Status:    domain.MessageStatusInterrupted,
			Revision:  1,
			Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
			CreatedAt: clock.Now(),
		}
	}()
	tools := make(map[int]*streamToolCall)
	seenIDs := make(map[string]struct{})
	var stop domain.StopReason
	var inputTokens, outputTokens int64
	responseEnded := false

	for {
		evt, err := stream.Recv()
		if err != nil {
			if errors.Is(err, io.EOF) && responseEnded {
				break
			}
			return streamResponse{}, fmt.Errorf("stream ended before response_end: %w", err)
		}
		if responseEnded {
			return streamResponse{}, fmt.Errorf("event %q after response_end", evt.Kind)
		}

		switch evt.Kind {
		case domain.ModelEventResponseStart, domain.ModelEventTextStart,
			domain.ModelEventTextEnd, domain.ModelEventProviderWarning:
		case domain.ModelEventTextDelta:
			text += evt.TextDelta
		case domain.ModelEventToolCallStart:
			if _, exists := tools[evt.ToolIndex]; exists {
				return streamResponse{}, fmt.Errorf("duplicate tool index %d", evt.ToolIndex)
			}
			if evt.ToolID == "" || evt.ToolName == "" {
				return streamResponse{}, fmt.Errorf("tool call start requires id and name")
			}
			if _, exists := seenIDs[evt.ToolID]; exists {
				return streamResponse{}, fmt.Errorf("duplicate tool call id %q", evt.ToolID)
			}
			seenIDs[evt.ToolID] = struct{}{}
			tools[evt.ToolIndex] = &streamToolCall{index: evt.ToolIndex, id: evt.ToolID, name: evt.ToolName}
		case domain.ModelEventToolArgsDelta:
			tool, ok := tools[evt.ToolIndex]
			if !ok {
				return streamResponse{}, fmt.Errorf("arguments for unknown tool index %d", evt.ToolIndex)
			}
			tool.args += evt.ToolArgs
		case domain.ModelEventToolCallEnd:
			tool, ok := tools[evt.ToolIndex]
			if !ok {
				return streamResponse{}, fmt.Errorf("end for unknown tool index %d", evt.ToolIndex)
			}
			tool.ended = true
		case domain.ModelEventUsage:
			if evt.InputTokens < 0 || evt.OutputTokens < 0 {
				return streamResponse{}, fmt.Errorf("negative token usage")
			}
			inputTokens, outputTokens = evt.InputTokens, evt.OutputTokens
		case domain.ModelEventStreamError:
			if evt.Error == "" {
				evt.Error = "provider stream error"
			}
			return streamResponse{}, errors.New(evt.Error)
		case domain.ModelEventResponseEnd:
			if evt.StopReason == "" {
				return streamResponse{}, fmt.Errorf("response_end requires stop reason")
			}
			stop = evt.StopReason
			responseEnded = true
		default:
			return streamResponse{}, fmt.Errorf("unknown model event kind %q", evt.Kind)
		}
	}

	indexes := make([]int, 0, len(tools))
	for index, tool := range tools {
		if !tool.ended {
			return streamResponse{}, fmt.Errorf("incomplete tool call at index %d", index)
		}
		indexes = append(indexes, index)
	}
	sort.Ints(indexes)
	parts := make([]domain.ContentPart, 0, len(indexes)+1)
	if text != "" {
		parts = append(parts, domain.ContentPart{Kind: domain.PartText, Text: text})
	}
	for _, index := range indexes {
		tool := tools[index]
		id, err := domain.ParseToolCallID(tool.id)
		if err != nil {
			return streamResponse{}, fmt.Errorf("invalid tool call id %q: %w", tool.id, err)
		}
		call := domain.ToolCall{ID: id, Name: tool.name, Arguments: json.RawMessage(tool.args)}
		if err := call.Validate(); err != nil {
			return streamResponse{}, fmt.Errorf("invalid tool call at index %d: %w", index, err)
		}
		parts = append(parts, domain.ContentPart{PartIndex: len(parts), Kind: domain.PartToolCall, ToolCall: &call})
	}
	if len(parts) == 0 {
		return streamResponse{}, fmt.Errorf("empty model response")
	}
	return streamResponse{
		Message: domain.Message{
			ID:        domain.NewMessageID(),
			Role:      domain.RoleAssistant,
			Status:    domain.MessageStatusFinal,
			Revision:  1,
			Parts:     parts,
			CreatedAt: clock.Now(),
		},
		StopReason: stop, InputTokens: inputTokens, OutputTokens: outputTokens,
	}, nil
}

type streamToolCall struct {
	index int
	id    string
	name  string
	args  string
	ended bool
}
