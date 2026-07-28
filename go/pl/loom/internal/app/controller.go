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
// Created: 2026/07/23

package app

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/render"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// ControllerState represents the high-level state of the SessionController.
type ControllerState string

const (
	ControllerStateBooting          ControllerState = "booting"
	ControllerStateIdle             ControllerState = "idle"
	ControllerStateRunning          ControllerState = "running"
	ControllerStateAwaitingApproval ControllerState = "awaiting_approval"
	ControllerStateCancelling       ControllerState = "cancelling"
	ControllerStateFatal            ControllerState = "fatal"
	ControllerStateClosed           ControllerState = "closed"
)

// Snapshot is a read-only projection of the controller's current state.
type Snapshot struct {
	State         ControllerState  `json:"state"`
	SessionID     domain.SessionID `json:"session_id"`
	RunID         domain.RunID     `json:"run_id,omitempty"`
	ModelName     string           `json:"model_name"`
	ProviderName  string           `json:"provider_name,omitempty"`
	ContextWindow int64            `json:"context_window,omitempty"`
	// ReasoningEffort is the effective reasoning dial ("off"/"low"/... or
	// "budget:N"); empty means the provider decides. ReasoningOverridden
	// marks that it comes from the session override rather than the model's
	// configuration.
	ReasoningEffort     string           `json:"reasoning_effort,omitempty"`
	ReasoningOverridden bool             `json:"reasoning_overridden,omitempty"`
	WorkspaceRoot       string           `json:"workspace_root"`
	TurnCount           int              `json:"turn_count"`
	Usage               domain.Usage     `json:"usage"`
	Messages            []domain.Message `json:"messages,omitempty"`
	PendingApprovals    []domain.EventID `json:"pending_approvals,omitempty"`
	PendingSteers       []string         `json:"pending_steers,omitempty"`
	Timestamp           time.Time        `json:"timestamp"`
}

// SessionSummary is the frontend-safe metadata used by session pickers.
type SessionSummary struct {
	ID        domain.SessionID
	Version   int64
	CreatedAt time.Time
	UpdatedAt time.Time
}

// Controller is the per-frontend-session unique Runtime owner. It serializes
// commands, manages session/turn lifecycle, bridges approvals, and publishes
// runtime events.
//
// Only one active turn is allowed per session.
type Controller struct {
	bootstrap *Bootstrap
	broker    *runtimeevent.Broker
	approver  *ChannelApprover
	// rulesApprover auto-allows calls matching session-persisted rules
	// ("allow always") before delegating to approver. The agent loop sees
	// only this wrapper.
	rulesApprover *RuleApprover
	clock         domain.Clock
	logger        *slog.Logger

	mu          sync.Mutex
	state       ControllerState
	sessionID   domain.SessionID
	runID       domain.RunID
	turnCounter int
	questioner  *ChannelQuestioner
	lastUsage   domain.Usage
	messages    []domain.Message
	resumedRun  *agent.Run
	resumed     bool
	// current overrides Bootstrap.Current after a /model switch; the zero
	// value means the bootstrap default is still in effect. Provider
	// instances are prebuilt, so a switch is just a reference swap applied
	// from the next turn on.
	current config.ProviderModelRef
	// reasoningOverride holds the session-scoped reasoning dial set via
	// /reasoning; nil means the selected model's configured reasoning
	// applies. Independent of model switches by design — it is the user's
	// per-task intent, cleared only by "/reasoning default".
	reasoningOverride *domain.ReasoningSpec
	// forceCompact is the one-shot manual compaction request (/compact):
	// consumed by the next turn's loop construction.
	forceCompact bool

	// sessionCtx is the context for the entire TUI session.
	// Cancelling it terminates the controller.
	sessionCtx    context.Context
	cancelSession context.CancelFunc

	// turnCtx is the context for the current turn.
	// Cancelling it cancels only the current turn.
	turnCtx    context.Context
	cancelTurn context.CancelFunc
	activeTurn uint64
	turnDone   chan struct{}
	nextTurn   uint64
	running    bool

	cmdCh     chan controllerCommand
	doneCh    chan struct{}
	closeOnce sync.Once
}

// ControllerConfig configures a Controller.
type ControllerConfig struct {
	Bootstrap *Bootstrap
	Broker    *runtimeevent.Broker
	Approver  *ChannelApprover
	// Questioner bridges ask_user questions to the frontend; nil disables
	// the bridge (questions then resolve through the bootstrap's
	// questioner, e.g. the autonomous one when headless).
	Questioner *ChannelQuestioner
	Clock      domain.Clock
	Logger     *slog.Logger
}

// NewController creates a new Controller in the booting state.
// Call Run to start the command processing loop.
func NewController(cfg ControllerConfig) *Controller {
	clock := cfg.Clock
	if clock == nil {
		clock = domain.RealClock{}
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	sessionCtx, cancelSession := context.WithCancel(context.Background())
	// Bootstrap is nil in some UI tests; session rules then degrade to a
	// process-local store so "allow always" still works for the run.
	sessionRules := permission.NewSessionRules()
	if cfg.Bootstrap != nil && cfg.Bootstrap.SessionRules != nil {
		sessionRules = cfg.Bootstrap.SessionRules
	}
	c := &Controller{
		bootstrap:     cfg.Bootstrap,
		broker:        cfg.Broker,
		approver:      cfg.Approver,
		rulesApprover: NewRuleApprover(cfg.Approver, sessionRules),
		questioner:    cfg.Questioner,
		clock:         clock,
		logger:        logger,
		state:         ControllerStateBooting,
		sessionCtx:    sessionCtx,
		cancelSession: cancelSession,
		cmdCh:         make(chan controllerCommand, 64),
		doneCh:        make(chan struct{}),
	}
	// Bridge model questions onto the runtime event stream: the agent loop
	// blocks in ask_user until a frontend answers via AnswerQuestion.
	if cfg.Questioner != nil {
		cfg.Questioner.BindPublish(func(q domain.Question) {
			c.mu.Lock()
			sessionID, runID, turn := c.sessionID, c.runID, c.turnCounter
			c.mu.Unlock()
			c.publishEphemeral(sessionID, runID, turn, runtimeevent.KindQuestionAsked, runtimeevent.QuestionAskedPayload{
				QuestionID:    q.ID,
				Text:          q.Text,
				Options:       q.Options,
				AllowMultiple: q.AllowMultiple,
			})
		})
	}
	return c
}

// Run starts the command processing loop. It blocks until the controller
// is shut down.
func (c *Controller) Run(ctx context.Context) {
	c.mu.Lock()
	if c.running {
		c.mu.Unlock()
		c.logger.Error("controller Run called more than once")
		return
	}
	c.running = true
	c.state = ControllerStateIdle
	c.mu.Unlock()

	c.logger.Info("controller started", "session_id", c.sessionID)

	defer func() {
		c.mu.Lock()
		turnDone := c.turnDone
		c.mu.Unlock()
		if turnDone != nil {
			<-turnDone
		}
		c.mu.Lock()
		c.state = ControllerStateClosed
		c.mu.Unlock()
		c.closeOnce.Do(func() { close(c.doneCh) })
		c.logger.Info("controller stopped")
	}()

	for {
		select {
		case <-ctx.Done():
			c.handleShutdown()
			return
		case <-c.sessionCtx.Done():
			c.handleShutdown()
			return
		case cmd := <-c.cmdCh:
			c.dispatch(cmd)
		}
	}
}

// SubmitResult reports how a submitted prompt was accepted.
type SubmitResult struct {
	// Steered is true when the prompt was queued into the active turn's
	// SteerCell instead of starting a new turn immediately.
	Steered bool
	// QueueLen is the resulting pending-steer count (0 when started).
	QueueLen int
}

// SubmitPrompt submits a user prompt. While the controller is idle it
// starts a new turn; while a turn is busy (running, awaiting approval, or
// cancelling) the prompt is queued for steering — the agent loop injects
// it before its next model call, and leftovers become the next turn's
// prompt automatically (docs/STEER_DESIGN.md §3.1).
func (c *Controller) SubmitPrompt(ctx context.Context, prompt string) (SubmitResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, ResultCh: resultCh}:
	case <-ctx.Done():
		return SubmitResult{}, ctx.Err()
	case <-c.doneCh:
		return SubmitResult{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return SubmitResult{}, result.Err
		}
		out, _ := result.Value.(SubmitResult)
		return out, nil
	case <-ctx.Done():
		return SubmitResult{}, ctx.Err()
	case <-c.doneCh:
		return SubmitResult{}, fmt.Errorf("controller is closed")
	}
}

