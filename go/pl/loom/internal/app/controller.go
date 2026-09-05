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
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/media"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/render"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/tool/subagent"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
)

// ControllerState represents the high-level state of the SessionController.
type ControllerState string

const (
	ControllerStateBooting          ControllerState = "booting"
	ControllerStateIdle             ControllerState = "idle"
	ControllerStateRunning          ControllerState = "running"
	ControllerStateAwaitingApproval ControllerState = "awaiting_approval"
	ControllerStateCancelling       ControllerState = "cancelling"
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
	// Window projects the session's context-window thresholds
	// (docs/CONTEXT_DESIGN.md §4.1) so frontends can render occupancy
	// against the compaction trigger without re-deriving ratios; nil when
	// the model declares no usable window.
	Window *WindowInfo `json:"window,omitempty"`
	// Occupancy estimates the size of the next model request (transcript
	// plus the per-request overhead captured at turn build), on the same
	// scale as ContextUsagePayload.OccupancyTokens.
	Occupancy int64 `json:"occupancy,omitempty"`
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
	// PendingRequests projects in-flight approvals and ask_user questions
	// with their full card payloads, so a (re)connecting client can rebuild
	// its UI from the snapshot alone (docs/SERVE_DESIGN.md §4.4).
	PendingRequests []PendingRequest `json:"pending_requests,omitempty"`
	PendingSteers   []string         `json:"pending_steers,omitempty"`
	// PendingFollowups projects the cell's next-turn queue: messages held
	// until the busy turn ends, relayed one per turn boundary.
	PendingFollowups []string `json:"pending_followups,omitempty"`
	// LastError projects the most recent unrecovered turn failure (a model
	// request the loop gave up on, or a turn-level error) so a
	// (re)connecting client can render a persistent error block from the
	// snapshot alone — live-only error blocks otherwise vanish on session
	// switch/reload. Cleared when a later model call succeeds or a new
	// turn starts.
	LastError *SnapshotError `json:"last_error,omitempty"`
	// Plan is the run's latest task plan (update_plan), projected so a
	// (re)connecting client can render the plan panel from the snapshot
	// alone instead of waiting for the next plan.updated event.
	Plan *domain.Plan `json:"plan,omitempty"`
	// Delegated marks a sub-agent child session: read-only for frontends —
	// prompts are rejected; approvals/questions stay resolvable.
	Delegated bool `json:"delegated,omitempty"`
	// ParentSessionID is the delegating parent when Delegated is true.
	ParentSessionID string `json:"parent_session_id,omitempty"`
	// EventSeq is the broker-sequence watermark this projection has
	// applied: subscribe with it as cursor for a gapless snapshot+delta
	// handoff (docs/SERVE_DESIGN.md §4.4).
	EventSeq  uint64    `json:"event_seq"`
	Timestamp time.Time `json:"timestamp"`
}

// SnapshotError is the wire projection of a terminal turn failure. Stage
// and Code come from the model.request_failed audit event when one
// preceded the failure; a bare Message means the turn died from a cause
// the domain log could not represent (e.g. a persistence error).
type SnapshotError struct {
	Stage   string `json:"stage,omitempty"`
	Code    string `json:"code,omitempty"`
	Message string `json:"message"`
}

// snapshotErrorMaxRunes bounds a projected error message: provider error
// bodies can embed multi-hundred-byte JSON blobs.
const snapshotErrorMaxRunes = 300

// WindowInfo is the wire projection of agent.WindowModel: every threshold a
// frontend needs to render context occupancy against the compaction trigger.
type WindowInfo struct {
	Nominal        int64 `json:"nominal"`
	Effective      int64 `json:"effective"`
	CompactTrigger int64 `json:"compact_trigger"`
	CompactTarget  int64 `json:"compact_target"`
}

// PendingRequestKind identifies a resolvable request surfaced to frontends.
type PendingRequestKind string

const (
	// PendingRequestApproval is a tool-call approval (resolve via ResolveApproval).
	PendingRequestApproval PendingRequestKind = "approval"
	// PendingRequestQuestion is an ask_user question (resolve via AnswerQuestion).
	PendingRequestQuestion PendingRequestKind = "question"
)

// PendingRequest is a pending, one-shot-resolvable request (approval or
// ask_user question) projected for Snapshot consumers — the reconnect-safe
// companion to the ephemeral approval.requested / question.asked events.
type PendingRequest struct {
	Kind     PendingRequestKind                     `json:"kind"`
	ID       domain.EventID                         `json:"id"`
	Approval *runtimeevent.ApprovalRequestedPayload `json:"approval,omitempty"`
	Question *domain.Question                       `json:"question,omitempty"`
}

