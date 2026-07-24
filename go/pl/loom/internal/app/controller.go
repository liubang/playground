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
	"github.com/liubang/playground/go/pl/loom/internal/domain"
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
	State            ControllerState  `json:"state"`
	SessionID        domain.SessionID `json:"session_id"`
	RunID            domain.RunID     `json:"run_id,omitempty"`
	ModelName        string           `json:"model_name"`
	WorkspaceRoot    string           `json:"workspace_root"`
	TurnCount        int              `json:"turn_count"`
	Usage            domain.Usage     `json:"usage"`
	Messages         []domain.Message `json:"messages,omitempty"`
	PendingApprovals []domain.EventID `json:"pending_approvals,omitempty"`
	Timestamp        time.Time        `json:"timestamp"`
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
	clock     domain.Clock
	logger    *slog.Logger

	mu          sync.Mutex
	state       ControllerState
	sessionID   domain.SessionID
	runID       domain.RunID
	turnCounter int
	lastUsage   domain.Usage
	messages    []domain.Message
	resumedRun  *agent.Run
	resumed     bool

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
	Clock     domain.Clock
	Logger    *slog.Logger
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
	return &Controller{
		bootstrap:     cfg.Bootstrap,
		broker:        cfg.Broker,
		approver:      cfg.Approver,
		clock:         clock,
		logger:        logger,
		state:         ControllerStateBooting,
		sessionCtx:    sessionCtx,
		cancelSession: cancelSession,
		cmdCh:         make(chan controllerCommand, 64),
		doneCh:        make(chan struct{}),
	}
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

// SubmitPrompt submits a user prompt and starts a new turn.
// It returns an error if the controller is not idle.
func (c *Controller) SubmitPrompt(ctx context.Context, prompt string) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, ResultCh: resultCh}:
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
// matches the canonical PreparedCall currently awaiting a decision.
func (c *Controller) ResolveApproval(ctx context.Context, binding ApprovalBinding, decision domain.Decision) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdResolveApproval, Approval: binding, Decision: decision, ResultCh: resultCh}:
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
	if c.state != ControllerStateIdle {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot submit prompt in state %q", c.state)}
		return
	}
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

	cmd.ResultCh <- controllerResult{}
}

func (c *Controller) runTurn(ctx context.Context, prompt string, turnCounter int) error {
	store := c.bootstrap.Store
	clock := c.clock

	c.mu.Lock()
	run := c.resumedRun
	c.resumedRun = nil
	resumed := c.resumed
	c.resumed = false
	c.mu.Unlock()
	if run == nil && turnCounter > 1 {
		var err error
		run, err = c.continueRun(ctx)
		if err != nil {
			return err
		}
	}
	if run == nil {
		run = agent.NewRun(c.sessionID, c.bootstrap.Config.Limits, clock)
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
			Model:        c.bootstrap.ModelName,
			Workspace:    c.bootstrap.Config.WorkspaceRoot,
			Resumed:      resumed,
			MessageCount: len(run.Messages),
		})
	}

	// Build the loop
	loop := &agent.Loop{
		Run:          run,
		Model:        c.bootstrap.Model,
		ModelName:    c.bootstrap.ModelName,
		Store:        &publishingStore{inner: store, broker: c.broker, sessionID: c.sessionID, runID: run.ID, clock: clock, controller: c, previews: make(map[domain.ToolCallID]string), pendingArgs: make(map[domain.ToolCallID]json.RawMessage)},
		Approver:     c.approver,
		Policy:       c.bootstrap.Policy,
		Registry:     c.bootstrap.Registry,
		Logger:       c.logger,
		SystemPrompt: c.bootstrap.PromptBuilder,
		StreamHooks: agent.StreamHooks{
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
		c.bootstrap.Config.Limits,
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

	cmd.ResultCh <- controllerResult{}
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
	sessionID := c.sessionID
	c.state = ControllerStateIdle
	c.mu.Unlock()

	if err := c.bootstrap.Store.CreateSession(c.sessionCtx, sessionID); err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("create session: %w", err)}
		return
	}
	c.logger.Info("new session created", "session_id", sessionID)
	cmd.ResultCh <- controllerResult{}
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
		c.bootstrap.Config.Limits, c.clock, c.bootstrap.Validator)
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
	c.state = ControllerStateIdle
	c.mu.Unlock()

	c.logger.Info("session resumed", "session_id", c.sessionID)
	cmd.ResultCh <- controllerResult{}
}

func (c *Controller) handleRequestSnapshot(cmd controllerCommand) {
	c.mu.Lock()
	snap := Snapshot{
		State:            c.state,
		SessionID:        c.sessionID,
		RunID:            c.runID,
		ModelName:        c.bootstrap.ModelName,
		WorkspaceRoot:    c.bootstrap.Config.WorkspaceRoot,
		TurnCount:        c.turnCounter,
		Usage:            c.lastUsage,
		Messages:         append([]domain.Message(nil), c.messages...),
		PendingApprovals: c.approver.PendingApprovals(),
		Timestamp:        c.clock.Now(),
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

	// Deny all pending approvals
	c.approver.DenyAll()

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
	cmdSubmitPrompt    = "submit_prompt"
	cmdCancelTurn      = "cancel_turn"
	cmdResolveApproval = "resolve_approval"
	cmdNewSession      = "new_session"
	cmdResumeSession   = "resume_session"
	cmdRequestSnapshot = "request_snapshot"
	cmdShutdown        = "shutdown"
)

type controllerCommand struct {
	Kind      string
	Prompt    string
	SessionID domain.SessionID
	Approval  ApprovalBinding
	Decision  domain.Decision
	ResultCh  chan<- controllerResult
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
				CallID:     payload.CallID,
				ToolName:   payload.ToolName,
				Status:     payload.Status,
				DurationMs: durationMs,
				Error:      payload.ErrorCode,
				FinishedAt: payload.FinishedAt,
				Preview:    preview,
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
	CallID     domain.ToolCallID `json:"call_id"`
	ToolName   string            `json:"tool_name,omitempty"`
	Status     domain.ToolStatus `json:"status"`
	ErrorCode  string            `json:"error_code,omitempty"`
	StartedAt  time.Time         `json:"started_at"`
	FinishedAt time.Time         `json:"finished_at"`
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