// CancelTurn cancels the current turn.
func (c *Controller) CancelTurn(ctx context.Context) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdCancelTurn, ResultCh: resultCh}:
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		return result.Err
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
}

// ResolveApproval resolves a pending approval only when the frontend binding
// matches the canonical PreparedCall currently awaiting a decision. When
// ruleHint is non-nil and the decision is Allow, the controller derives a
// categorical rule ("allow always") and returns a short note describing it;
// the note is empty when nothing was remembered.
func (c *Controller) ResolveApproval(ctx context.Context, binding ApprovalBinding, decision domain.Decision, ruleHint *ApprovalRuleHint) (string, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdResolveApproval, Approval: binding, Decision: decision, RuleHint: ruleHint, ResultCh: resultCh}:
	case <-ctx.Done():
		return "", ctx.Err()
	case <-c.doneCh:
		return "", fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return "", result.Err
		}
		note, _ := result.Value.(string)
		return note, nil
	case <-ctx.Done():
		return "", ctx.Err()
	case <-c.doneCh:
		return "", fmt.Errorf("controller is closed")
	}
}

// NewSession creates a new session.
func (c *Controller) NewSession(ctx context.Context) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdNewSession, ResultCh: resultCh}:
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		return result.Err
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
}

// AnswerQuestionResult reports the outcome of an AnswerQuestion command:
// Resolved is false when the question was unknown or already answered.
type AnswerQuestionResult struct {
	Resolved bool
}

// AnswerQuestion delivers the frontend's answer to a pending ask_user
// question and publishes the resolution event so every frontend dismisses
// the overlay.
func (c *Controller) AnswerQuestion(ctx context.Context, id domain.EventID, answer domain.QuestionAnswer) (AnswerQuestionResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdAnswerQuestion, QuestionID: id, Answer: answer, ResultCh: resultCh}:
	case <-ctx.Done():
		return AnswerQuestionResult{}, ctx.Err()
	case <-c.doneCh:
		return AnswerQuestionResult{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return AnswerQuestionResult{}, result.Err
		}
		out, _ := result.Value.(AnswerQuestionResult)
		return out, nil
	case <-ctx.Done():
		return AnswerQuestionResult{}, ctx.Err()
	case <-c.doneCh:
		return AnswerQuestionResult{}, fmt.Errorf("controller is closed")
	}
}

// RequestCompactionResult reports the outcome of a RequestCompaction
// command. AlreadyPending is true when a compaction was already scheduled
// and this request changed nothing.
type RequestCompactionResult struct {
	AlreadyPending bool
}

// RequestCompaction schedules a context-compaction pass before the next
// model call. It is the manual counterpart of the loop's automatic pressure
// triggers: the flag is one-shot, consumed by the next turn, and the
// compaction itself is reported through the ContextCompacted runtime event.
func (c *Controller) RequestCompaction(ctx context.Context) (RequestCompactionResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdRequestCompaction, ResultCh: resultCh}:
	case <-ctx.Done():
		return RequestCompactionResult{}, ctx.Err()
	case <-c.doneCh:
		return RequestCompactionResult{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return RequestCompactionResult{}, result.Err
		}
		out, _ := result.Value.(RequestCompactionResult)
		return out, nil
	case <-ctx.Done():
		return RequestCompactionResult{}, ctx.Err()
	case <-c.doneCh:
		return RequestCompactionResult{}, fmt.Errorf("controller is closed")
	}
}

// SetReasoningResult reports the outcome of a SetReasoning command: the
// reasoning spec now in effect and whether it comes from the session
// override (true) or the model's configured default (false).
type SetReasoningResult struct {
	Effective  domain.ReasoningSpec
	Overridden bool
}

// SetReasoning sets or clears the session-scoped reasoning override. The
// argument is one of "off", "low", "medium", "high" (set an override) or
// "default" (clear it and fall back to the selected model's configured
// reasoning). Like /model, the override is in-memory session state: it
// applies from the next turn on and never touches the config file.
func (c *Controller) SetReasoning(ctx context.Context, arg string) (SetReasoningResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSetReasoning, Reasoning: arg, ResultCh: resultCh}:
	case <-ctx.Done():
		return SetReasoningResult{}, ctx.Err()
	case <-c.doneCh:
		return SetReasoningResult{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return SetReasoningResult{}, result.Err
		}
		out, _ := result.Value.(SetReasoningResult)
		return out, nil
	case <-ctx.Done():
		return SetReasoningResult{}, ctx.Err()
	case <-c.doneCh:
		return SetReasoningResult{}, fmt.Errorf("controller is closed")
	}
}

// SetModelResult reports the outcome of a successful SetModel: the
// previous and current selection plus the new model's metadata, so the
// frontend can refresh the status bar (ctx denominator) immediately
// instead of waiting for the next snapshot.
type SetModelResult struct {
	Prev config.ProviderModelRef
	Cur  config.ProviderModelRef
	Meta config.Model
}

// SetModel switches the model used by subsequent turns. ref accepts the
// "provider/model" form or a bare model/provider name (see
// config.ResolveRef). A turn already in flight finishes with the model it
// started on; the new selection applies from the next turn on.
func (c *Controller) SetModel(ctx context.Context, ref string) (SetModelResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSetModel, ModelName: ref, ResultCh: resultCh}:
	case <-ctx.Done():
		return SetModelResult{}, ctx.Err()
	case <-c.doneCh:
		return SetModelResult{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return SetModelResult{}, result.Err
		}
		out, _ := result.Value.(SetModelResult)
		return out, nil
	case <-ctx.Done():
		return SetModelResult{}, ctx.Err()
	case <-c.doneCh:
		return SetModelResult{}, fmt.Errorf("controller is closed")
	}
}

// ResumeSession resumes an existing session.
func (c *Controller) ResumeSession(ctx context.Context, sessionID domain.SessionID) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdResumeSession, SessionID: sessionID, ResultCh: resultCh}:
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		return result.Err
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
}