// SessionSummary is the frontend-safe metadata used by session pickers.
type SessionSummary struct {
	ID        domain.SessionID `json:"id"`
	Version   int64            `json:"version"`
	CreatedAt time.Time        `json:"created_at"`
	UpdatedAt time.Time        `json:"updated_at"`
	// WorkspaceID is the owning workspace (docs/WORKSPACE_DESIGN.md W1),
	// empty for pre-v5 sessions not yet backfilled.
	WorkspaceID string `json:"workspace_id,omitempty"`
	// State is the live controller state, or "closed" for sessions without
	// a live runtime in this process (docs/WEB_DESIGN.md §7.7).
	State ControllerState `json:"state"`
	// ModelName/TurnCount are populated for live sessions only (they come
	// from the controller projection, not the store).
	ModelName string `json:"model_name,omitempty"`
	TurnCount int    `json:"turn_count,omitempty"`
	// Title is the session's first user prompt: whitespace-collapsed and
	// rune-truncated; empty when the session never received a prompt.
	Title string `json:"title,omitempty"`
	// ParentSessionID is set for delegated sub-agent sessions (the child's
	// run.created delegation edge), for hierarchical pickers.
	ParentSessionID string `json:"parent_session_id,omitempty"`
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
	// packages is the shared capability set (process-level via the
	// embedded runtime); ForgetPackage must evict the session-scope twin
	// too, or a forgotten package would keep auto-approving until
	// process restart.
	packages *permission.PackageSet
	clock    domain.Clock
	logger   *slog.Logger

	mu          sync.Mutex
	state       ControllerState
	sessionID   domain.SessionID
	runID       domain.RunID
	turnCounter int
	questioner  *ChannelQuestioner
	// runtime holds the per-session mutable state (cells, questioner,
	// registry overlay). It is the controller's ONLY source of session
	// state (docs/SERVE_DESIGN.md §4.2, single-construction-path rule).
	runtime   *SessionRuntime
	lastUsage domain.Usage
	messages  []domain.Message
	// lastError projects the latest unrecovered turn failure for
	// Snapshot.LastError: set by EventModelRequestFailed (and, as a
	// fallback, by turn-finish errors the event log could not represent),
	// cleared by EventModelResponseCompleted / EventRunCreated. The pointed
	// value is never mutated in place, so snapshots may share it.
	lastError *SnapshotError
	// plan/hasPlan project the run's latest task plan (domain.EventPlanRevised)
	// for Snapshot consumers; seeded from the checkpoint on session resume.
	plan    domain.Plan
	hasPlan bool
	// delegated/parentSessionID mark a resumed sub-agent child session (the
	// run.created delegation edge). Delegated sessions are read-only:
	// prompts are rejected; approvals and questions stay resolvable.
	delegated       bool
	parentSessionID domain.SessionID
	// appliedSeq is the broker-sequence watermark sampled inside the
	// projection-update critical section (docs/SERVE_DESIGN.md §4.4).
	appliedSeq uint64
	// pendingCards/pendingQuestions project in-flight approval requests
	// and ask_user questions with full payloads for Snapshot.PendingRequests.
	pendingCards     map[domain.EventID]runtimeevent.ApprovalRequestedPayload
	pendingQuestions map[domain.EventID]domain.Question
	// approvalActors remembers who resolved each approval until the
	// resolution event is published.
	approvalActors map[domain.EventID]string
	resumedRun     *agent.Run
	resumed        bool
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
	// lastWindowNominal is the previous turn's model window; a shrink marks
	// the next compaction as a ModelDownshift in the audit event.
	lastWindowNominal int64
	// requestOverheadTokens is the per-request overhead estimate (system
	// prompt parts, plan note, tool schemas) captured at turn build, so
	// the snapshot's occupancy projects the same next-request scale the
	// loop reports via context.usage events.
	requestOverheadTokens int64

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

	// steerHook, when non-nil, runs inside handleSteer after the busy-state
	// routing but before the message lands in the steer cell. Test-only seam
	// for the M21 TOCTOU regression: it lets a test force the turn to finish
	// exactly inside the window between the state read and the cell.Put.
	steerHook func()
	nextTurn  uint64
	running   bool

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
	// questioner, e.g. the autonomous one when headless). Ignored when
	// Runtime is provided — the runtime's questioner wins.
	Questioner *ChannelQuestioner
	// Runtime carries per-session state (docs/SERVE_DESIGN.md §4.2). When
	// nil (legacy TUI/tests assembly), one is derived from the bootstrap,
	// reusing its cells; serve's SessionService always passes an explicit
	// per-session Runtime.
	Runtime *SessionRuntime
	Clock   domain.Clock
	Logger  *slog.Logger
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
	// Bootstrap is nil in some UI tests; the capability set then
	// degrades to a process-local one so "allow always" still works.
	packages := permission.NewPackageSet()
	policyFn := func() permission.Policy {
		return permission.Policy{Packages: packages, Mode: permission.ModeOnRequest}
	}
	var rememberedStore *permission.RememberedStore
	if cfg.Bootstrap != nil && cfg.Bootstrap.Packages != nil {
		packages = cfg.Bootstrap.Packages
		policyFn = cfg.Bootstrap.CurrentPermissionPolicy
		rememberedStore = cfg.Bootstrap.RememberedStore
	}
	runtime := cfg.Runtime
	if runtime == nil {
		var err error
		runtime, err = NewSessionRuntime(cfg.Bootstrap, cfg.Questioner)
		if err != nil {
			// NewSessionRuntime only errors on nil cells/questioner, which it
			// never passes by construction; reaching this is a programming error.
			panic(fmt.Sprintf("derive session runtime: %v", err))
		}
	}
	c := &Controller{
		bootstrap:        cfg.Bootstrap,
		broker:           cfg.Broker,
		approver:         cfg.Approver,
		rulesApprover:    NewRuleApprover(cfg.Approver, policyFn, rememberedStore),
		packages:         packages,
		questioner:       runtime.Questioner,
		clock:            clock,
		logger:           logger,
		state:            ControllerStateBooting,
		sessionCtx:       sessionCtx,
		cancelSession:    cancelSession,
		cmdCh:            make(chan controllerCommand, 64),
		doneCh:           make(chan struct{}),
		runtime:          runtime,
		pendingCards:     make(map[domain.EventID]runtimeevent.ApprovalRequestedPayload),
		pendingQuestions: make(map[domain.EventID]domain.Question),
		approvalActors:   make(map[domain.EventID]string),
	}
	// Seed the process-level model/reasoning preference as this session's
	// initial selection: a manual switch persists globally and every new
	// session starts from it (the session can still diverge afterwards via
	// /model and /reasoning, which re-persist the global preference).
	c.seedPrefsLocked()
	// Bridge model questions onto the runtime event stream: the agent loop
	// blocks in ask_user until a frontend answers via AnswerQuestion.
	if runtime.Questioner != nil {
		runtime.Questioner.BindPublish(func(q domain.Question) {
			c.mu.Lock()
			sessionID, runID, turn := c.sessionID, c.runID, c.turnCounter
			c.pendingQuestions[q.ID] = q
			c.noteProjectionLocked()
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

// seedPrefsLocked applies the process-level model/reasoning preference as
// the session's selection. Called at construction and on every new session
// (a controller is reused across /new), so a switch made in another session
// is picked up; a resumed session keeps whatever it already had.
// Callers must hold c.mu (construction is single-threaded).
func (c *Controller) seedPrefsLocked() {
	if c.bootstrap == nil {
		return
	}
	if ref := c.bootstrap.CurrentModel(); ref != (config.ProviderModelRef{}) {
		c.current = ref
	}
	switch effort := c.bootstrap.ReasoningPreference(); effort {
	case "", "default":
		// no override: the selected model's configured reasoning applies
		c.reasoningOverride = nil
	default:
		c.reasoningOverride = &domain.ReasoningSpec{Effort: domain.ReasoningEffort(effort)}
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

// SubmitResult reports how a submitted prompt was accepted.
type SubmitResult struct {
	// Steered is true when the prompt was queued into the active turn's
	// SteerCell instead of starting a new turn immediately.
	Steered bool
	// Followup is true when the prompt was queued for AFTER the busy turn
	// (next-turn delivery) rather than steering into it.
	Followup bool
	// QueueLen is the resulting pending-queue count (0 when started).
	QueueLen int
	// Turn is the session's turn counter after the submission (the new
	// turn when started, the busy turn when queued).
	Turn int
}

// SubmitPrompt submits a user prompt. While the controller is idle it
// starts a new turn; while a turn is busy (running, awaiting approval, or
// cancelling) the prompt is queued for steering — the agent loop injects
// it before its next model call, and leftovers become the next turn's
// prompt automatically (docs/STEER_DESIGN.md §3.1).
func (c *Controller) SubmitPrompt(ctx context.Context, prompt string) (SubmitResult, error) {
	return c.SubmitPromptWithImages(ctx, prompt, nil)
}

// SubmitPromptWithImages submits a user prompt with optional image attachments.
// Images are encoded as ContentPart items alongside the text prompt in the
// user message sent to the model. While the controller is busy, images are
// dropped and only the text prompt is queued for steering (steer messages
// are text-only).
func (c *Controller) SubmitPromptWithImages(ctx context.Context, prompt string, images []domain.ImageContent) (SubmitResult, error) {
	return c.submit(ctx, prompt, images, false)
}

// SubmitFollowup queues a text-only prompt for AFTER the busy turn: it is
// held in the cell's followup (next-turn) queue and relayed as the next
// turn's prompt, one per turn boundary — versus SubmitPrompt's steer
// (next-step) delivery into the running turn. On an idle session a
// followup simply starts the turn.
func (c *Controller) SubmitFollowup(ctx context.Context, prompt string) (SubmitResult, error) {
	return c.submit(ctx, prompt, nil, true)
}

func (c *Controller) submit(ctx context.Context, prompt string, images []domain.ImageContent, followup bool) (SubmitResult, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, Images: images, Followup: followup, ResultCh: resultCh}:
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
	// Images are not carried on cancellation.
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
	return c.ResolveApprovalWithActor(ctx, binding, decision, ruleHint, "")
}

// ResolveApprovalWithActor is ResolveApproval plus the identity of who
// resolved (docs/SERVE_DESIGN.md §4.6): the actor lands in the audit log
// and the approval.resolved event payload. An empty actor means the local
// interactive frontend (TUI).
func (c *Controller) ResolveApprovalWithActor(ctx context.Context, binding ApprovalBinding, decision domain.Decision, ruleHint *ApprovalRuleHint, actor string) (string, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdResolveApproval, Approval: binding, Decision: decision, RuleHint: ruleHint, Actor: actor, ResultCh: resultCh}:
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
	// Window projects the new model's context thresholds so frontends
	// re-base occupancy rendering on the same derivation the turn runner
	// will use; nil when the model declares no usable window.
	Window *WindowInfo
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

// FeedbackScoreName is the Langfuse score name user votes are recorded
// under (BOOLEAN: 1 = thumbs up, 0 = thumbs down).
const FeedbackScoreName = "user_feedback"

// SubmitFeedback records a user vote for one run's trace. value is 1 (up)
// or 0 (down); comment is optional free text. The run → trace binding
// travels in assistant-message metadata, so the lookup scans the live
// transcript projection — the same messages the client renders, hence
// exactly the set a user can vote on.
func (c *Controller) SubmitFeedback(ctx context.Context, runID string, value float64, comment string) error {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{
		Kind:            cmdSubmitFeedback,
		FeedbackRunID:   runID,
		FeedbackValue:   value,
		FeedbackComment: comment,
		ResultCh:        resultCh,
	}:
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
		// Already stopped — e.g. the Run context was cancelled while the
		// shutdown command was in flight (SessionService.Shutdown cancels
		// the shared service context before shutting controllers down, and
		// Run's select may take the ctx.Done branch first). Shutdown's
		// contract is "ensure the controller is stopped"; that end state
		// already holds, so this is success, not an error.
		return nil
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

// ListSessions returns recent persisted sessions for a frontend picker.
func (c *Controller) ListSessions(ctx context.Context, limit int) ([]SessionSummary, error) {
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("session listing is unavailable for this store")
	}
	summaries, _, err := store.ListSessions(ctx, "", limit, false, c.bootstrap.WorkspaceID)
	if err != nil {
		return nil, err
	}
	result := make([]SessionSummary, len(summaries))
	for i, summary := range summaries {
		result[i] = SessionSummary{ID: summary.ID, Version: summary.Version, CreatedAt: summary.CreatedAt, UpdatedAt: summary.UpdatedAt}
		if !summary.ParentSessionID.IsZero() {
			result[i].ParentSessionID = summary.ParentSessionID.String()
		}
	}
	return result, nil
}

// SkillInfo is the frontend-safe projection of one discovered skill,
// backing the /skill listing and the aggregated /v1/skills endpoint.
type SkillInfo struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Scope       string `json:"scope"`
	Path        string `json:"path"`
	// Disabled marks skills suppressed via config skills.disabled. Such
	// skills only appear in the management listing (GET /v1/skills), never
	// in the catalog the model sees.
	Disabled bool `json:"disabled,omitempty"`
}

// SkillsListing is the result of a ListSkills call: the discovered skills
// plus the load issues collected during discovery (a skill that fails to
// parse never appears in Skills, so the issues are the only way to see
// why it is missing).
type SkillsListing struct {
	Skills []SkillInfo
	Issues []string
}

// ListSkills reloads the skill catalog from disk — so the listing reflects
// files added or fixed since the last prompt build — stores the fresh
// snapshot into the shared catalog (keeping read_skill consistent with
// what the user just saw), and returns the projection. It reads the
// bootstrap directly (like ListSessions): discovery is cheap and
// read-only, so it needs no command-queue serialization.
func (c *Controller) ListSkills(ctx context.Context) (SkillsListing, error) {
	if c.bootstrap == nil || c.bootstrap.Skills == nil {
		return SkillsListing{}, fmt.Errorf("skills are disabled (skills.enabled=false or the built-in system prompt is off)")
	}
	catalog := c.bootstrap.Skills.Loader.Load(ctx)
	c.bootstrap.Skills.Catalog.Store(catalog)
	// The TUI listing mirrors the catalog the model sees, so disabled
	// skills are absent here (they only surface in the management
	// listing, which marks them).
	return SkillsListing{Skills: skillInfos(catalog.Skills(), nil), Issues: issueStrings(catalog.Issues())}, nil
}

// MCPServerInfo is the frontend-safe projection of one configured MCP
// server, backing the /mcp listing.
type MCPServerInfo struct {
	Name      string
	Connected bool
	Error     string
	Tools     []string
}

// ToolchainEnvironment returns the cached PATH-augmentation report for the
// TUI /doctor listing (the same snapshot the settings environment card
// serves). Read-only and cheap, so like ListSkills it needs no
// command-queue serialization.
func (c *Controller) ToolchainEnvironment(ctx context.Context) (*ToolchainReport, error) {
	report := process.CurrentToolchainReport()
	if report == nil {
		return nil, fmt.Errorf("toolchain report is not available")
	}
	return report, nil
}

// ListMCPServers returns the status of every configured MCP server.
// Like ListSessions it reads runtime-owned state directly; the manager's
// projection reflects live reconnects (config hot-reload, /v1/mcp
// reconnect endpoint).
func (c *Controller) ListMCPServers(ctx context.Context) ([]MCPServerInfo, error) {
	if c.bootstrap == nil || c.bootstrap.MCP() == nil {
		return nil, fmt.Errorf("no mcp servers configured")
	}
	servers := c.bootstrap.MCP().Servers()
	out := make([]MCPServerInfo, 0, len(servers))
	for _, srv := range servers {
		// MCPServerInfo.Tools 是 []string（TUI 只需要名字）；manager 层
		// 的 ToolInfo 还带简介，此处投影为纯名称列表。
		tools := make([]string, 0, len(srv.Tools))
		for _, t := range srv.Tools {
			tools = append(tools, t.Name)
		}
		out = append(out, MCPServerInfo{
			Name:      srv.Name,
			Connected: srv.Connected,
			Error:     srv.Error,
			Tools:     tools,
		})
	}
	return out, nil
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
		return c.bootstrap.CurrentDefault()
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
	if c.bootstrap != nil && c.bootstrap.Resolved() != nil {
		if meta, ok := c.bootstrap.Resolved().ModelMeta(current); ok {
			return meta.Reasoning.DomainSpec(), false
		}
	}
	return domain.ReasoningSpec{}, false
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

// followupCellPeek returns the pending followup queue without draining it.
func (c *Controller) followupCellPeek() []string {
	if cell := c.steerCell(); cell != nil {
		return cell.PeekFollowups()
	}
	return nil
}

// recordPlan stores the latest task plan for Snapshot projection.
func (c *Controller) recordPlan(p domain.Plan) {
	c.mu.Lock()
	c.plan, c.hasPlan = p, true
	c.mu.Unlock()
}

// rememberedStore returns the persistent "allow always" store, or nil when
// persistence is disabled or unavailable.
func (c *Controller) rememberedStore() *permission.RememberedStore {
	if c.bootstrap == nil {
		return nil
	}
	return c.bootstrap.RememberedStore
}

// deriveEnv returns the derivation environment (sandbox roots) of the
// workspace's active policy.
func (c *Controller) deriveEnv() permission.DeriveEnv {
	if c.bootstrap == nil {
		return permission.DeriveEnv{}
	}
	return c.bootstrap.CurrentPermissionPolicy().Env
}

// ListPackages returns the combined capability set (builtin + user +
// project + remembered + session) for the /rules picker.
func (c *Controller) ListPackages(ctx context.Context) ([]permission.Package, error) {
	if c.bootstrap == nil {
		return nil, fmt.Errorf("policy not available")
	}
	return c.bootstrap.CurrentPackages(), nil
}

// ForgetPackage removes a remembered package from the persistent store,
// evicts its session-scope twin, and reloads the declarative layers so
// the change takes effect immediately.
func (c *Controller) ForgetPackage(ctx context.Context, bind permission.Binding) error {
	store := c.rememberedStore()
	if store == nil {
		return fmt.Errorf("remembered store not available")
	}
	ok, err := store.Forget(ctx, bind)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("package %v not found in remembered store", bind)
	}
	if c.packages != nil {
		c.packages.ForgetSession(bind, c.bootstrap.CurrentPermissionPolicy().Workspace)
	}
	if err := c.bootstrap.ReloadPolicy(ctx); err != nil {
		return fmt.Errorf("reload policy: %w", err)
	}
	return nil
}

func (c *Controller) setState(s ControllerState) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.state = s
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
	case cmdSubmitFeedback:
		c.handleSubmitFeedback(cmd)
	case cmdListCheckpoints:
		c.handleListCheckpoints(cmd)
	case cmdRewind:
		c.handleRewind(cmd)
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
	delegated := c.delegated
	c.mu.Unlock()
	if delegated {
		cmd.ResultCh <- controllerResult{Err: domain.NewError(domain.ErrInvalidInput, "session is a delegated sub-agent session (read-only): prompts are not accepted")}
		return
	}
	switch state {
	case ControllerStateRunning, ControllerStateAwaitingApproval, ControllerStateCancelling:
		c.handleSteer(cmd)
		return
	case ControllerStateIdle:
	default:
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot submit prompt in state %q", state)}
		return
	}
	// Image attachments: gate on the active model's declared modalities and
	// persist the raw bytes as artifacts BEFORE the turn starts — a bad
	// image must fail the submission, not the turn. The user message then
	// carries references (media.Materialize derives the wire image at every
	// model call), so no base64 ever lands in the transcript. Note the
	// steer path above keeps its text-only semantics: images submitted
	// while busy are dropped before this point.
	if len(cmd.Images) > 0 {
		c.mu.Lock()
		current := c.currentLocked()
		c.mu.Unlock()
		if meta, _ := c.bootstrap.Resolved().ModelMeta(current); !meta.SupportsImages() {
			cmd.ResultCh <- controllerResult{Err: domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("model %q does not support image input (config modalities); switch models or remove the attachments", current.String()))}
			return
		}
		refs, err := c.storeImageAttachments(cmd.Images)
		if err != nil {
			cmd.ResultCh <- controllerResult{Err: err}
			return
		}
		cmd.ImageRefs = refs
	}
	c.mu.Lock()
	if c.sessionID.IsZero() {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("no active session; call NewSession or ResumeSession first")}
		return
	}
	c.state = ControllerStateRunning
	// A new turn supersedes the previous turn's failure projection. This
	// is the authoritative turn-start point: run.created events are
	// persisted by flushRunEvents directly against the store, so they
	// never flow through publishingStore's projection updates.
	c.lastError = nil
	turnCounter := c.turnCounter + 1
	c.turnCounter = turnCounter
	c.nextTurn++
	turnID := c.nextTurn
	c.activeTurn = turnID
	sessionID, runID := c.sessionID, c.runID
	// A completed plan is archived at the turn boundary: the runtime
	// already treats it as inert (complete plans are not re-injected into
	// model context), so drop the projection to keep snapshots clean; the
	// empty plan.updated below lets live clients hide the panel. An
	// unfinished plan survives — the next update_plan revision refreshes
	// the panel.
	planArchived := c.hasPlan && c.plan.IsComplete()
	if planArchived {
		c.plan = domain.Plan{}
		c.hasPlan = false
	}
	c.mu.Unlock()

	if planArchived {
		c.publishDurable(sessionID, runID, turnCounter, runtimeevent.KindPlanUpdated, domain.Plan{Items: []domain.PlanItem{}})
	}

	// Resolve @file references and the leading /skill trigger: content is
	// injected into the model context here rather than discovered by the model.
	// The enriched prompt persists with the user message, so the transcript
	// matches what the model saw.
	cmd.Prompt = enrichPromptWithReferences(c.bootstrap, cmd.Prompt, c.logger)

	// Publish turn started event
	// Note: the envelope runID here is the PREVIOUS turn's (zero on the
	// first turn) — the new run is only created later in executeTurn.
	// Consumers needing the current run id must follow later events
	// (loop-emitted events via publishingStore carry the real one).
	c.publishDurable(sessionID, runID, turnCounter, runtimeevent.KindTurnStarted, runtimeevent.TurnStartedPayload{
		TurnIndex: turnCounter,
		Prompt:    cmd.Prompt,
		Images:    cmd.ImageRefs,
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
		err := c.runTurn(turnCtx, cmd.Prompt, cmd.ImageRefs, turnCounter)
		c.onTurnFinished(turnID, turnCounter, err)
	}()

	cmd.ResultCh <- controllerResult{Value: SubmitResult{Turn: turnCounter}}
}

// storeImageAttachments decodes client-supplied base64 images and persists
// each as an artifact (media.StoreImage sniffs the real media type from the
// bytes; the declared type is never trusted). The submission fails as a
// whole when any image is invalid — partial attachment sets would silently
// drop user intent.
func (c *Controller) storeImageAttachments(images []domain.ImageContent) ([]domain.ArtifactRef, error) {
	refs := make([]domain.ArtifactRef, 0, len(images))
	for i, img := range images {
		raw, err := base64.StdEncoding.DecodeString(img.Data)
		if err != nil {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("image %d: invalid base64 payload", i+1), domain.WithCause(err))
		}
		if len(raw) > media.MaxImageBytes {
			return nil, domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("image %d is %d bytes, exceeding the %d byte limit", i+1, len(raw), media.MaxImageBytes))
		}
		ref, err := media.StoreImage(c.sessionCtx, c.bootstrap.Artifact, raw)
		if err != nil {
			return nil, fmt.Errorf("image %d: %w", i+1, err)
		}
		refs = append(refs, ref)
	}
	return refs, nil
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
	if c.steerHook != nil {
		c.steerHook()
	}
	var err error
	var n int
	if cmd.Followup {
		err = cell.PutFollowup(cmd.Prompt)
		n = cell.FollowupLen()
	} else {
		err = cell.Put(cmd.Prompt)
		n = cell.Len()
	}
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: err}
		return
	}
	// runID is written by the turn goroutine (runTurn) under c.mu; snapshot
	// the publish identity under the same lock to avoid a data race.
	c.mu.Lock()
	sessionID, runID, turnCounter := c.sessionID, c.runID, c.turnCounter
	turnOver := c.state == ControllerStateIdle
	c.mu.Unlock()
	queue := ""
	if cmd.Followup {
		queue = "followup"
	}
	c.publishEphemeral(sessionID, runID, turnCounter, runtimeevent.KindSteerQueued, runtimeevent.SteerQueuedPayload{
		Text:     cmd.Prompt,
		QueueLen: n,
		Queue:    queue,
	})
	cmd.ResultCh <- controllerResult{Value: SubmitResult{Steered: !cmd.Followup, Followup: cmd.Followup, QueueLen: n, Turn: turnCounter}}
	// Review M21: dispatch routed this submission here from a state read
	// taken BEFORE the cell.Put; the turn may have finished in between,
	// with onTurnFinished's relay already past an empty cell — the message
	// would then sit in the cell until the next external submission even
	// though the caller was told it queued. The re-check under c.mu closes
	// the window: if the turn is over, relay now. Ordering is safe both
	// ways — whoever drains the cell first wins, the other relay finds it
	// empty; if the state is still busy (or cancelling), the running turn
	// or onTurnFinished's relay observes the Put because it happened-before
	// this mu critical section.
	if turnOver {
		c.relayPendingSteers()
	}
}

// steerCell returns the session's steer mailbox from its runtime (never
// nil after NewController; the nil check is defensive for hand-assembled
// zero-value controllers).
func (c *Controller) steerCell() *agent.SteerCell {
	if c.runtime == nil {
		return nil
	}
	return c.runtime.SteerCell
}

// subagentModelSource returns the delegate_task model mailbox; nil when
// the sub-agent is disabled or the bootstrap is hand-assembled in tests.
func (c *Controller) subagentModelSource() *subagent.ModelSource {
	if c.bootstrap == nil {
		return nil
	}
	return c.bootstrap.SubagentModels
}

// SubagentView is a read-only projection of a delegated sub-agent
// session, backing the TUI's drill-in overlay
// (docs/SUBAGENT_DESIGN.md §10).
type SubagentView struct {
	SessionID domain.SessionID
	// Outcome is empty while the child is still running.
	Outcome  domain.Outcome
	Messages []domain.Message
	Usage    domain.Usage
	Active   bool
}

// SubagentView loads the latest checkpoint of a delegated child session.
// It reads the store directly (like ListSessions): the child loop
// flushes a checkpoint after every tool batch, so the projection is
// near-real-time without any event-stream coupling.
func (c *Controller) SubagentView(ctx context.Context, sessionID domain.SessionID) (SubagentView, error) {
	if sessionID.IsZero() {
		return SubagentView{}, fmt.Errorf("sub-agent session ID is required")
	}
	if c.bootstrap == nil || c.bootstrap.Store == nil {
		return SubagentView{}, fmt.Errorf("sub-agent view is unavailable for this runtime")
	}
	checkpoint, err := c.bootstrap.Store.LoadLatestCheckpoint(ctx, sessionID)
	if err != nil {
		return SubagentView{}, fmt.Errorf("load sub-agent checkpoint: %w", err)
	}
	return SubagentView{
		SessionID: sessionID,
		Outcome:   checkpoint.State.Outcome,
		Messages:  append([]domain.Message(nil), checkpoint.Messages...),
		Usage:     checkpoint.Usage,
		Active:    checkpoint.State.Lifecycle != domain.LifecycleTerminal,
	}, nil
}