// RequestSnapshot returns a read-only projection of the current state.
func (c *Controller) RequestSnapshot(ctx context.Context) (Snapshot, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdRequestSnapshot, ResultCh: resultCh}:
	case <-ctx.Done():
		return Snapshot{}, ctx.Err()
	case <-c.doneCh:
		return Snapshot{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return Snapshot{}, result.Err
		}
		if snap, ok := result.Value.(Snapshot); ok {
			return snap, nil
		}
		return Snapshot{}, fmt.Errorf("unexpected snapshot value type")
	case <-ctx.Done():
		return Snapshot{}, ctx.Err()
	case <-c.doneCh:
		return Snapshot{}, fmt.Errorf("controller is closed")
	}
}

// Shutdown initiates a graceful shutdown.
func (c *Controller) Shutdown(ctx context.Context) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdShutdown, ResultCh: resultCh}:
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return result.Err
		}
		select {
		case <-c.doneCh:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	case <-ctx.Done():
		return ctx.Err()
	case <-c.doneCh:
		return nil
	}
}

// Subscribe returns a channel of runtime events and an unsubscribe function.
func (c *Controller) Subscribe() (<-chan runtimeevent.RuntimeEvent, func()) {
	return c.broker.Subscribe()
}

// ListSessions returns recent persisted sessions for a frontend picker.
func (c *Controller) ListSessions(ctx context.Context, limit int) ([]SessionSummary, error) {
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("session listing is unavailable for this store")
	}
	summaries, err := store.ListSessions(ctx, limit)
	if err != nil {
		return nil, err
	}
	result := make([]SessionSummary, len(summaries))
	for i, summary := range summaries {
		result[i] = SessionSummary{ID: summary.ID, Version: summary.Version, CreatedAt: summary.CreatedAt, UpdatedAt: summary.UpdatedAt}
	}
	return result, nil
}

// SessionID returns the current session ID.
func (c *Controller) SessionID() domain.SessionID {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sessionID
}

// State returns the current controller state.
func (c *Controller) State() ControllerState {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.state
}

// Done returns a channel that is closed when the controller is shut down.
func (c *Controller) Done() <-chan struct{} {
	return c.doneCh
}

// --- internal ---

// currentLocked returns the effective provider/model selection. The
// caller must hold c.mu. A nil bootstrap (UI tests) degrades to the zero
// reference.
func (c *Controller) currentLocked() config.ProviderModelRef {
	if c.current != (config.ProviderModelRef{}) {
		return c.current
	}
	if c.bootstrap != nil {
		return c.bootstrap.Current
	}
	return config.ProviderModelRef{}
}

// reasoningLocked returns the effective reasoning spec for the current
// model — the session override when set, otherwise the model's configured
// default — plus whether the override is the source. The caller must hold
// c.mu.
func (c *Controller) reasoningLocked(current config.ProviderModelRef) (domain.ReasoningSpec, bool) {
	if c.reasoningOverride != nil {
		return *c.reasoningOverride, true
	}
	if c.bootstrap != nil && c.bootstrap.Resolved != nil {
		if meta, ok := c.bootstrap.Resolved.ModelMeta(current); ok {
			return meta.Reasoning.DomainSpec(), false
		}
	}
	return domain.ReasoningSpec{}, false
}

// describeReasoning renders a spec for status-bar display; empty means the
// provider decides.
func describeReasoning(spec domain.ReasoningSpec) string {
	if spec.Effort != "" {
		return string(spec.Effort)
	}
	if spec.BudgetTokens > 0 {
		return fmt.Sprintf("budget:%d", spec.BudgetTokens)
	}
	return ""
}

// workspaceLocked returns the workspace root; nil-bootstrap safe.
func (c *Controller) workspaceLocked() string {
	if c.bootstrap != nil {
		return c.bootstrap.WorkspaceRoot
	}
	return ""
}

// steerCellPeek returns the pending steer queue without draining it.
func (c *Controller) steerCellPeek() []string {
	if cell := c.steerCell(); cell != nil {
		return cell.Peek()
	}
	return nil
}

// persistRememberedRules reports whether "allow always" prefixes should be
// written to the user rules layer (rules.persist_remembered; default true).
func (c *Controller) persistRememberedRules() bool {
	if c.bootstrap == nil || c.bootstrap.Resolved == nil {
		return true
	}
	return c.bootstrap.Resolved.Rules.PersistRemembered
}

func (c *Controller) setState(s ControllerState) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.state = s
}

func (c *Controller) getState() ControllerState {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.state
}

func (c *Controller) dispatch(cmd controllerCommand) {
	switch cmd.Kind {
	case cmdSubmitPrompt:
		c.handleSubmitPrompt(cmd)
	case cmdCancelTurn:
		c.handleCancelTurn(cmd)
	case cmdResolveApproval:
		c.handleResolveApproval(cmd)
	case cmdNewSession:
		c.handleNewSession(cmd)
	case cmdResumeSession:
		c.handleResumeSession(cmd)
	case cmdSetModel:
		c.handleSetModel(cmd)
	case cmdSetReasoning:
		c.handleSetReasoning(cmd)
	case cmdRequestCompaction:
		c.handleRequestCompaction(cmd)
	case cmdAnswerQuestion:
		c.handleAnswerQuestion(cmd)
	case cmdRequestSnapshot:
		c.handleRequestSnapshot(cmd)
	case cmdShutdown:
		c.handleShutdown()
		if cmd.ResultCh != nil {
			cmd.ResultCh <- controllerResult{}
		}
	default:
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("unknown command kind %q", cmd.Kind)}
	}
}

func (c *Controller) handleSubmitPrompt(cmd controllerCommand) {
	c.mu.Lock()
	state := c.state
	c.mu.Unlock()
	switch state {
	case ControllerStateRunning, ControllerStateAwaitingApproval, ControllerStateCancelling:
		c.handleSteer(cmd)
		return
	case ControllerStateIdle:
	default:
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot submit prompt in state %q", state)}
		return
	}
	c.mu.Lock()
	if c.sessionID.IsZero() {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("no active session; call NewSession or ResumeSession first")}
		return
	}
	c.state = ControllerStateRunning
	turnCounter := c.turnCounter + 1
	c.turnCounter = turnCounter
	c.nextTurn++
	turnID := c.nextTurn
	c.activeTurn = turnID
	c.mu.Unlock()

	// Publish turn started event
	c.publishDurable(c.sessionID, c.runID, turnCounter, runtimeevent.KindTurnStarted, runtimeevent.TurnStartedPayload{
		TurnIndex: turnCounter,
		Prompt:    cmd.Prompt,
	})

	// Create turn context
	turnCtx, cancelTurn := context.WithCancel(c.sessionCtx)
	turnDone := make(chan struct{})
	c.mu.Lock()
	c.turnCtx = turnCtx
	c.cancelTurn = cancelTurn
	c.turnDone = turnDone
	c.mu.Unlock()

	// Run the loop in a goroutine. Its immutable identity prevents a stale
	// completion from overwriting a newer lifecycle state.
	go func() {
		defer cancelTurn()
		defer close(turnDone)
		err := c.runTurn(turnCtx, cmd.Prompt, turnCounter)
		c.onTurnFinished(turnID, turnCounter, err)
	}()

	cmd.ResultCh <- controllerResult{Value: SubmitResult{}}
}