func (c *Controller) runTurn(ctx context.Context, prompt string, imageRefs []domain.ArtifactRef, turnCounter int) error {
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
	sessionID := c.sessionID
	c.mu.Unlock()
	// Attribute every command this turn spawns to this session via ctx
	// (docs/SERVE_DESIGN.md §4.3): concurrent sessions never share the
	// process-level AtomicSessionEnv.
	ctx = process.ContextWithSessionEnv(ctx, process.LoomSessionEnv(c.bootstrap.Version, sessionID.String()))
	provider := c.bootstrap.Resolved().ProviderByName(current.Provider)
	if provider == nil {
		// Cannot happen through Load/SetModel (both validate the ref), but a
		// hand-assembled bootstrap in tests can mismatch — fail the turn
		// loudly instead of panicking on provider.Model.
		return fmt.Errorf("provider %q is not configured", current.Provider)
	}
	modelMeta, _ := c.bootstrap.Resolved().ModelMeta(current)
	if run == nil && turnCounter > 1 {
		var err error
		run, err = c.continueRun(ctx)
		if err != nil {
			return err
		}
	}
	if run == nil {
		run = agent.NewRun(c.sessionID, c.bootstrap.Resolved().Limits, clock)
		// A fresh session may not exist until its first prompt; bind it to
		// this controller's workspace (docs/WORKSPACE_DESIGN.md W1).
		if err := store.CreateSession(ctx, c.sessionID, c.bootstrap.WorkspaceID); err != nil {
			c.logger.Debug("create session", "error", err)
		}
	}

	// Persist the initial or recovery continuation event before adding the prompt.
	if err := c.flushRunEvents(ctx, run); err != nil {
		return fmt.Errorf("persist run initialization: %w", err)
	}

	// Add user message. Image attachments are artifact references persisted
	// by handleSubmitPrompt; the wire image is derived per request by
	// media.Materialize, keeping the transcript free of base64 blobs.
	parts := make([]domain.ContentPart, 0, 1+len(imageRefs))
	parts = append(parts, domain.ContentPart{Kind: domain.PartText, Text: prompt})
	for _, ref := range imageRefs {
		ref := ref
		parts = append(parts, domain.ContentPart{Kind: domain.PartArtifact, Artifact: &ref})
	}
	userMsg := domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     parts,
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
	c.noteProjectionLocked()
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

	// Derive the model's context thresholds: the declared window (or the
	// limits fallback) scaled by the configured ratios, with an optional
	// per-model utilization override.
	window := c.windowModelFor(modelMeta)
	// Capture the per-request overhead estimate for snapshot occupancy
	// projections; refreshed per turn as the plan and tool set evolve.
	overheadTokens := int64(agent.EstimateOverheadTokens(ctx, c.bootstrap.CurrentPrompt(), current.Model, clock, run.Plan, c.runtime.Registry.List()))
	// Label the first compaction of this turn: a manual /compact request,
	// or a switch to a smaller-window model (ModelDownshift).
	c.mu.Lock()
	previousWindow := c.lastWindowNominal
	c.lastWindowNominal = window.Nominal
	c.requestOverheadTokens = overheadTokens
	c.mu.Unlock()
	triggerHint := ""
	switch {
	case forceCompact:
		triggerHint = "manual"
	case previousWindow > 0 && window.Nominal > 0 && window.Nominal < previousWindow:
		triggerHint = "downshift"
	}

	// Publish this turn's model selection for delegate_task's child
	// loops (subagent.ModelSource mailbox; docs/SUBAGENT_DESIGN.md D7).
	PublishSubagentSnapshot(c.subagentModelSource(), c.bootstrap.Resolved(), current, reasoning, c.sessionID)

	// Build the loop
	loop := &agent.Loop{
		Run:            run,
		Model:          provider.ModelFor(current.Model),
		ModelName:      current.Model,
		Store:          &publishingStore{inner: store, broker: c.broker, sessionID: c.sessionID, runID: run.ID, clock: clock, controller: c, previews: make(map[domain.ToolCallID]string), artifacts: make(map[domain.ToolCallID][]domain.ArtifactRef), pendingArgs: make(map[domain.ToolCallID]json.RawMessage)},
		Approver:       c.rulesApprover,
		Policy:         c.bootstrap.CurrentPolicy(),
		Registry:       c.runtime.Registry,
		Logger:         c.logger,
		SystemPrompt:   c.bootstrap.CurrentPrompt(),
		Artifacts:      c.bootstrap.Artifact,
		Recorder:       c.bootstrap.Recorder,
		Prompt:         prompt,
		Workspace:      c.bootstrap.WorkspaceRoot,
		Window:         window,
		Runaway:        c.bootstrap.Resolved().Runaway,
		Reasoning:      reasoning,
		SupportsImages: modelMeta.SupportsImages(),
		Compaction:     agent.CompactionControl{Force: forceCompact, TriggerHint: triggerHint},
		GoalCell:       c.runtime.GoalCell,
		PlanCell:       c.runtime.PlanCell,
		SteerCell:      c.runtime.SteerCell,
		// Reuse the tracing cost rates for the cost budget; zero when the
		// user never configured pricing, which disables cost accounting.
		CostInputUSDPerMTok:  c.bootstrap.Resolved().Tracing.CostInputPerMTok,
		CostOutputUSDPerMTok: c.bootstrap.Resolved().Tracing.CostOutputPerMTok,
		StreamHooks: agent.StreamHooks{
			OnContextUsage: func(occupancyTokens int64) {
				c.publishDurable(c.sessionID, run.ID, turnCounter, runtimeevent.KindContextUsage, runtimeevent.ContextUsagePayload{
					OccupancyTokens: occupancyTokens,
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
		c.bootstrap.Resolved().Limits,
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
		// Turns can die from causes the domain log could not represent (e.g.
		// a persistence failure before any run-failed event): keep a generic
		// failure projection so reconnecting clients still see the turn
		// failed. A preceding model.request_failed already recorded the
		// richer structured reason and wins; cancellation is a user action,
		// never an error.
		if c.lastError == nil && !errors.Is(err, context.Canceled) {
			c.lastError = &SnapshotError{Message: toolkit.Ellipsize(err.Error(), snapshotErrorMaxRunes)}
		}
	}
	if c.activeTurn != turnID {
		c.mu.Unlock()
		return
	}
	sessionID, runID := c.sessionID, c.runID
	c.turnCtx = nil
	c.cancelTurn = nil
	if c.state != ControllerStateClosed {
		c.state = ControllerStateIdle
	}
	// A finished turn has no resolvable requests left: a cancelled turn
	// skips the resolved-event cleanup path, so drop the projections here
	// or ghost approvals/questions would linger in snapshots forever
	// (review M2).
	c.pendingCards = make(map[domain.EventID]runtimeevent.ApprovalRequestedPayload)
	c.pendingQuestions = make(map[domain.EventID]domain.Question)
	c.approvalActors = make(map[domain.EventID]string)
	c.noteProjectionLocked()
	c.mu.Unlock()

	var payload any
	if err != nil && !errors.Is(err, context.Canceled) {
		// Surface turn failures the domain log could not represent (for example
		// a persistence error before the loop emitted any run-failed event).
		// Cancellation is a user action already carried by run.cancelled.
		payload = runtimeevent.TurnFinishedPayload{Error: err.Error()}
	}
	c.publishDurable(sessionID, runID, turn, runtimeevent.KindTurnFinished, payload)

	c.relayPendingSteers()
}

// relayPendingSteers forwards queued messages as the next turn's prompt.
// Steer leftovers (queued after the loop's last drain) take priority and
// merge into one prompt — mirroring dsh's next-step-before-next-turn
// claim order; otherwise exactly ONE followup relays per turn boundary,
// so a queued batch becomes one turn per message. The relay re-enters
// through cmdCh so submission stays serialized with external input; the
// result channel is buffered, so nobody has to read it. Relaying on
// every terminal outcome (completed, cancelled, failed, budget) is what
// makes Ctrl+C flush pending messages.
func (c *Controller) relayPendingSteers() {
	cell := c.steerCell()
	if cell == nil {
		return
	}
	var prompt string
	if cell.Len() > 0 {
		prompt = strings.Join(cell.Take(), "\n\n")
	} else if followup, ok := cell.TakeFollowup(); ok {
		prompt = followup
	} else {
		return
	}
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, ResultCh: make(chan controllerResult, 1)}:
	default:
		// The queue is full or the controller is shutting down; the
		// messages are lost with the process, which matches the cell's
		// volatile contract (STEER_DESIGN §3.3).
		c.logger.Warn("steer relay dropped: command queue unavailable", "messages", prompt)
	}
}

func (c *Controller) handleCancelTurn(cmd controllerCommand) {
	c.mu.Lock()
	state := c.state
	cancelTurn := c.cancelTurn
	sessionID, runID, turnCounter := c.sessionID, c.runID, c.turnCounter
	c.mu.Unlock()

	if state != ControllerStateRunning && state != ControllerStateAwaitingApproval {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot cancel in state %q", state)}
		return
	}

	if cancelTurn != nil {
		cancelTurn()
	}

	c.publishEphemeral(sessionID, runID, turnCounter, runtimeevent.KindRunCancelRequested, nil)
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

	// "Allow always" memory must land BEFORE the decision wakes the agent
	// loop: the woken loop immediately re-evaluates the batch's remaining
	// calls against session memory, and a late memory re-prompts calls the
	// user just approved categorically (the duplicate request then
	// auto-resolves underneath its still-visible card). The memory is
	// derived from the projected card — the exact call the user was
	// shown — never from client-supplied arguments, and only when the
	// card's identity matches the binding being resolved.
	var note string
	if cmd.Decision == domain.DecisionAllow && cmd.RuleHint != nil {
		note = c.rememberApprovalRule(cmd.Approval, cmd.RuleHint.Trust)
	}

	// Record the actor BEFORE resolving: the resolution wakes the turn
	// goroutine, which may persist and publish the resolved event
	// immediately — the actor must already be projected by then (review
	// M1). Rolled back when the binding does not match.
	if cmd.Actor != "" {
		c.mu.Lock()
		c.approvalActors[cmd.Approval.ApprovalID] = cmd.Actor
		c.mu.Unlock()
	}
	if !c.approver.ResolveApproval(cmd.Approval, cmd.Decision) {
		if cmd.Actor != "" {
			c.mu.Lock()
			delete(c.approvalActors, cmd.Approval.ApprovalID)
			c.mu.Unlock()
		}
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("approval binding does not match a pending request")}
		return
	}

	actor := cmd.Actor
	if actor == "" {
		actor = "local"
	}
	c.logger.Info("approval resolved", "approval_id", cmd.Approval.ApprovalID, "decision", cmd.Decision, "actor", actor)
	cmd.ResultCh <- controllerResult{Value: note}
}

// rememberApprovalRule derives and stores the capability packages for
// an approved call (session scope in the shared set, persisted when a
// remembered store is configured; rules.persist_remembered=false opts
// out by not opening one). Returns the display note, empty when the
// call is not rememberable. The derivation input is the projected
// approval card whose identity matches the binding — never the client's
// rule hint, which carries only the trust flavor.
func (c *Controller) rememberApprovalRule(binding ApprovalBinding, trust string) string {
	c.mu.Lock()
	card, ok := c.pendingCards[binding.ApprovalID]
	c.mu.Unlock()
	if !ok || card.CallID != binding.CallID || card.ArgsHash != binding.ArgsHash {
		// No card (stale/early resolution) or a card for a DIFFERENT
		// call: there is nothing trustworthy to derive memory from.
		return ""
	}
	rule, ok := c.rulesApprover.RememberCall(card.ToolName, card.Source, card.Arguments, trust)
	if !ok {
		return ""
	}
	if rule.PersistFailed {
		return rule.Label + "（仅本次会话生效：持久化失败）"
	}
	if store := c.rememberedStore(); store != nil {
		return rule.Label + " (saved to " + store.Path() + ")"
	}
	return rule.Label
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
	c.lastError = nil
	c.plan = domain.Plan{}
	c.hasPlan = false
	c.delegated = false
	c.parentSessionID = domain.SessionID{}
	c.lastUsage = domain.Usage{}
	c.resumedRun = nil
	c.resumed = false
	c.pendingCards = make(map[domain.EventID]runtimeevent.ApprovalRequestedPayload)
	c.pendingQuestions = make(map[domain.EventID]domain.Question)
	c.approvalActors = make(map[domain.EventID]string)
	// A compaction is requested against a specific transcript; it must not
	// leak into a different session's first turn.
	c.forceCompact = false
	// A new session starts from the CURRENT global preference, not whatever
	// the previous session on this controller diverged to.
	c.seedPrefsLocked()
	sessionID := c.sessionID
	c.state = ControllerStateIdle
	c.mu.Unlock()

	if err := c.bootstrap.Store.CreateSession(c.sessionCtx, sessionID, c.bootstrap.WorkspaceID); err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("create session: %w", err)}
		return
	}
	c.logger.Info("new session created", "session_id", sessionID)
	cmd.ResultCh <- controllerResult{}
}