// handleSteer queues a prompt submitted while a turn is busy into the
// SteerCell and notifies the frontend. A full cell rejects so the UI can
// restore the draft.
func (c *Controller) handleSteer(cmd controllerCommand) {
	cell := c.steerCell()
	if cell == nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot steer without a configured steer cell")}
		return
	}
	if err := cell.Put(cmd.Prompt); err != nil {
		cmd.ResultCh <- controllerResult{Err: err}
		return
	}
	n := cell.Len()
	c.publishEphemeral(c.sessionID, c.runID, c.turnCounter, runtimeevent.KindSteerQueued, runtimeevent.SteerQueuedPayload{
		Text:     cmd.Prompt,
		QueueLen: n,
	})
	cmd.ResultCh <- controllerResult{Value: SubmitResult{Steered: true, QueueLen: n}}
}

// steerCell returns the shared steer mailbox; nil in tests that assemble a
// bare Controller.
func (c *Controller) steerCell() *agent.SteerCell {
	if c.bootstrap == nil {
		return nil
	}
	return c.bootstrap.SteerCell
}

func (c *Controller) runTurn(ctx context.Context, prompt string, turnCounter int) error {
	store := c.bootstrap.Store
	clock := c.clock

	c.mu.Lock()
	run := c.resumedRun
	c.resumedRun = nil
	resumed := c.resumed
	c.resumed = false
	current := c.currentLocked()
	reasoning, _ := c.reasoningLocked(current)
	// A manual /compact request is one-shot: the flag moves onto this turn's
	// loop and is cleared here so later turns compact on pressure only.
	forceCompact := c.forceCompact
	c.forceCompact = false
	c.mu.Unlock()
	provider := c.bootstrap.Resolved.ProviderByName(current.Provider)
	if provider == nil {
		// Cannot happen through Load/SetModel (both validate the ref), but a
		// hand-assembled bootstrap in tests can mismatch — fail the turn
		// loudly instead of panicking on provider.Model.
		return fmt.Errorf("provider %q is not configured", current.Provider)
	}
	modelMeta, _ := c.bootstrap.Resolved.ModelMeta(current)
	if run == nil && turnCounter > 1 {
		var err error
		run, err = c.continueRun(ctx)
		if err != nil {
			return err
		}
	}
	if run == nil {
		run = agent.NewRun(c.sessionID, c.bootstrap.Resolved.Limits, clock)
		// A fresh session may not exist until its first prompt.
		if err := store.CreateSession(ctx, c.sessionID); err != nil {
			c.logger.Debug("create session", "error", err)
		}
	}

	// Persist the initial or recovery continuation event before adding the prompt.
	if err := c.flushRunEvents(ctx, run); err != nil {
		return fmt.Errorf("persist run initialization: %w", err)
	}

	// Add user message
	userMsg := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: prompt}},
		CreatedAt: clock.Now(),
	}
	run.AddUserMessage(userMsg)

	// Persist user message
	if err := c.flushRunEvents(ctx, run); err != nil {
		return fmt.Errorf("persist user message: %w", err)
	}

	// Update controller state and its recoverable transcript projection.
	c.mu.Lock()
	c.runID = run.ID
	c.messages = append([]domain.Message(nil), run.Messages...)
	c.lastUsage = run.Usage
	c.mu.Unlock()

	// Publish session opened when the first turn in this frontend starts.
	if turnCounter == 1 {
		c.publishDurable(c.sessionID, run.ID, turnCounter, runtimeevent.KindSessionOpened, runtimeevent.SessionOpenedPayload{
			Model:        current.String(),
			Workspace:    c.bootstrap.WorkspaceRoot,
			Resumed:      resumed,
			MessageCount: len(run.Messages),
		})
	}

	// Build the loop
	loop := &agent.Loop{
		Run:           run,
		Model:         provider.Model,
		ModelName:     current.Model,
		Store:         &publishingStore{inner: store, broker: c.broker, sessionID: c.sessionID, runID: run.ID, clock: clock, controller: c, previews: make(map[domain.ToolCallID]string), pendingArgs: make(map[domain.ToolCallID]json.RawMessage)},
		Approver:      c.rulesApprover,
		Policy:        c.bootstrap.Policy,
		Registry:      c.bootstrap.Registry,
		Logger:        c.logger,
		SystemPrompt:  c.bootstrap.PromptBuilder,
		Artifacts:     c.bootstrap.Artifact,
		Recorder:      c.bootstrap.Recorder,
		Prompt:        prompt,
		Workspace:     c.bootstrap.WorkspaceRoot,
		ContextWindow: modelMeta.ContextWindow,
		Reasoning:     reasoning,
		ForceCompact:  forceCompact,
		GoalCell:      c.bootstrap.GoalCell,
		PlanCell:      c.bootstrap.PlanCell,
		SteerCell:     c.bootstrap.SteerCell,
		StreamHooks: agent.StreamHooks{
			OnContextUsage: func(estTokens int, lastCallInputTokens int64) {
				c.publishDurable(c.sessionID, run.ID, turnCounter, runtimeevent.KindContextUsage, runtimeevent.ContextUsagePayload{
					EstTokens:           estTokens,
					LastCallInputTokens: lastCallInputTokens,
				})
			},
			OnReasoningDelta: func(delta string) {
				c.publishEphemeral(c.sessionID, run.ID, turnCounter, runtimeevent.KindModelReasoningDelta, runtimeevent.ModelReasoningDeltaPayload{
					RequestID: domain.NewEventID(),
					Delta:     delta,
				})
			},
			OnTextDelta: func(delta string) {
				c.publishEphemeral(c.sessionID, run.ID, turnCounter, runtimeevent.KindModelTextDelta, runtimeevent.ModelTextDeltaPayload{
					RequestID: domain.NewEventID(),
					Delta:     delta,
				})
			},
			OnToolCallDelta: func(toolIndex int, toolID, toolName, args string, deltaBytes int) {
				c.publishEphemeral(c.sessionID, run.ID, turnCounter, runtimeevent.KindModelToolCallDelta, runtimeevent.ModelToolCallDeltaPayload{
					RequestID:  domain.NewEventID(),
					ToolIndex:  toolIndex,
					ToolName:   toolName,
					ToolID:     toolID,
					Arguments:  args,
					DeltaBytes: deltaBytes,
				})
			},
			OnModelUsage: func(inputTokens, outputTokens int64) {
				c.publishDurable(c.sessionID, run.ID, turnCounter, runtimeevent.KindUsageUpdated, runtimeevent.UsageUpdatedPayload{
					InputTokens:  inputTokens,
					OutputTokens: outputTokens,
					Turns:        turnCounter,
				})
			},
		},
	}

	return loop.Execute(ctx)
}

// continueRun starts the next turn from the terminal checkpoint of the active
// session, preserving its transcript and optimistic persistence version.
func (c *Controller) continueRun(ctx context.Context) (*agent.Run, error) {
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("continuing a session requires SQLiteStore")
	}
	inspection, err := store.InspectSession(ctx, c.sessionID)
	if err != nil {
		return nil, fmt.Errorf("inspect session for continuation: %w", err)
	}
	if inspection.Checkpoint == nil {
		return nil, fmt.Errorf("cannot continue session without a checkpoint")
	}
	run, err := agent.ContinueRun(
		*inspection.Checkpoint,
		inspection.Transcript.Messages,
		inspection.Session.Version,
		c.bootstrap.Resolved.Limits,
		c.clock,
	)
	if err != nil {
		return nil, fmt.Errorf("continue session: %w", err)
	}
	return run, nil
}