func (c *Controller) handleAnswerQuestion(cmd controllerCommand) {
	if c.questioner == nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("question answering is unavailable")}
		return
	}
	resolved := c.questioner.Resolve(cmd.QuestionID, cmd.Answer)
	// Drop the projection entry regardless of the resolve outcome: an
	// unknown/already-resolved id can only refer to a stale card (review
	// M2 self-heal).
	c.mu.Lock()
	delete(c.pendingQuestions, cmd.QuestionID)
	c.noteProjectionLocked()
	sessionID, runID, turn := c.sessionID, c.runID, c.turnCounter
	c.mu.Unlock()
	if resolved {
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
	c.logger.Info("reasoning updated", "override", cmd.Reasoning, "effective", effective.Label())
	cmd.ResultCh <- controllerResult{Value: SetReasoningResult{Effective: effective, Overridden: overridden}}
}

func (c *Controller) handleSetModel(cmd controllerCommand) {
	if c.bootstrap == nil || c.bootstrap.Resolved() == nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("model switching is unavailable: no providers configured")}
		return
	}
	ref, err := c.bootstrap.Resolved().ResolveRef(cmd.ModelName)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: err}
		return
	}
	meta, _ := c.bootstrap.Resolved().ModelMeta(ref)
	windowInfo := c.windowInfoFor(meta)
	c.mu.Lock()
	prev := c.currentLocked()
	c.current = ref
	c.mu.Unlock()
	c.logger.Info("model switched", "previous", prev.String(), "current", ref.String())
	cmd.ResultCh <- controllerResult{Value: SetModelResult{Prev: prev, Cur: ref, Meta: meta, Window: windowInfo}}
}