func (c *Controller) flushRunEvents(ctx context.Context, run *agent.Run) error {
	if len(run.PendingEvents()) == 0 {
		return nil
	}
	events := append([]domain.Event(nil), run.PendingEvents()...)
	newVersion := run.PersistedVersion() + int64(len(events))
	checkpoint := domain.Checkpoint{
		ID:        domain.NewCheckpointID(),
		SessionID: run.SessionID,
		Sequence:  newVersion,
		State:     run.State,
		Messages:  append([]domain.Message(nil), run.Messages...),
		Plan:      run.Plan,
		Usage:     run.Usage,
		CreatedAt: run.Clock.Now(),
	}
	if err := c.bootstrap.Store.AppendEventsAndCheckpoint(ctx, run.SessionID, run.PersistedVersion(), events, checkpoint); err != nil {
		return err
	}
	run.MarkPersisted(newVersion, events)
	c.RecordUsage(run.Usage)
	c.mu.Lock()
	c.messages = append([]domain.Message(nil), run.Messages...)
	c.mu.Unlock()
	return nil
}

func (c *Controller) onTurnFinished(turnID uint64, turn int, err error) {
	c.mu.Lock()
	if err != nil {
		c.logger.Error("turn finished with error", "error", err)
	}
	if c.activeTurn != turnID {
		c.mu.Unlock()
		return
	}
	sessionID, runID := c.sessionID, c.runID
	c.turnCtx = nil
	c.cancelTurn = nil
	if c.state != ControllerStateClosed && c.state != ControllerStateFatal {
		c.state = ControllerStateIdle
	}
	c.mu.Unlock()

	var payload any
	if err != nil {
		// Surface turn failures the domain log could not represent (for example
		// a persistence error before the loop emitted any run-failed event).
		payload = runtimeevent.TurnFinishedPayload{Error: err.Error()}
	}
	c.publishDurable(sessionID, runID, turn, runtimeevent.KindTurnFinished, payload)

	// Steer relay: leftovers the loop never drained become the next turn's
	// prompt. The relay re-enters through cmdCh so submission stays
	// serialized with external input; the result channel is buffered, so
	// nobody has to read it. Relaying on every terminal outcome (completed,
	// cancelled, failed, budget) is what makes Ctrl+C flush pending steers.
	if cell := c.steerCell(); cell != nil && cell.Len() > 0 {
		prompt := strings.Join(cell.Take(), "\n\n")
		select {
		case c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, ResultCh: make(chan controllerResult, 1)}:
		default:
			// The queue is full or the controller is shutting down; the
			// messages are lost with the process, which matches the cell's
			// volatile contract (STEER_DESIGN §3.3).
			c.logger.Warn("steer relay dropped: command queue unavailable", "messages", prompt)
		}
	}
}

func (c *Controller) handleCancelTurn(cmd controllerCommand) {
	c.mu.Lock()
	state := c.state
	cancelTurn := c.cancelTurn
	c.mu.Unlock()

	if state != ControllerStateRunning && state != ControllerStateAwaitingApproval {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot cancel in state %q", state)}
		return
	}

	if cancelTurn != nil {
		cancelTurn()
	}

	c.publishEphemeral(c.sessionID, c.runID, c.turnCounter, runtimeevent.KindRunCancelRequested, nil)
	c.setState(ControllerStateCancelling)

	cmd.ResultCh <- controllerResult{}
}

func (c *Controller) handleResolveApproval(cmd controllerCommand) {
	c.mu.Lock()
	state := c.state
	c.mu.Unlock()

	if state != ControllerStateAwaitingApproval {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("not awaiting approval in state %q", state)}
		return
	}

	if !c.approver.ResolveApproval(cmd.Approval, cmd.Decision) {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("approval binding does not match a pending request")}
		return
	}

	var note string
	if cmd.Decision == domain.DecisionAllow && cmd.RuleHint != nil {
		if prefix, ok := c.rulesApprover.RememberRunCmd(cmd.RuleHint.ToolName, cmd.RuleHint.Arguments); ok {
			note = strings.Join(prefix, " ")
			// Persist the remembered prefix to the user rules layer so future
			// sessions inherit it (rules.persist_remembered=false opts out;
			// a nil bootstrap in tests keeps the default). The derivation
			// above already banned shells, eval interpreters, destructive
			// programs, heredocs, and escalated runs.
			if c.persistRememberedRules() {
				if dir, err := permission.RulesDirUser(); err == nil {
					if err := permission.AppendRememberedRule(dir, prefix); err != nil {
						c.logger.Warn("persist remembered rule failed", "prefix", note, "error", err)
					} else {
						note += " (saved to " + dir + ")"
					}
				}
			}
		}
	}
	cmd.ResultCh <- controllerResult{Value: note}
}

func (c *Controller) handleNewSession(cmd controllerCommand) {
	c.mu.Lock()
	if c.state != ControllerStateIdle && c.state != ControllerStateBooting {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot create new session in state %q", c.state)}
		return
	}
	c.sessionID = domain.NewSessionID()
	c.runID = domain.RunID{}
	c.turnCounter = 0
	c.messages = nil
	c.lastUsage = domain.Usage{}
	c.resumedRun = nil
	c.resumed = false
	// A compaction is requested against a specific transcript; it must not
	// leak into a different session's first turn.
	c.forceCompact = false
	sessionID := c.sessionID
	c.state = ControllerStateIdle
	c.mu.Unlock()

	if err := c.bootstrap.Store.CreateSession(c.sessionCtx, sessionID); err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("create session: %w", err)}
		return
	}
	c.publishSessionEnv(sessionID)
	c.logger.Info("new session created", "session_id", sessionID)
	cmd.ResultCh <- controllerResult{}
}

func (c *Controller) handleAnswerQuestion(cmd controllerCommand) {
	if c.questioner == nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("question answering is unavailable")}
		return
	}
	resolved := c.questioner.Resolve(cmd.QuestionID, cmd.Answer)
	if resolved {
		c.mu.Lock()
		sessionID, runID, turn := c.sessionID, c.runID, c.turnCounter
		c.mu.Unlock()
		c.publishEphemeral(sessionID, runID, turn, runtimeevent.KindQuestionAnswered, runtimeevent.QuestionAnsweredPayload{
			QuestionID: cmd.QuestionID,
			Skipped:    cmd.Answer.Skipped,
		})
	}
	cmd.ResultCh <- controllerResult{Value: AnswerQuestionResult{Resolved: resolved}}
}

func (c *Controller) handleRequestCompaction(cmd controllerCommand) {
	c.mu.Lock()
	pending := c.forceCompact
	c.forceCompact = true
	c.mu.Unlock()
	if !pending {
		c.logger.Info("compaction scheduled by user")
	}
	cmd.ResultCh <- controllerResult{Value: RequestCompactionResult{AlreadyPending: pending}}
}