// windowModelFor derives the model's context-window thresholds the one
// way every consumer shares: the declared window (or the limits fallback)
// scaled by the configured ratios, with an optional per-model utilization
// override.
func (c *Controller) windowModelFor(meta config.Model) agent.WindowModel {
	contextCfg := c.bootstrap.Resolved().Context
	if meta.WindowUtilization != nil {
		contextCfg.Utilization = *meta.WindowUtilization
	}
	return agent.NewWindowModel(meta.ContextWindow, c.bootstrap.Resolved().Limits.MaxInputTokens, contextCfg)
}

// windowInfoFor projects windowModelFor for the wire; nil when the model
// declares no usable window.
func (c *Controller) windowInfoFor(meta config.Model) *WindowInfo {
	window := c.windowModelFor(meta)
	if !window.Usable() {
		return nil
	}
	return &WindowInfo{
		Nominal:        window.Nominal,
		Effective:      window.Effective,
		CompactTrigger: window.CompactTrigger,
		CompactTarget:  window.CompactTarget,
	}
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
		c.bootstrap.Resolved().Limits, c.clock, c.bootstrap.Validator)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("recover session: %w", err)}
		return
	}

	c.mu.Lock()
	c.sessionID = cmd.SessionID
	c.runID = domain.RunID{}
	c.turnCounter = 0
	c.messages = append([]domain.Message(nil), inspection.Transcript.Messages...)
	c.lastError = lastErrorFromEvents(inspection.Events)
	c.lastUsage = run.Usage
	// Seed the plan projection from the checkpoint: the plan survives prompt
	// boundaries like the goal does, and no EventPlanRevised is re-emitted
	// for recovered state.
	c.plan = domain.Plan{}
	c.hasPlan = false
	if inspection.Checkpoint != nil && len(inspection.Checkpoint.Plan.Items) > 0 {
		c.plan = inspection.Checkpoint.Plan
		c.hasPlan = true
	}
	// Detect the delegation edge: a sub-agent child's first run.created
	// event carries delegated=true and the parent session.
	c.delegated = false
	c.parentSessionID = domain.SessionID{}
	for _, ev := range inspection.Events {
		if ev.Type != domain.EventRunCreated {
			continue
		}
		var p struct {
			Delegated       bool             `json:"delegated"`
			ParentSessionID domain.SessionID `json:"parent_session_id"`
		}
		if err := json.Unmarshal(ev.Payload, &p); err == nil && p.Delegated {
			c.delegated = true
			c.parentSessionID = p.ParentSessionID
		}
		break // only the first run.created carries the delegation edge
	}
	c.resumedRun = run
	c.resumed = true
	// Same as handleNewSession: a pending compaction belongs to the
	// transcript it was requested against, and stale request projections
	// belong to the previous session's lifetime (review L2).
	c.forceCompact = false
	c.pendingCards = make(map[domain.EventID]runtimeevent.ApprovalRequestedPayload)
	c.pendingQuestions = make(map[domain.EventID]domain.Question)
	c.approvalActors = make(map[domain.EventID]string)
	c.state = ControllerStateIdle
	c.mu.Unlock()

	c.logger.Info("session resumed", "session_id", c.sessionID)
	cmd.ResultCh <- controllerResult{}
}

// noteProjectionLocked samples the broker sequence as the applied
// watermark. Callers must hold c.mu and have just updated the snapshot
// projection: the watermark must never run ahead of the projection it
// describes, otherwise a client subscribing at the watermark would miss
// effects whose events already carry a smaller sequence
// (docs/SERVE_DESIGN.md §4.4).
func (c *Controller) noteProjectionLocked() {
	if c.broker != nil {
		c.appliedSeq = c.broker.Sequence()
	}
}

func (c *Controller) handleRequestSnapshot(cmd controllerCommand) {
	c.mu.Lock()
	current := c.currentLocked()
	var contextWindow int64
	var windowInfo *WindowInfo
	var occupancy int64
	if c.bootstrap != nil && c.bootstrap.Resolved() != nil {
		if meta, ok := c.bootstrap.Resolved().ModelMeta(current); ok {
			contextWindow = meta.ContextWindow
			windowInfo = c.windowInfoFor(meta)
			if windowInfo != nil {
				occupancy = int64(agent.EstimateTokens(c.messages)) + c.requestOverheadTokens
			}
		}
	}
	reasoning, overridden := c.reasoningLocked(current)
	pending := make([]PendingRequest, 0, len(c.pendingCards)+len(c.pendingQuestions))
	for id, card := range c.pendingCards {
		card := card
		// Deep-copy the mutable fields: snapshots cross the client boundary
		// where callers must never share mutable state with the runtime
		// (docs/SERVE_DESIGN.md §17.5, review L4).
		card.ReadPaths = append([]string(nil), card.ReadPaths...)
		card.WritePaths = append([]string(nil), card.WritePaths...)
		card.Arguments = append(json.RawMessage(nil), card.Arguments...)
		pending = append(pending, PendingRequest{Kind: PendingRequestApproval, ID: id, Approval: &card})
	}
	for id, q := range c.pendingQuestions {
		q := q
		q.Options = append([]domain.QuestionOption(nil), q.Options...)
		pending = append(pending, PendingRequest{Kind: PendingRequestQuestion, ID: id, Question: &q})
	}
	sort.Slice(pending, func(i, j int) bool {
		if pending[i].Kind != pending[j].Kind {
			return pending[i].Kind < pending[j].Kind
		}
		return pending[i].ID.String() < pending[j].ID.String()
	})
	snap := Snapshot{
		State:               c.state,
		SessionID:           c.sessionID,
		RunID:               c.runID,
		ModelName:           current.Model,
		ProviderName:        current.Provider,
		ContextWindow:       contextWindow,
		Window:              windowInfo,
		Occupancy:           occupancy,
		ReasoningEffort:     reasoning.Label(),
		ReasoningOverridden: overridden,
		WorkspaceRoot:       c.workspaceLocked(),
		TurnCount:           c.turnCounter,
		Usage:               c.lastUsage,
		Messages:            append([]domain.Message(nil), c.messages...),
		PendingApprovals:    c.approver.PendingApprovals(),
		PendingRequests:     pending,
		PendingSteers:       c.steerCellPeek(),
		PendingFollowups:    c.followupCellPeek(),
		LastError:           c.lastError,
		EventSeq:            c.appliedSeq,
		Timestamp:           c.clock.Now(),
	}
	if c.hasPlan {
		p := c.plan
		snap.Plan = &p
	}
	if c.delegated {
		snap.Delegated = true
		snap.ParentSessionID = c.parentSessionID.String()
	}
	c.mu.Unlock()
	cmd.ResultCh <- controllerResult{Value: snap}
}

// handleSubmitFeedback resolves the run's trace ID from the stamped
// assistant-message metadata and forwards the vote to the trace backend.
// The scan runs newest-first: feedback almost always targets a recent turn.
func (c *Controller) handleSubmitFeedback(cmd controllerCommand) {
	c.mu.Lock()
	traceID := ""
	for i := len(c.messages) - 1; i >= 0; i-- {
		m := c.messages[i]
		if m.Role == domain.RoleAssistant && m.Metadata["run_id"] == cmd.FeedbackRunID {
			traceID = m.Metadata["trace_id"]
			break
		}
	}
	var recorder trace.Recorder
	if c.bootstrap != nil {
		recorder = c.bootstrap.Recorder
	}
	c.mu.Unlock()

	if traceID == "" {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("%w: %s", ErrFeedbackTargetUnknown, cmd.FeedbackRunID)}
		return
	}
	// ScoreTrace is fire-and-forget by contract; a background context keeps
	// the submission alive even if the HTTP request's context is done.
	if recorder == nil || !recorder.ScoreTrace(context.Background(), traceID, FeedbackScoreName, cmd.FeedbackValue, cmd.FeedbackComment) {
		cmd.ResultCh <- controllerResult{Err: ErrTracingDisabled}
		return
	}
	cmd.ResultCh <- controllerResult{}
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
	delegated := c.delegated
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

	// Drain in-flight async sub-agents; the Manager cancels their
	// contexts and waits for their loops to persist a terminal state.
	if c.bootstrap.SubagentManager != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		c.bootstrap.SubagentManager.Shutdown(ctx)
		cancel()
	}

	// Enqueue the session for background memory extraction (P0-A): a cheap
	// upsert is all the shutdown path pays for; the startup/interval
	// pipeline drains the queue. Sub-agent sessions are skipped — their
	// content rolls up into the parent's transcript.
	if c.bootstrap.MemoryJobQueue != nil && !c.sessionID.IsZero() && !delegated {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		if err := c.bootstrap.MemoryJobQueue.EnqueueMemoryJob(ctx, c.sessionID, c.bootstrap.WorkspaceRoot); err != nil {
			c.logger.Warn("memory job enqueue failed", "error", err)
		}
		cancel()
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
// Called by the publishing store when a permission.resolved event is
// persisted — after the card was removed from pendingCards. A batch (or
// concurrent runs) can hold several pending approvals, so the state only
// leaves awaiting_approval once the LAST card resolves; flipping early
// would make the state gate reject the remaining resolves.
func (c *Controller) SetRunning() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.state == ControllerStateAwaitingApproval && len(c.pendingCards) == 0 {
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
	cmdListCheckpoints   = "list_checkpoints"
	cmdRewind            = "rewind"
	cmdSubmitFeedback    = "submit_feedback"
	cmdShutdown          = "shutdown"
)

type controllerCommand struct {
	Kind   string
	Prompt string
	Images []domain.ImageContent
	// Followup marks a busy-time submission for the next-turn queue: it
	// is held until the turn ends and relayed as the next prompt, instead
	// of steering into the running turn. Text-only, like steers. On an
	// idle session a followup simply starts the turn.
	Followup bool
	// ImageRefs carries the artifact-persisted form of Images, filled by
	// handleSubmitPrompt before the turn starts (never populated on the
	// steer path, which is text-only).
	ImageRefs  []domain.ArtifactRef
	SessionID  domain.SessionID
	ModelName  string
	Reasoning  string
	Approval   ApprovalBinding
	Decision   domain.Decision
	RuleHint   *ApprovalRuleHint
	QuestionID domain.EventID
	Answer     domain.QuestionAnswer
	// CheckpointSequence is the rewind target (cmdRewind); Limit bounds
	// list-style queries (cmdListCheckpoints).
	CheckpointSequence int64
	Limit              int
	// Actor identifies who resolved an approval (cmdResolveApproval).
	Actor string
	// Feedback carries the user-feedback vote (cmdSubmitFeedback): the
	// target run, the 0/1 vote, and an optional free-text comment.
	FeedbackRunID   string
	FeedbackValue   float64
	FeedbackComment string
	ResultCh        chan<- controllerResult
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

	// artifacts stashes the artifact references of a tool result keyed by call
	// ID over the same window, so the runtime ToolCompleted event can carry
	// them for live rendering (images and externalized outputs alike).
	artifacts map[domain.ToolCallID][]domain.ArtifactRef

	// pendingArgs stashes raw tool-call arguments keyed by call ID between
	// EventModelResponseCompleted (which carries them in the assistant
	// message) and tool preparation/approval, so edit calls can render a
	// diff for display. Entries are dropped once execution starts.
	pendingArgs map[domain.ToolCallID]json.RawMessage
}

func (s *publishingStore) CreateSession(ctx context.Context, sessionID domain.SessionID, workspaceID domain.WorkspaceID) error {
	return s.inner.CreateSession(ctx, sessionID, workspaceID)
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
	s.controller.noteProjectionLocked()
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

func (s *publishingStore) RecordFileChange(ctx context.Context, sessionID domain.SessionID, path string, beforeExisted bool, beforeHash string, beforeContent []byte, afterHash string) error {
	return s.inner.RecordFileChange(ctx, sessionID, path, beforeExisted, beforeHash, beforeContent, afterHash)
}

func (s *publishingStore) InspectSession(ctx context.Context, sessionID domain.SessionID) (domain.SessionInspection, error) {
	return s.inner.InspectSession(ctx, sessionID)
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
				Turn:      payload.Turn,
			})
		}
	case domain.EventModelResponseCompleted:
		var payload domain.ResponseCompletedPayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			// A successful call recovers any earlier failure projection
			// (e.g. a context-overflow retry that eventually went through).
			s.controller.clearLastError()
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
			completed := runtimeevent.ModelResponseCompletedPayload{
				RequestID:    requestID,
				StopReason:   stopReason,
				HasToolCalls: hasToolCalls,
				Text:         strings.Join(payload.Message.TextParts(), ""),
			}
			// Per-request usage rides on the persisted event since
			// ResponseCompletedPayload; events written before that carry no
			// usage and leave the token fields at zero.
			if payload.Usage != nil {
				completed.InputTokens = payload.Usage.InputTokens
				completed.OutputTokens = payload.Usage.OutputTokens
				completed.ReasoningTokens = payload.Usage.ReasoningTokens
			}
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelResponseCompleted, completed)
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
			s.controller.setLastError(&SnapshotError{
				Stage:   payload.Stage,
				Code:    payload.Code,
				Message: toolkit.Ellipsize(payload.Message, snapshotErrorMaxRunes),
			})
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelRequestFailed, runtimeevent.ModelRequestFailedPayload{
				RequestID: payload.RequestID,
				Stage:     payload.Stage,
				Code:      payload.Code,
				Message:   payload.Message,
			})
		}
	case domain.EventModelRequestRetrying:
		var payload modelRequestRetryingDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindModelRequestRetrying, runtimeevent.ModelRequestRetryingPayload{
				RequestID:   payload.RequestID,
				Stage:       payload.Stage,
				Code:        payload.Code,
				Message:     payload.Message,
				Attempt:     payload.Attempt,
				MaxAttempts: payload.MaxAttempts,
				WaitMs:      payload.WaitMs,
			})
		}
	case domain.EventToolCallPrepared:
		var payload toolCallAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindToolPrepared, runtimeevent.ToolPreparedPayload{
				CallID:   payload.CallID,
				ToolName: payload.Tool,
				Risk:     payload.Risk,
				Target:   toolCallTarget(payload, s.pendingArgs[payload.CallID]),
				Diff:     render.DiffForToolCall(payload.Tool, s.pendingArgs[payload.CallID], domain.ToolDiffUnbounded),
			})
		}
	case domain.EventPermissionRequested:
		var payload toolCallAuditDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			card := runtimeevent.ApprovalRequestedPayload{
				ApprovalID:  ev.ID,
				CallID:      payload.CallID,
				ToolName:    payload.Tool,
				Source:      payload.Source,
				Risk:        payload.Risk,
				Description: payload.ApprovalDesc,
				ArgsHash:    payload.ArgsHash,
				ReadPaths:   payload.ReadPaths,
				WritePaths:  payload.WritePaths,
				Arguments:   s.pendingArgs[payload.CallID],
			}
			// Surface what "allow always" would remember so frontends can label
			// (or hide) the option honestly instead of offering a no-op, the
			// L2 trust option for escalated calls, and the derived consequence
			// description — what the operation DOES, not its text. One
			// derivation feeds all three projections.
			d := permission.DeriveRawArgs(payload.Tool, payload.Source, card.Arguments, s.controller.deriveEnv())
			if preview, ok := ApprovalRulePreview(d); ok {
				card.RulePreview = preview
			}
			if RunCmdTrustPreview(d) {
				if label, _, ok := permission.MemoryPreviewLabel(d, TrustUnsandboxed); ok {
					card.TrustPreview = label
				}
			}
			card.Consequence = approvalConsequence(d)
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindApprovalRequested, card)
			// Project the card so reconnecting clients can rebuild it from
			// the snapshot (the requested event itself is not replayed into
			// the projection path). No watermark sampling here: mid-batch
			// sampling would push the watermark past projection updates that
			// only happen at batch end (review M3); a lagging watermark is
			// always safe (at worst a redundant replay).
			s.controller.mu.Lock()
			s.controller.pendingCards[ev.ID] = card
			s.controller.mu.Unlock()
			s.controller.SetAwaitingApproval()
		}
	case domain.EventPermissionResolved:
		var payload permissionResolvedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			// The approval ID is the REQUESTED event's ID recorded in the
			// payload — pendingCards, approvalActors, and every frontend's
			// approval map are all keyed by it. The resolved event's own ID
			// (ev.ID) is a fresh identifier that matches nothing; using it
			// here leaked zombie cards into snapshots and pinned the session
			// in awaiting_approval for the rest of the turn.
			approvalID := payload.ApprovalID
			if approvalID.IsZero() {
				approvalID = ev.ID // legacy events without the field
			}
			s.controller.mu.Lock()
			actor := s.controller.approvalActors[approvalID]
			delete(s.controller.approvalActors, approvalID)
			delete(s.controller.pendingCards, approvalID)
			s.controller.mu.Unlock()
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindApprovalResolved, runtimeevent.ApprovalResolvedPayload{
				ApprovalID: approvalID,
				CallID:     payload.CallID,
				Decision:   payload.Decision,
				Actor:      actor,
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
			callID, preview, artifacts := toolResultPreview(payload.Message)
			if callID.IsZero() {
				break
			}
			if preview != "" && s.previews != nil {
				s.previews[callID] = preview
			}
			if len(artifacts) > 0 && s.artifacts != nil {
				s.artifacts[callID] = artifacts
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
			artifacts := s.artifacts[payload.CallID]
			delete(s.artifacts, payload.CallID)
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindToolCompleted, runtimeevent.ToolCompletedPayload{
				CallID:       payload.CallID,
				ToolName:     payload.ToolName,
				Status:       payload.Status,
				DurationMs:   durationMs,
				Error:        payload.ErrorCode,
				ErrorMessage: payload.ErrorMessage,
				FinishedAt:   payload.FinishedAt,
				Preview:      preview,
				Artifacts:    artifacts,
			})
		}
	case domain.EventBudgetUpdated:
		var payload domain.Usage
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindBudgetUpdated, runtimeevent.BudgetUpdatedPayload{
				Turns:             payload.Turns,
				InputTokens:       payload.InputTokens,
				OutputTokens:      payload.OutputTokens,
				ToolCalls:         payload.ToolCalls,
				CachedInputTokens: payload.CachedInputTokens,
				ContextTokens:     payload.ContextTokens,
			})
		}
	case domain.EventContextCompacted:
		var payload contextCompactedDTO
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindContextCompacted, runtimeevent.ContextCompactedPayload{
				Trigger:          payload.Trigger,
				Phase:            payload.Phase,
				MaskedOutputs:    payload.MaskedOutputs,
				MaskedBytes:      payload.MaskedBytes,
				ArchivedMessages: payload.ArchivedMessages,
				EstTokensBefore:  payload.EstTokensBefore,
				EstTokensAfter:   payload.EstTokensAfter,
				Summarized:       payload.Summarized,
			})
		}
	case domain.EventBudgetNotice:
		var payload domain.BudgetNoticePayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindBudgetNotice, runtimeevent.BudgetNoticePayload{
				Text:      strings.Join(payload.Message.TextParts(), ""),
				Dimension: payload.Dimension,
				Level:     payload.Level,
			})
		}
	case domain.EventBudgetWrapupStarted:
		var payload domain.BudgetWrapupPayload
		if err := json.Unmarshal(ev.Payload, &payload); err == nil {
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindBudgetNotice, runtimeevent.BudgetNoticePayload{
				Text:      fmt.Sprintf("run budget exhausted (%s); the model is wrapping up with a final summary", payload.Dimension),
				Dimension: payload.Dimension,
				WrapUp:    true,
			})
		}
	case domain.EventPlanRevised:
		var plan domain.Plan
		if err := json.Unmarshal(ev.Payload, &plan); err == nil {
			s.controller.recordPlan(plan)
			s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindPlanUpdated, plan)
		}
	case domain.EventRunCompleted:
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRunCompleted, nil)
	case domain.EventRunFailed:
		// The bare "run failed" carries no diagnosable reason; enrich it
		// with the recorded request failure so live clients can show what
		// actually killed the run.
		message := "run failed"
		if last := s.controller.currentLastError(); last != nil && last.Message != "" {
			message = "run failed: " + last.Message
		}
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRuntimeFatal, runtimeevent.RuntimeFatalPayload{
			Message: message,
		})
	case domain.EventRunCancelled:
		s.controller.publishDurable(sessionID, s.runID, 0, runtimeevent.KindRunCancelled, nil)
	}
}