func (c *Controller) handleSetReasoning(cmd controllerCommand) {
	var override *domain.ReasoningSpec
	switch strings.TrimSpace(cmd.Reasoning) {
	case "default":
		// nil: clear the override, fall back to the model's configuration.
	case "off", "low", "medium", "high":
		effort := domain.ReasoningEffort(strings.TrimSpace(cmd.Reasoning))
		override = &domain.ReasoningSpec{Effort: effort}
	default:
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("reasoning must be off, low, medium, high, or default, got %q", cmd.Reasoning)}
		return
	}
	c.mu.Lock()
	c.reasoningOverride = override
	current := c.currentLocked()
	effective, overridden := c.reasoningLocked(current)
	c.mu.Unlock()
	c.logger.Info("reasoning updated", "override", cmd.Reasoning, "effective", describeReasoning(effective))
	cmd.ResultCh <- controllerResult{Value: SetReasoningResult{Effective: effective, Overridden: overridden}}
}

func (c *Controller) handleSetModel(cmd controllerCommand) {
	if c.bootstrap == nil || c.bootstrap.Resolved == nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("model switching is unavailable: no providers configured")}
		return
	}
	ref, err := c.bootstrap.Resolved.ResolveRef(cmd.ModelName)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: err}
		return
	}
	meta, _ := c.bootstrap.Resolved.ModelMeta(ref)
	c.mu.Lock()
	prev := c.currentLocked()
	c.current = ref
	c.mu.Unlock()
	c.logger.Info("model switched", "previous", prev.String(), "current", ref.String())
	cmd.ResultCh <- controllerResult{Value: SetModelResult{Prev: prev, Cur: ref, Meta: meta}}
}

func (c *Controller) handleResumeSession(cmd controllerCommand) {
	c.mu.Lock()
	if c.state != ControllerStateIdle && c.state != ControllerStateBooting {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot resume session in state %q", c.state)}
		return
	}
	c.mu.Unlock()

	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("session recovery is unavailable for this store")}
		return
	}
	inspection, err := store.InspectSession(c.sessionCtx, cmd.SessionID)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("inspect session: %w", err)}
		return
	}
	run, err := agent.RecoverRun(inspection.Session.ID, inspection.Checkpoint,
		inspection.Transcript.Messages, inspection.Events, inspection.Session.Version,
		c.bootstrap.Resolved.Limits, c.clock, c.bootstrap.Validator)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("recover session: %w", err)}
		return
	}

	c.mu.Lock()
	c.sessionID = cmd.SessionID
	c.runID = domain.RunID{}
	c.turnCounter = 0
	c.messages = append([]domain.Message(nil), inspection.Transcript.Messages...)
	c.lastUsage = run.Usage
	c.resumedRun = run
	c.resumed = true
	// Same as handleNewSession: a pending compaction belongs to the
	// transcript it was requested against.
	c.forceCompact = false
	c.state = ControllerStateIdle
	c.mu.Unlock()

	c.publishSessionEnv(c.sessionID)
	c.logger.Info("session resumed", "session_id", c.sessionID)
	cmd.ResultCh <- controllerResult{}
}

// publishSessionEnv points the runner's attribution environment at the given
// session, so commands spawned from here on carry its identity. The holder
// may be nil in tests that assemble a Bootstrap by hand.
func (c *Controller) publishSessionEnv(sessionID domain.SessionID) {
	if c.bootstrap == nil || c.bootstrap.SessionEnv == nil {
		return
	}
	c.bootstrap.SessionEnv.Store(process.LoomSessionEnv(c.bootstrap.Version, sessionID.String()))
}

func (c *Controller) handleRequestSnapshot(cmd controllerCommand) {
	c.mu.Lock()
	current := c.currentLocked()
	var contextWindow int64
	if c.bootstrap != nil && c.bootstrap.Resolved != nil {
		if meta, ok := c.bootstrap.Resolved.ModelMeta(current); ok {
			contextWindow = meta.ContextWindow
		}
	}
	reasoning, overridden := c.reasoningLocked(current)
	snap := Snapshot{
		State:               c.state,
		SessionID:           c.sessionID,
		RunID:               c.runID,
		ModelName:           current.Model,
		ProviderName:        current.Provider,
		ContextWindow:       contextWindow,
		ReasoningEffort:     describeReasoning(reasoning),
		ReasoningOverridden: overridden,
		WorkspaceRoot:       c.workspaceLocked(),
		TurnCount:           c.turnCounter,
		Usage:               c.lastUsage,
		Messages:            append([]domain.Message(nil), c.messages...),
		PendingApprovals:    c.approver.PendingApprovals(),
		PendingSteers:       c.steerCellPeek(),
		Timestamp:           c.clock.Now(),
	}
	c.mu.Unlock()
	cmd.ResultCh <- controllerResult{Value: snap}
}

func (c *Controller) handleShutdown() {
	c.mu.Lock()
	if c.state == ControllerStateClosed {
		c.mu.Unlock()
		return
	}
	c.state = ControllerStateClosed
	cancelSession := c.cancelSession
	cancelTurn := c.cancelTurn
	c.mu.Unlock()

	// Cancel current turn
	if cancelTurn != nil {
		cancelTurn()
	}

	// Deny all pending approvals and skip all pending questions.
	c.approver.DenyAll()
	if c.questioner != nil {
		c.questioner.SkipAll()
	}

	// Cancel session context
	if cancelSession != nil {
		cancelSession()
	}

	c.logger.Info("controller shutting down")
}

func (c *Controller) publishDurable(sessionID domain.SessionID, runID domain.RunID, turn int, kind runtimeevent.RuntimeEventKind, payload any) {
	if c.broker == nil {
		return
	}
	if err := c.broker.PublishDurable(sessionID, runID, turn, kind, payload); err != nil {
		c.logger.Error("publish durable event", "error", err, "kind", kind)
	}
}

func (c *Controller) publishEphemeral(sessionID domain.SessionID, runID domain.RunID, turn int, kind runtimeevent.RuntimeEventKind, payload any) {
	if c.broker == nil {
		return
	}
	if err := c.broker.PublishEphemeral(sessionID, runID, turn, kind, payload); err != nil {
		c.logger.Error("publish ephemeral event", "error", err, "kind", kind)
	}
}

// SetAwaitingApproval transitions the controller to awaiting_approval state.
// Called by the publishing store when a permission.requested event is persisted.
func (c *Controller) SetAwaitingApproval() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.state == ControllerStateRunning {
		c.state = ControllerStateAwaitingApproval
	}
}

// SetRunning transitions the controller back to running state.
// Called by the publishing store when a permission.resolved event is persisted.
func (c *Controller) SetRunning() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.state == ControllerStateAwaitingApproval {
		c.state = ControllerStateRunning
	}
}

// RecordUsage updates the last known usage.
func (c *Controller) RecordUsage(usage domain.Usage) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.lastUsage = usage
}

// --- command types ---

const (
	cmdSubmitPrompt      = "submit_prompt"
	cmdCancelTurn        = "cancel_turn"
	cmdResolveApproval   = "resolve_approval"
	cmdNewSession        = "new_session"
	cmdResumeSession     = "resume_session"
	cmdSetModel          = "set_model"
	cmdSetReasoning      = "set_reasoning"
	cmdRequestCompaction = "request_compaction"
	cmdAnswerQuestion    = "answer_question"
	cmdRequestSnapshot   = "request_snapshot"
	cmdShutdown          = "shutdown"
)

type controllerCommand struct {
	Kind       string
	Prompt     string
	SessionID  domain.SessionID
	ModelName  string
	Reasoning  string
	Approval   ApprovalBinding
	Decision   domain.Decision
	RuleHint   *ApprovalRuleHint
	QuestionID domain.EventID
	Answer     domain.QuestionAnswer
	ResultCh   chan<- controllerResult
}

type controllerResult struct {
	Value any
	Err   error
}

// --- publishing store ---

// publishingStore wraps a domain.SessionStore and publishes runtime events
// after domain events are successfully persisted.
type publishingStore struct {
	inner      domain.SessionStore
	broker     *runtimeevent.Broker
	sessionID  domain.SessionID
	runID      domain.RunID
	clock      domain.Clock
	controller *Controller

	// previews stashes bounded tool-result excerpts keyed by call ID between
	// EventToolResultAdded and EventToolExecutionCompleted, so the runtime
	// ToolCompleted event can carry a displayable preview.
	previews map[domain.ToolCallID]string

	// pendingArgs stashes raw tool-call arguments keyed by call ID between
	// EventModelResponseCompleted (which carries them in the assistant
	// message) and tool preparation/approval, so edit calls can render a
	// diff for display. Entries are dropped once execution starts.
	pendingArgs map[domain.ToolCallID]json.RawMessage
}

func (s *publishingStore) CreateSession(ctx context.Context, sessionID domain.SessionID) error {
	return s.inner.CreateSession(ctx, sessionID)
}

func (s *publishingStore) AppendEvents(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event) error {
	if err := s.inner.AppendEvents(ctx, sessionID, expectedVersion, events); err != nil {
		return err
	}
	s.publishForEvents(sessionID, events)
	return nil
}

func (s *publishingStore) AppendEventsAndCheckpoint(ctx context.Context, sessionID domain.SessionID, expectedVersion int64, events []domain.Event, checkpoint domain.Checkpoint) error {
	if err := s.inner.AppendEventsAndCheckpoint(ctx, sessionID, expectedVersion, events, checkpoint); err != nil {
		return err
	}
	s.publishForEvents(sessionID, events)
	// Keep the controller snapshot projection aligned with the durable checkpoint.
	s.controller.mu.Lock()
	s.controller.messages = append([]domain.Message(nil), checkpoint.Messages...)
	s.controller.lastUsage = checkpoint.Usage
	s.controller.mu.Unlock()
	return nil
}

func (s *publishingStore) LoadEvents(ctx context.Context, sessionID domain.SessionID, after int64) ([]domain.Event, error) {
	return s.inner.LoadEvents(ctx, sessionID, after)
}

func (s *publishingStore) SaveCheckpoint(ctx context.Context, ckpt domain.Checkpoint) error {
	return s.inner.SaveCheckpoint(ctx, ckpt)
}

func (s *publishingStore) LoadLatestCheckpoint(ctx context.Context, sessionID domain.SessionID) (domain.Checkpoint, error) {
	return s.inner.LoadLatestCheckpoint(ctx, sessionID)
}

func (s *publishingStore) publishForEvents(sessionID domain.SessionID, events []domain.Event) {
	for _, ev := range events {
		s.publishForEvent(sessionID, ev)
	}
}

func (s *publishingStore) publishForEvent(sessionID domain.SessionID, ev domain.Event) {
	switch ev.Type {
	case domain.EventRunStateChanged:
		var payload domain.RunState
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRunPhaseChanged, runtimeevent.RunPhasePayload{
				Phase: payload.Phase,
			})
		}
	case domain.EventModelRequestStarted:
		var payload modelRequestAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelRequestStarted, runtimeevent.ModelRequestStartedPayload{
				RequestID: payload.RequestID,
				ModelName: payload.ModelName,
			})
		}
	case domain.EventModelResponseCompleted:
		var payload domain.MessageEventPayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			hasToolCalls := len(payload.Message.ToolCalls()) > 0
			requestID, parseErr := domain.ParseEventID(payload.Message.Metadata["request_id"])
			if parseErr != nil {
				requestID = ev.ID
			}
			stopReason := domain.StopReason(payload.Message.Metadata["stop_reason"])
			if stopReason == "" {
				// Sessions persisted before stop reasons were recorded fall back to
				// an inference from the message shape.
				if hasToolCalls {
					stopReason = domain.StopToolUse
				} else {
					stopReason = domain.StopEndTurn
				}
			}
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelResponseCompleted, runtimeevent.ModelResponseCompletedPayload{
				RequestID:    requestID,
				StopReason:   stopReason,
				HasToolCalls: hasToolCalls,
				Text:         strings.Join(payload.Message.TextParts(), ""),
			})
			// Stash raw arguments so edit calls can render a diff when the
			// tool is prepared or escalated to approval. The map is bounded
			// against leaks from calls that never reach execution.
			for _, call := range payload.Message.ToolCalls() {
				if len(call.Arguments) > 0 && s.pendingArgs != nil {
					s.pendingArgs[call.ID] = call.Arguments
				}
			}
			if len(s.pendingArgs) > pendingArgsCap {
				s.pendingArgs = make(map[domain.ToolCallID]json.RawMessage)
			}
		}
	case domain.EventModelRequestFailed:
		var payload modelRequestFailedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelRequestFailed, runtimeevent.ModelRequestFailedPayload{
				RequestID: payload.RequestID,
				Stage:     payload.Stage,
				Code:      payload.Code,
			})
		}
	case domain.EventToolCallPrepared:
		var payload toolCallAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindToolPrepared, runtimeevent.ToolPreparedPayload{
				CallID:   payload.CallID,
				ToolName: payload.Tool,
				Risk:     payload.Risk,
				Target:   toolCallTarget(payload),
				Diff:     render.DiffForToolCall(payload.Tool, s.pendingArgs[payload.CallID], toolDiffMaxLines),
			})
		}
	case domain.EventPermissionRequested:
		var payload toolCallAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindApprovalRequested, runtimeevent.ApprovalRequestedPayload{
				ApprovalID:  ev.ID,
				CallID:      payload.CallID,
				ToolName:    payload.Tool,
				Risk:        payload.Risk,
				Description: payload.ApprovalDesc,
				ArgsHash:    payload.ArgsHash,
				ReadPaths:   payload.ReadPaths,
				WritePaths:  payload.WritePaths,
				Diff:        render.DiffForToolCall(payload.Tool, s.pendingArgs[payload.CallID], toolDiffMaxLines),
				Arguments:   s.pendingArgs[payload.CallID],
			})
			s.controller.SetAwaitingApproval()
		}
	case domain.EventPermissionResolved:
		var payload permissionResolvedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindApprovalResolved, runtimeevent.ApprovalResolvedPayload{
				ApprovalID: ev.ID,
				CallID:     payload.CallID,
				Decision:   payload.Decision,
			})
			s.controller.SetRunning()
		}
	case domain.EventUserMessageAdded:
		// Only the loop's steer drain produces this inside a turn — the
		// turn-opening prompt is persisted through the bare store (see
		// controller.runTurn), so this case uniquely identifies injections.
		var payload domain.MessageEventPayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindSteerInjected, runtimeevent.SteerInjectedPayload{
				Text: strings.Join(payload.Message.TextParts(), ""),
			})
		}
	case domain.EventToolResultAdded:
		var payload domain.MessageEventPayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			callID, preview := toolResultPreview(payload.Message)
			if !callID.IsZero() && preview != "" && s.previews != nil {
				s.previews[callID] = preview
			}
		}
	case domain.EventToolExecutionStarted:
		var payload toolCallAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			delete(s.pendingArgs, payload.CallID)
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindToolStarted, runtimeevent.ToolStartedPayload{
				CallID:   payload.CallID,
				ToolName: payload.Tool,
			})
		}
	case domain.EventToolExecutionCompleted:
		var payload toolExecutionCompletedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			durationMs := payload.FinishedAt.Sub(payload.StartedAt).Milliseconds()
			preview := s.previews[payload.CallID]
			delete(s.previews, payload.CallID)
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindToolCompleted, runtimeevent.ToolCompletedPayload{
				CallID:       payload.CallID,
				ToolName:     payload.ToolName,
				Status:       payload.Status,
				DurationMs:   durationMs,
				Error:        payload.ErrorCode,
				ErrorMessage: payload.ErrorMessage,
				FinishedAt:   payload.FinishedAt,
				Preview:      preview,
			})
		}
	case domain.EventBudgetUpdated:
		var payload domain.Usage
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindBudgetUpdated, runtimeevent.BudgetUpdatedPayload{
				Turns:        payload.Turns,
				InputTokens:  payload.InputTokens,
				OutputTokens: payload.OutputTokens,
				ToolCalls:    payload.ToolCalls,
			})
		}
	case domain.EventContextCompacted:
		var payload contextCompactedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindContextCompacted, runtimeevent.ContextCompactedPayload{
				MaskedOutputs:    payload.MaskedOutputs,
				MaskedBytes:      payload.MaskedBytes,
				ArchivedMessages: payload.ArchivedMessages,
				EstTokensBefore:  payload.EstTokensBefore,
				EstTokensAfter:   payload.EstTokensAfter,
				Summarized:       payload.Summarized,
			})
		}
	case domain.EventPlanRevised:
		var plan domain.Plan
		if err := json.Unmarshal(ev.Payload, &plan); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindPlanUpdated, plan)
		}
	case domain.EventRunCompleted:
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRunCompleted, nil)
	case domain.EventRunFailed:
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRuntimeFatal, runtimeevent.RuntimeFatalPayload{
			Message: "run failed",
		})
	case domain.EventRunCancelled:
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRunCancelled, nil)
	}
}