// --- Local DTOs for deserializing agent event payloads ---
// These mirror the unexported types in the agent package.

type modelRequestAuditDTO struct {
	RequestID  domain.EventID `json:"request_id"`
	ModelName  string         `json:"model_name"`
	ManifestID string         `json:"manifest_id"`
	// Turn is the 1-based prompt turn the request belongs to (0 for events
	// written before turn was persisted).
	Turn         int    `json:"turn,omitempty"`
	ManifestHash string `json:"manifest_hash"`
	PromptHash   string `json:"prompt_hash"`
}

type modelRequestFailedDTO struct {
	RequestID domain.EventID `json:"request_id"`
	Stage     string         `json:"stage"`
	Code      string         `json:"code"`
	Message   string         `json:"message,omitempty"`
}

// modelRequestRetryingDTO mirrors the agent's unexported retry payload.
type modelRequestRetryingDTO struct {
	RequestID   domain.EventID `json:"request_id"`
	Stage       string         `json:"stage"`
	Code        string         `json:"code"`
	Message     string         `json:"message,omitempty"`
	Attempt     int            `json:"attempt"`
	MaxAttempts int            `json:"max_attempts"`
	WaitMs      int64          `json:"wait_ms"`
}

// setLastError records the latest unrecovered turn failure for snapshots.
func (c *Controller) setLastError(failure *SnapshotError) {
	c.mu.Lock()
	c.lastError = failure
	c.mu.Unlock()
}

// clearLastError drops the failure projection (recovery or a new turn).
func (c *Controller) clearLastError() {
	c.mu.Lock()
	c.lastError = nil
	c.mu.Unlock()
}

// currentLastError reads the failure projection. The value is never
// mutated in place, so the returned pointer is safe to share.
func (c *Controller) currentLastError() *SnapshotError {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.lastError
}

// lastErrorFromEvents rebuilds the last-failure projection from the
// persisted timeline on session resume: a request failure sticks until a
// later successful response or a new run supersedes it. A crash-orphaned
// tail run (no terminal event — the process died mid-turn) projects an
// interruption notice, so the dead turn does not vanish silently from a
// reconnecting client's view.
func lastErrorFromEvents(events []domain.Event) *SnapshotError {
	var last *SnapshotError
	for _, ev := range events {
		switch ev.Type {
		case domain.EventModelRequestFailed:
			var payload modelRequestFailedDTO
			if err := json.Unmarshal(ev.Payload, &payload); err == nil {
				last = &SnapshotError{
					Stage:   payload.Stage,
					Code:    payload.Code,
					Message: toolkit.Ellipsize(payload.Message, snapshotErrorMaxRunes),
				}
			}
		case domain.EventModelResponseCompleted, domain.EventRunCreated:
			last = nil
		}
	}
	if last == nil && !agent.OrphanedRunID(events).IsZero() {
		last = &SnapshotError{Message: "previous turn ended before completing (crash, kill, or rewind); the session recovered and can continue"}
	}
	return last
}

// contextCompactedDTO mirrors the agent's unexported compaction payload.
type contextCompactedDTO struct {
	Trigger          string `json:"trigger"`
	Phase            string `json:"phase"`
	MaskedOutputs    int    `json:"masked_outputs"`
	MaskedBytes      int    `json:"masked_bytes"`
	ArchivedMessages int    `json:"archived_messages,omitempty"`
	EstTokensBefore  int    `json:"est_tokens_before"`
	EstTokensAfter   int    `json:"est_tokens_after"`
	Summarized       bool   `json:"summarized,omitempty"`
}

type toolCallAuditDTO struct {
	CallID       domain.ToolCallID `json:"call_id"`
	Tool         string            `json:"tool"`
	Source       domain.ToolSource `json:"source,omitempty"`
	Risk         domain.RiskLevel  `json:"risk"`
	ArgsHash     string            `json:"args_hash,omitempty"`
	ReadPaths    []string          `json:"read_paths,omitempty"`
	WritePaths   []string          `json:"write_paths,omitempty"`
	ApprovalDesc string            `json:"approval_desc,omitempty"`
}

type permissionResolvedDTO struct {
	// ApprovalID echoes the permission.requested event ID (the approval ID)
	// recorded by the agent loop. Events persisted before this field existed
	// leave it zero; the handler falls back to the resolved event's own ID.
	ApprovalID domain.EventID    `json:"approval_id,omitempty"`
	CallID     domain.ToolCallID `json:"call_id"`
	Decision   domain.Decision   `json:"decision"`
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
// call. For run_cmd the read/write paths are always the workspace root
// (enforcement boundary, not the interesting target): the command line
// reconstructed from the call arguments is what the user wants to see, with
// the approval description as fallback (e.g. pending-args cap overflow).
// For all other tools: the first write path, then the first read path,
// then the approval description.
func toolCallTarget(audit toolCallAuditDTO, args json.RawMessage) string {
	if audit.Tool == "run_cmd" {
		if cmd := runCmdCommandLine(args); cmd != "" {
			return cmd
		}
		return audit.ApprovalDesc
	}
	if len(audit.WritePaths) > 0 {
		return audit.WritePaths[0]
	}
	if len(audit.ReadPaths) > 0 {
		return audit.ReadPaths[0]
	}
	return audit.ApprovalDesc
}

// runCmdCommandLine returns the displayed command line for a run_cmd
// call: the single 'command' string, verbatim.
func runCmdCommandLine(args json.RawMessage) string {
	if len(args) == 0 {
		return ""
	}
	var parsed struct {
		Command string `json:"command"`
	}
	if err := json.Unmarshal(args, &parsed); err != nil {
		return ""
	}
	return parsed.Command
}

// Bounds for the tool result preview carried by runtime ToolCompleted
// events, and for rendered argument diffs: domain.ToolPreviewMaxLines,
// domain.ToolPreviewMaxBytes, domain.ToolDiffMaxLines (single home,
// REVIEW R8).

// pendingArgsCap bounds the stashed-arguments map against leaks from calls
// that never reach execution (e.g. denied approvals).
const pendingArgsCap = 256

// toolResultPreview extracts a bounded text excerpt from a tool-result
// message (the joined text parts, falling back to the error message) plus
// the displayable artifact references carried by the result content.
// Model-only artifacts (view_image) are excluded: live clients render the
// text header as the audit reference and must not render the image itself.
func toolResultPreview(msg domain.Message) (domain.ToolCallID, string, []domain.ArtifactRef) {
	for _, part := range msg.Parts {
		if part.Kind != domain.PartToolResult || part.ToolResult == nil {
			continue
		}
		result := part.ToolResult
		var b strings.Builder
		var artifacts []domain.ArtifactRef
		for _, cp := range result.Content {
			switch cp.Kind {
			case domain.PartText:
				b.WriteString(cp.Text)
			case domain.PartArtifact:
				if cp.Artifact != nil && !cp.ModelOnly {
					artifacts = append(artifacts, *cp.Artifact)
				}
			}
		}
		text := b.String()
		if strings.TrimSpace(text) == "" && result.Error != nil {
			text = result.Error.Message
		}
		return result.CallID, boundPreviewLines(text, domain.ToolPreviewMaxLines, domain.ToolPreviewMaxBytes), artifacts
	}
	return domain.ToolCallID{}, "", nil
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
		out = domain.TruncateAtRuneBoundary(out, maxBytes)
		truncated = true
	}
	if truncated {
		out += "\n…"
	}
	return out
}