// --- Local DTOs for deserializing agent event payloads ---
// These mirror the unexported types in the agent package.

type modelRequestAuditDTO struct {
	RequestID    domain.EventID `json:"request_id"`
	ModelName    string         `json:"model_name"`
	ManifestID   string         `json:"manifest_id"`
	ManifestHash string         `json:"manifest_hash"`
	PromptHash   string         `json:"prompt_hash"`
}

type modelRequestFailedDTO struct {
	RequestID domain.EventID `json:"request_id"`
	Stage     string         `json:"stage"`
	Code      string         `json:"code"`
}

// contextCompactedDTO mirrors the agent's unexported compaction payload.
type contextCompactedDTO struct {
	MaskedOutputs    int  `json:"masked_outputs"`
	MaskedBytes      int  `json:"masked_bytes"`
	ArchivedMessages int  `json:"archived_messages,omitempty"`
	EstTokensBefore  int  `json:"est_tokens_before"`
	EstTokensAfter   int  `json:"est_tokens_after"`
	Summarized       bool `json:"summarized,omitempty"`
}

type toolCallAuditDTO struct {
	CallID       domain.ToolCallID `json:"call_id"`
	Tool         string            `json:"tool"`
	Risk         domain.RiskLevel  `json:"risk"`
	ArgsHash     string            `json:"args_hash,omitempty"`
	ReadPaths    []string          `json:"read_paths,omitempty"`
	WritePaths   []string          `json:"write_paths,omitempty"`
	ApprovalDesc string            `json:"approval_desc,omitempty"`
}

type permissionResolvedDTO struct {
	CallID   domain.ToolCallID `json:"call_id"`
	Decision domain.Decision   `json:"decision"`
}

type toolExecutionCompletedDTO struct {
	CallID       domain.ToolCallID `json:"call_id"`
	ToolName     string            `json:"tool_name,omitempty"`
	Status       domain.ToolStatus `json:"status"`
	ErrorCode    string            `json:"error_code,omitempty"`
	ErrorMessage string            `json:"error_message,omitempty"`
	StartedAt    time.Time         `json:"started_at"`
	FinishedAt   time.Time         `json:"finished_at"`
}

// toolCallTarget picks the most descriptive display target for a prepared
// call: the first write path, then the first read path, then the approval
// description.
func toolCallTarget(audit toolCallAuditDTO) string {
	if len(audit.WritePaths) > 0 {
		return audit.WritePaths[0]
	}
	if len(audit.ReadPaths) > 0 {
		return audit.ReadPaths[0]
	}
	return audit.ApprovalDesc
}

// Bounds for the tool result preview carried by runtime ToolCompleted events.
const (
	toolPreviewMaxLines = 12
	toolPreviewMaxBytes = 1200
)

// toolDiffMaxLines bounds the rendered argument diff for edit/write calls.
const toolDiffMaxLines = 40

// pendingArgsCap bounds the stashed-arguments map against leaks from calls
// that never reach execution (e.g. denied approvals).
const pendingArgsCap = 256

// toolResultPreview extracts a bounded text excerpt from a tool-result
// message: the joined text parts, falling back to the error message.
func toolResultPreview(msg domain.Message) (domain.ToolCallID, string) {
	for _, part := range msg.Parts {
		if part.Kind != domain.PartToolResult || part.ToolResult == nil {
			continue
		}
		result := part.ToolResult
		var b strings.Builder
		for _, cp := range result.Content {
			if cp.Kind == domain.PartText {
				b.WriteString(cp.Text)
			}
		}
		text := b.String()
		if strings.TrimSpace(text) == "" && result.Error != nil {
			text = result.Error.Message
		}
		return result.CallID, boundPreviewLines(text, toolPreviewMaxLines, toolPreviewMaxBytes)
	}
	return domain.ToolCallID{}, ""
}

// boundPreviewLines trims text to at most maxLines lines and maxBytes bytes,
// marking truncation with an ellipsis line.
func boundPreviewLines(text string, maxLines, maxBytes int) string {
	text = strings.TrimSpace(text)
	if text == "" {
		return ""
	}
	truncated := false
	lines := strings.Split(text, "\n")
	if len(lines) > maxLines {
		lines = lines[:maxLines]
		truncated = true
	}
	out := strings.Join(lines, "\n")
	if len(out) > maxBytes {
		out = out[:maxBytes]
		truncated = true
	}
	if truncated {
		out += "\n…"
	}
	return out
}

// toolCallTarget extracts the primary subject of a prepared call for one-line
// display: write paths for edits, read paths otherwise, and the approval
// description (e.g. the command line for run_cmd) when no paths exist.
