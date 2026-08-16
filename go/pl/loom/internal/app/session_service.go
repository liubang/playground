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
// Created: 2026/08/03

package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// Typed sentinel errors of the protocol-agnostic application layer
// (docs/SERVE_DESIGN.md §17.5): transport adapters map them to their own
// error model (HTTP status codes today, JSON-RPC/gRPC codes tomorrow).
var (
	// ErrDraining reports that the service is shutting down and rejects new
	// sessions and prompts.
	ErrDraining = errors.New("session service is draining")
	// ErrSessionNotFound reports a command against a session with no live handle.
	ErrSessionNotFound = errors.New("session not found")
	// ErrCursorInvalid reports that a SubscribeEvents cursor can no longer be
	// honored (rotated out, or from a previous process lifetime); the caller
	// must resync via Snapshot.
	ErrCursorInvalid = errors.New("event cursor can no longer be honored")
	// ErrTooManySessions reports that the live-session limit is reached.
	ErrTooManySessions = errors.New("live session limit reached")
	// ErrTooManyTurns reports that the global active-turn gate rejected a new turn.
	ErrTooManyTurns = errors.New("too many active turns")
	// ErrTracingDisabled reports a feedback submission while the trace
	// backend is not configured: the vote cannot be recorded anywhere.
	ErrTracingDisabled = errors.New("tracing is disabled")
	// ErrFeedbackTargetUnknown reports a feedback submission whose run_id
	// matches no traced assistant message in the transcript (e.g. a turn
	// from before trace stamping, or compacted away).
	ErrFeedbackTargetUnknown = errors.New("no trace found for run")
	// ErrShareNotFound reports a share link that was revoked or never
	// created (the token is the only credential for the public routes).
	ErrShareNotFound = errors.New("share not found")
	// ErrSharedArtifactUnknown reports an artifact read through a share
	// link for a blob the shared session never referenced.
	ErrSharedArtifactUnknown = errors.New("shared artifact not found")
)

// SessionService resource defaults (docs/SERVE_DESIGN.md §7.2).
const (
	defaultMaxSessions     = 32
	defaultIdleTTL         = 30 * time.Minute
	defaultMaxActiveTurns  = 4
	defaultSubscriberQueue = 256
	idempotencyCap         = 128
)

// SessionServiceConfig tunes a SessionService; zero fields take defaults.
type SessionServiceConfig struct {
	// MaxSessions bounds concurrently live session handles.
	MaxSessions int
	// IdleTTL reclaims idle handles after this inactivity; <= 0 disables sweeping.
	IdleTTL time.Duration
	// ReplayCap is the per-session replay ring capacity.
	ReplayCap int
	// MaxActiveTurns gates globally concurrent turns; <= 0 means unlimited.
	MaxActiveTurns int
	// SubscriberQueue is the per-subscription live-event buffer; a slow
	// subscriber that fills it is disconnected (and resyncs via cursor).
	SubscriberQueue int
	// ShareEndpoint, when non-nil, is reconciled on config hot-apply (the
	// share section changes take effect immediately); nil leaves the
	// listener lifecycle to the caller alone.
	ShareEndpoint ShareEndpointController
	// RulesDir is the user-layer rules directory (<loom home>/rules).
	// Rule pack install/uninstall writes pack-*.json files here; empty
	// disables pack management.
	RulesDir string
	// DisableApprovalNotify suppresses the desktop notification fired when
	// a run blocks on an approval. Set by frontends that already surface
	// the request prominently (WebUI) or mirror it to the notification
	// center themselves (desktop app).
	DisableApprovalNotify bool
	Logger                *slog.Logger
}

// ShareEndpointController is the process-level LAN share listener
// (server.ShareManager), abstracted here to keep app free of the
// transport package. Apply reconciles the listener to the desired
// state; a start failure is returned but must not fail the hot-apply.
type ShareEndpointController interface {
	Apply(enabled bool, listen string) error
}

// SessionHandle owns one live session: its controller, isolated runtime,
// replay log, subscription fan-out, and idempotency cache.
type SessionHandle struct {
	ID domain.SessionID
	// WorkspaceID is the owning workspace (docs/WORKSPACE_DESIGN.md W1).
	WorkspaceID domain.WorkspaceID
	Controller  *Controller
	Approver    *ChannelApprover
	Runtime     *SessionRuntime
	Replay      *runtimeevent.ReplayLog

	mu           sync.Mutex
	subscribers  map[uint64]chan runtimeevent.RuntimeEvent
	nextSubID    uint64
	idem         map[string]SubmitResult
	idemOrder    []string
	idemInFlight map[string]*idemFlight

	lastActiveNanos atomic.Int64
}

// idemFlight is the single-flight slot for an in-progress idempotent
// submission: concurrent retries wait on done and share the first result
// instead of re-executing the turn (review M7).
type idemFlight struct {
	done chan struct{}
	res  SubmitResult
	err  error
}

func (h *SessionHandle) touch() {
	h.lastActiveNanos.Store(time.Now().UnixNano())
}

func (h *SessionHandle) removeSubscriber(id uint64) {
	h.mu.Lock()
	delete(h.subscribers, id)
	h.mu.Unlock()
}

// dropSubscribers closes every live subscription channel of this handle
// (idle reclaim, pump resync, service shutdown); forwarders observe the
// close and exit, and clients resync.
func (h *SessionHandle) dropSubscribers() {
	h.mu.Lock()
	channels := make([]chan runtimeevent.RuntimeEvent, 0, len(h.subscribers))
	for id, ch := range h.subscribers {
		channels = append(channels, ch)
		delete(h.subscribers, id)
	}
	h.mu.Unlock()
	for _, ch := range channels {
		close(ch)
	}
}

func (h *SessionHandle) rememberIdem(key string, res SubmitResult) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, exists := h.idem[key]; !exists {
		h.idemOrder = append(h.idemOrder, key)
	}
	h.idem[key] = res
	for len(h.idemOrder) > idempotencyCap {
		oldest := h.idemOrder[0]
		h.idemOrder = h.idemOrder[1:]
		delete(h.idem, oldest)
	}
}

// SessionService owns every live session in a process. It replaces the
// single-Controller assumption in cmd/loom with a registry that any
// transport (in-process client today, HTTP/SSE tomorrow) multiplexes
// (docs/SERVE_DESIGN.md §4.1). It also owns the single event pump:
// subscribers never touch the broker directly, so a slow client can never
// stall the runtime.
type SessionService struct {
	proc     *ProcessRuntime
	registry *WorkspaceRegistry
	broker   *runtimeevent.Broker
	logger   *slog.Logger

	maxSessions           int
	idleTTL               time.Duration
	replayCap             int
	maxActiveTurns        int
	subscriberQueue       int
	shareEndpoint         ShareEndpointController
	rulesDir              string
	disableApprovalNotify bool

	mu       sync.Mutex
	sessions map[domain.SessionID]*SessionHandle
	closing  bool

	// applyMu serializes config hot-applies and MCP reconnects (they may
	// block on MCP connect timeouts; two applies must never interleave).
	applyMu sync.Mutex

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// NewSessionService creates the service and starts its event pump and idle
// sweeper. The broker should be constructed with a generous durable queue
// (e.g. runtimeevent.WithDurableQueue(4096)) — the pump is the only broker
// subscriber that must never fall behind. Sessions are assembled against
// workspaces resolved through reg (docs/WORKSPACE_DESIGN.md §5.3).
func NewSessionService(proc *ProcessRuntime, reg *WorkspaceRegistry, broker *runtimeevent.Broker, cfg SessionServiceConfig) *SessionService {
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	s := &SessionService{
		proc:                  proc,
		registry:              reg,
		broker:                broker,
		logger:                logger,
		rulesDir:              cfg.RulesDir,
		maxSessions:           cfg.MaxSessions,
		idleTTL:               cfg.IdleTTL,
		replayCap:             cfg.ReplayCap,
		maxActiveTurns:        cfg.MaxActiveTurns,
		subscriberQueue:       cfg.SubscriberQueue,
		shareEndpoint:         cfg.ShareEndpoint,
		disableApprovalNotify: cfg.DisableApprovalNotify,
		sessions:              make(map[domain.SessionID]*SessionHandle),
	}
	if s.maxSessions <= 0 {
		s.maxSessions = defaultMaxSessions
	}
	if s.idleTTL == 0 {
		s.idleTTL = defaultIdleTTL
	}
	if s.maxActiveTurns == 0 {
		s.maxActiveTurns = defaultMaxActiveTurns
	}
	if s.subscriberQueue <= 0 {
		s.subscriberQueue = defaultSubscriberQueue
	}
	s.ctx, s.cancel = context.WithCancel(context.Background())
	s.wg.Add(1)
	go s.pump()
	if s.idleTTL > 0 {
		s.wg.Add(1)
		go s.sweeper()
	}
	return s
}

// newHandle assembles one live session on the given workspace's bootstrap:
// isolated runtime (fresh cells), its own approver/questioner, and a
// controller running on the service lifetime context.
func (s *SessionService) newHandle(ws *Bootstrap) (*SessionHandle, error) {
	questioner := NewChannelQuestioner(nil)
	runtime, err := NewIsolatedSessionRuntime(ws, questioner)
	if err != nil {
		return nil, fmt.Errorf("build session runtime: %w", err)
	}
	var approverOpts []ChannelApproverOption
	if s.disableApprovalNotify {
		approverOpts = append(approverOpts, WithoutApprovalNotify())
	}
	approver := NewChannelApprover(approverOpts...)
	controller := NewController(ControllerConfig{
		Bootstrap: ws,
		Broker:    s.broker,
		Approver:  approver,
		Runtime:   runtime,
		Logger:    s.logger,
	})
	h := &SessionHandle{
		WorkspaceID:  ws.WorkspaceID,
		Controller:   controller,
		Approver:     approver,
		Runtime:      runtime,
		Replay:       runtimeevent.NewReplayLog(s.replayCap),
		subscribers:  make(map[uint64]chan runtimeevent.RuntimeEvent),
		idem:         make(map[string]SubmitResult),
		idemInFlight: make(map[string]*idemFlight),
	}
	go controller.Run(s.ctx)
	h.touch()
	return h, nil
}

// workspaceStore returns the process store as a WorkspaceStore when it
// implements the workspace persistence contract (session.SQLiteStore does).
func (s *SessionService) workspaceStore() (domain.WorkspaceStore, bool) {
	store, ok := s.proc.Store.(domain.WorkspaceStore)
	return store, ok
}

// DefaultWorkspaceID returns the process's default workspace (the launch
// directory), zero when none was registered (hand-assembled test services).
func (s *SessionService) DefaultWorkspaceID() domain.WorkspaceID {
	return s.registry.DefaultID()
}

// ListWorkspaces returns every registered workspace, newest first.
func (s *SessionService) ListWorkspaces(ctx context.Context) ([]domain.Workspace, error) {
	return s.registry.List(ctx)
}

// RegisterWorkspace registers (or reuses by canonical root) a workspace and
// returns its persisted entity (docs/WORKSPACE_DESIGN.md §8.1).
func (s *SessionService) RegisterWorkspace(ctx context.Context, root, name string) (domain.Workspace, error) {
	rt, err := s.registry.Register(ctx, root, name)
	if err != nil {
		return domain.Workspace{}, err
	}
	store, ok := s.workspaceStore()
	if !ok {
		return domain.Workspace{ID: rt.WorkspaceID, Name: name, RootPath: rt.WorkspaceRoot}, nil
	}
	return store.GetWorkspace(ctx, rt.WorkspaceID)
}

// GetWorkspace returns the workspace with the given ID. A missing row —
// which the SQLite store reports as ErrUnavailable("workspace not found") —
// is normalized to ErrWorkspaceNotFound so transports map it to 404
// (docs/WORKSPACE_DESIGN.md §8.1); genuine store failures pass through.
func (s *SessionService) GetWorkspace(ctx context.Context, id domain.WorkspaceID) (domain.Workspace, error) {
	store, ok := s.workspaceStore()
	if !ok {
		return domain.Workspace{}, ErrWorkspaceNotFound
	}
	ws, err := store.GetWorkspace(ctx, id)
	if err != nil {
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) && agentErr.Code == domain.ErrUnavailable && agentErr.Message == "workspace not found" {
			return domain.Workspace{}, ErrWorkspaceNotFound
		}
		return domain.Workspace{}, err
	}
	return ws, nil
}

// CountSessionsPerWorkspace returns per-workspace session counts for the
// list-workspaces endpoint.
func (s *SessionService) CountSessionsPerWorkspace(ctx context.Context) (map[domain.WorkspaceID]int, error) {
	store, ok := s.workspaceStore()
	if !ok {
		return map[domain.WorkspaceID]int{}, nil
	}
	return store.CountSessionsPerWorkspace(ctx)
}

// DeleteWorkspace removes a workspace entity and cascades to its sessions
// (docs/WORKSPACE_DESIGN.md §16.1): the persisted row together with every
// session it owns, the registry's in-memory indexes, and the assembled
// runtime. Live sessions are torn down (subscribers dropped, controllers
// shut down) instead of blocking the deletion. The on-disk root directory
// is never touched. Deleting the default workspace is allowed — the
// registry auto-re-pins the default to the newest remaining workspace, or
// clears it when no workspaces remain.
//
// Concurrency: the registry deletion and the live-handle eviction run
// inside one s.mu critical section, and CreateSession/ResumeSession
// re-verify the workspace's registry membership when they insert their
// handle under the same lock — a session can therefore never come alive on
// a deleted workspace in either interleaving. Controller shutdown and the
// runtime close happen after the lock is released so the critical section
// stays non-blocking. The store rows are already gone while the evicted
// controllers wind down: an in-flight turn may log a failed event append
// in that window, which is harmless — the turn is being torn down anyway.
func (s *SessionService) DeleteWorkspace(ctx context.Context, id domain.WorkspaceID) error {
	if id.IsZero() {
		return ErrWorkspaceNotFound
	}
	s.mu.Lock()
	rt, err := s.registry.Delete(ctx, id)
	if err != nil {
		s.mu.Unlock()
		return err
	}
	live := make([]*SessionHandle, 0)
	for sid, h := range s.sessions {
		if h.WorkspaceID == id {
			live = append(live, h)
			delete(s.sessions, sid)
		}
	}
	s.mu.Unlock()
	for _, h := range live {
		h.dropSubscribers()
		if err := h.Controller.Shutdown(ctx); err != nil {
			s.logger.Warn("shutdown during workspace delete failed", "session_id", h.ID, "workspace_id", id, "error", err)
		}
	}
	if rt != nil {
		rt.Close()
	}
	return nil
}

// resolveWorkspace resolves the workspace a session is assembled against.
// A zero ID falls back to the process's default workspace (legacy clients,
// docs/WORKSPACE_DESIGN.md W5).
func (s *SessionService) resolveWorkspace(ctx context.Context, id domain.WorkspaceID) (*Bootstrap, error) {
	if id.IsZero() {
		if ws := s.registry.Default(); ws != nil {
			return ws, nil
		}
		return nil, ErrWorkspaceNotFound
	}
	return s.registry.Resolve(ctx, id)
}

// sessionWorkspace looks up a session's owning workspace for resume. A
// lookup failure (session not found, or a pre-v5 row still carrying the
// migration default ”) resolves to the zero ID — the caller falls back to
// the default workspace and the controller reports the authoritative
// resume error for a genuinely missing session.
func (s *SessionService) sessionWorkspace(ctx context.Context, id domain.SessionID) domain.WorkspaceID {
	store, ok := s.proc.Store.(domain.WorkspaceStore)
	if !ok {
		return domain.WorkspaceID{}
	}
	wsID, err := store.SessionWorkspace(ctx, id)
	if err != nil {
		return domain.WorkspaceID{}
	}
	return wsID
}

// CreateSession starts a brand-new session in the given workspace and
// returns its live handle. A zero workspaceID uses the default workspace.
func (s *SessionService) CreateSession(ctx context.Context, workspaceID domain.WorkspaceID) (*SessionHandle, error) {
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return nil, ErrDraining
	}
	if len(s.sessions) >= s.maxSessions {
		s.mu.Unlock()
		return nil, ErrTooManySessions
	}
	s.mu.Unlock()

	ws, err := s.resolveWorkspace(ctx, workspaceID)
	if err != nil {
		return nil, err
	}
	h, err := s.newHandle(ws)
	if err != nil {
		return nil, err
	}
	if err := h.Controller.NewSession(ctx); err != nil {
		_ = h.Controller.Shutdown(context.Background())
		return nil, fmt.Errorf("create session: %w", err)
	}
	h.ID = h.Controller.SessionID()

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closing {
		_ = h.Controller.Shutdown(context.Background())
		return nil, ErrDraining
	}
	// The workspace may have been deleted while this handle was being built
	// (DeleteWorkspace holds s.mu across its live-check and registry
	// eviction, so this insertion is serialized against it): never bring a
	// session alive on a deleted workspace.
	if _, ok := s.registry.Get(h.WorkspaceID); !ok {
		_ = h.Controller.Shutdown(context.Background())
		return nil, ErrWorkspaceNotFound
	}
	s.sessions[h.ID] = h
	return h, nil
}

// ResumeSession attaches to an existing persisted session; when the session
// is already live it returns the existing handle (one SessionID has at most
// one Controller process-wide).
func (s *SessionService) ResumeSession(ctx context.Context, id domain.SessionID) (*SessionHandle, error) {
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return nil, ErrDraining
	}
	if h, ok := s.sessions[id]; ok {
		s.mu.Unlock()
		h.touch()
		return h, nil
	}
	if len(s.sessions) >= s.maxSessions {
		s.mu.Unlock()
		return nil, ErrTooManySessions
	}
	s.mu.Unlock()

	// Assemble against the session's owning workspace (W1): the persisted
	// workspace_id determines which PathValidator/policy/prompt assembly the
	// resumed runtime binds to.
	ws, err := s.resolveWorkspace(ctx, s.sessionWorkspace(ctx, id))
	if err != nil {
		return nil, err
	}
	h, err := s.newHandle(ws)
	if err != nil {
		return nil, err
	}
	if err := h.Controller.ResumeSession(ctx, id); err != nil {
		_ = h.Controller.Shutdown(context.Background())
		return nil, fmt.Errorf("resume session: %w", err)
	}
	h.ID = id

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closing {
		_ = h.Controller.Shutdown(context.Background())
		return nil, ErrDraining
	}
	// Same workspace-deletion race guard as CreateSession (see above).
	if _, ok := s.registry.Get(h.WorkspaceID); !ok {
		_ = h.Controller.Shutdown(context.Background())
		return nil, ErrWorkspaceNotFound
	}
	if existing, ok := s.sessions[id]; ok {
		// Lost a resume race against another caller; discard ours.
		go func() { _ = h.Controller.Shutdown(context.Background()) }()
		return existing, nil
	}
	s.sessions[id] = h
	return h, nil
}

// Get returns the live handle for id without creating it.
func (s *SessionService) Get(id domain.SessionID) (*SessionHandle, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	h, ok := s.sessions[id]
	return h, ok
}

func (s *SessionService) handle(id domain.SessionID) (*SessionHandle, error) {
	h, ok := s.Get(id)
	if !ok {
		return nil, ErrSessionNotFound
	}
	return h, nil
}

// sessionTitleMaxRunes bounds the picker title derived from the first
// user prompt (docs/WEB_DESIGN.md §7.7).
const sessionTitleMaxRunes = 50

// ModelCatalogEntry is one selectable model in the picker wire shape.
type ModelCatalogEntry struct {
	Provider      string `json:"provider"`
	Name          string `json:"name"`
	ContextWindow int64  `json:"context_window,omitempty"`
	// Modalities declares the model's input modalities (e.g. ["text",
	// "image"]); empty means text-only. The frontend uses it to badge
	// vision-capable models and to gate the image-attachment affordances.
	Modalities []string `json:"modalities,omitempty"`
}

// ModelCatalog is the wire shape of GET /v1/meta/models: every selectable
// model plus the process default (docs/WEB_DESIGN.md — 模型切换器数据源)。
type ModelCatalog struct {
	Models []ModelCatalogEntry `json:"models"`
	// Default is the configured default selection, "provider/model".
	Default string `json:"default"`
}

// ModelCatalog returns the configured model catalog for frontend pickers.
// Default reflects the runtime-current selection (a persisted manual
// switch wins over the configured default).
func (s *SessionService) ModelCatalog() ModelCatalog {
	resolved := s.proc.Resolved()
	catalog := ModelCatalog{Default: s.proc.CurrentModel().String()}
	for i := range resolved.Providers {
		p := &resolved.Providers[i]
		for _, m := range p.Models {
			catalog.Models = append(catalog.Models, ModelCatalogEntry{
				Provider: p.Name, Name: m.Name, ContextWindow: m.ContextWindow, Modalities: m.Modalities,
			})
		}
	}
	return catalog
}

// summarizeSessionTitle collapses whitespace and rune-truncates a first
// user prompt into a one-line picker title.
func summarizeSessionTitle(text string) string {
	text = strings.Join(strings.Fields(text), " ")
	runes := []rune(text)
	if len(runes) > sessionTitleMaxRunes {
		return string(runes[:sessionTitleMaxRunes-1]) + "…"
	}
	return text
}

// ListSessions returns one page of persisted sessions, including non-live
// ones, enriched for frontend pickers (docs/WEB_DESIGN.md §7.7): every row
// gets a title (first user prompt); live sessions additionally report their
// controller state, model and turn count — non-live rows report
// state=closed. archived selects the archived view. cursor ("" = first
// page) enables keyset pagination for infinite-scroll pickers; nextCursor
// is "" when the page is the last one.
func (s *SessionService) ListSessions(ctx context.Context, cursor string, limit int, archived bool, workspaceID domain.WorkspaceID) ([]SessionSummary, string, error) {
	store, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return nil, "", fmt.Errorf("session listing is unavailable for this store")
	}
	summaries, nextCursor, err := store.ListSessions(ctx, cursor, limit, archived, workspaceID)
	if err != nil {
		return nil, "", err
	}
	ids := make([]domain.SessionID, len(summaries))
	for i, summary := range summaries {
		ids[i] = summary.ID
	}
	// Titles are best-effort enrichment: a title failure must not fail the
	// listing itself.
	titles, err := store.FirstUserMessageTexts(ctx, ids)
	if err != nil {
		titles = nil
	}
	result := make([]SessionSummary, len(summaries))
	for i, summary := range summaries {
		item := SessionSummary{
			ID: summary.ID, Version: summary.Version,
			CreatedAt: summary.CreatedAt, UpdatedAt: summary.UpdatedAt,
			WorkspaceID: summary.WorkspaceID.String(),
			State:       ControllerStateClosed,
			Title:       summarizeSessionTitle(titles[summary.ID]),
		}
		if !summary.ParentSessionID.IsZero() {
			item.ParentSessionID = summary.ParentSessionID.String()
		}
		if h, ok := s.Get(summary.ID); ok {
			item.State = h.Controller.State()
			if snap, err := h.Controller.RequestSnapshot(ctx); err == nil {
				item.ModelName = snap.ModelName
				item.TurnCount = snap.TurnCount
			}
		}
		result[i] = item
	}
	return result, nextCursor, nil
}

// DeleteSession removes a session and all its persisted data. A live
// handle is shut down first so no in-flight turn keeps writing into a
// deleted session.
func (s *SessionService) DeleteSession(ctx context.Context, id domain.SessionID) error {
	s.mu.Lock()
	h, live := s.sessions[id]
	if live {
		delete(s.sessions, id)
	}
	s.mu.Unlock()
	if live {
		h.dropSubscribers()
		if err := h.Controller.Shutdown(ctx); err != nil {
			s.logger.Warn("shutdown before delete failed", "session_id", id, "error", err)
		}
	}
	store, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return fmt.Errorf("session deletion is unavailable for this store")
	}
	return store.DeleteSession(ctx, id)
}

// SetSessionArchived marks a session archived (hidden from default session
// listings) or restores it to active. The live runtime is unaffected.
func (s *SessionService) SetSessionArchived(ctx context.Context, id domain.SessionID, archived bool) error {
	store, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return fmt.Errorf("session archiving is unavailable for this store")
	}
	return store.SetSessionArchived(ctx, id, archived)
}

// SubmitPrompt forwards a prompt to the session's controller. While the
// session is busy the prompt steers the active turn (TUI semantics,
// docs/SERVE_DESIGN.md §16.3 D1); with followup=true it queues for AFTER
// the busy turn instead (next-turn delivery, one per turn boundary).
// Followups are text-only. idemKey, when non-empty, makes the call
// idempotent within this process: a repeat returns the first result with
// deduplicated=true (§4.7).
func (s *SessionService) SubmitPrompt(ctx context.Context, id domain.SessionID, prompt string, images []domain.ImageContent, idemKey string, followup bool) (result SubmitResult, deduplicated bool, err error) {
	if s.isClosing() {
		return SubmitResult{}, false, ErrDraining
	}
	if followup && len(images) > 0 {
		return SubmitResult{}, false, domain.NewError(domain.ErrInvalidInput, "followups are text-only; send images with a regular prompt")
	}
	h, err := s.handle(id)
	if err != nil {
		return SubmitResult{}, false, err
	}
	if idemKey != "" {
		h.mu.Lock()
		if res, ok := h.idem[idemKey]; ok {
			h.mu.Unlock()
			h.touch()
			return res, true, nil
		}
		if flight, ok := h.idemInFlight[idemKey]; ok {
			// A concurrent retry of the same key is already executing:
			// wait for it and share its result instead of running twice.
			h.mu.Unlock()
			select {
			case <-flight.done:
				h.touch()
				return flight.res, true, flight.err
			case <-ctx.Done():
				return SubmitResult{}, false, ctx.Err()
			}
		}
		flight := &idemFlight{done: make(chan struct{})}
		h.idemInFlight[idemKey] = flight
		h.mu.Unlock()
		defer func() {
			h.mu.Lock()
			delete(h.idemInFlight, idemKey)
			flight.res, flight.err = result, err
			close(flight.done)
			h.mu.Unlock()
		}()
	}
	// Global active-turn gate: only brand-new turns count; steering a busy
	// session is always allowed. This is a soft TOCTOU gate (state may
	// change between the check and the controller command) — a backpressure
	// hint, not a hard concurrency guarantee.
	if s.maxActiveTurns > 0 && h.Controller.State() == ControllerStateIdle && s.activeTurns() >= s.maxActiveTurns {
		return SubmitResult{}, false, ErrTooManyTurns
	}
	if followup {
		result, err = h.Controller.SubmitFollowup(ctx, prompt)
	} else {
		result, err = h.Controller.SubmitPromptWithImages(ctx, prompt, images)
	}
	if err != nil {
		return SubmitResult{}, false, err
	}
	if idemKey != "" {
		h.rememberIdem(idemKey, result)
	}
	h.touch()
	return result, false, nil
}

// CancelTurn cancels the session's active turn.
func (s *SessionService) CancelTurn(ctx context.Context, id domain.SessionID) error {
	h, err := s.handle(id)
	if err != nil {
		return err
	}
	h.touch()
	return h.Controller.CancelTurn(ctx)
}

// ResolveApproval resolves a pending approval with the resolving actor's
// identity (audit + approval.resolved payload, §4.6).
func (s *SessionService) ResolveApproval(ctx context.Context, id domain.SessionID, binding ApprovalBinding, decision domain.Decision, hint *ApprovalRuleHint, actor string) (string, error) {
	h, err := s.handle(id)
	if err != nil {
		return "", err
	}
	h.touch()
	return h.Controller.ResolveApprovalWithActor(ctx, binding, decision, hint, actor)
}

// AnswerQuestion resolves a pending ask_user question.
func (s *SessionService) AnswerQuestion(ctx context.Context, id domain.SessionID, questionID domain.EventID, answer domain.QuestionAnswer) (AnswerQuestionResult, error) {
	h, err := s.handle(id)
	if err != nil {
		return AnswerQuestionResult{}, err
	}
	h.touch()
	return h.Controller.AnswerQuestion(ctx, questionID, answer)
}

// SubmitFeedback records a user vote (1 = up, 0 = down) for one run of the
// session, forwarded to the trace backend as a score (see
// Controller.SubmitFeedback).
func (s *SessionService) SubmitFeedback(ctx context.Context, id domain.SessionID, runID string, value float64, comment string) error {
	h, err := s.handle(id)
	if err != nil {
		return err
	}
	h.touch()
	return h.Controller.SubmitFeedback(ctx, runID, value, comment)
}

// Snapshot returns the session's live projection, including the event
// watermark (Snapshot.EventSeq) for a gapless snapshot+delta handoff.
func (s *SessionService) Snapshot(ctx context.Context, id domain.SessionID) (Snapshot, error) {
	h, err := s.handle(id)
	if err != nil {
		return Snapshot{}, err
	}
	return h.Controller.RequestSnapshot(ctx)
}

// SetModel switches the session's model from the next turn on. The choice
// also becomes the process-level preference (persisted), so sessions
// created afterwards start from it.
func (s *SessionService) SetModel(ctx context.Context, id domain.SessionID, ref string) (SetModelResult, error) {
	h, err := s.handle(id)
	if err != nil {
		return SetModelResult{}, err
	}
	h.touch()
	result, err := h.Controller.SetModel(ctx, ref)
	if err != nil {
		return SetModelResult{}, err
	}
	s.proc.SetModelPreference(ctx, result.Cur)
	return result, nil
}

// SetReasoning sets the session-scoped reasoning dial. Like SetModel, the
// choice is persisted as the process-level preference for future sessions.
func (s *SessionService) SetReasoning(ctx context.Context, id domain.SessionID, arg string) (SetReasoningResult, error) {
	h, err := s.handle(id)
	if err != nil {
		return SetReasoningResult{}, err
	}
	h.touch()
	result, err := h.Controller.SetReasoning(ctx, arg)
	if err != nil {
		return SetReasoningResult{}, err
	}
	s.proc.SetReasoningPreference(ctx, strings.TrimSpace(arg))
	return result, nil
}

// RequestCompaction schedules a forced compaction for the session's next turn.
func (s *SessionService) RequestCompaction(ctx context.Context, id domain.SessionID) (RequestCompactionResult, error) {
	h, err := s.handle(id)
	if err != nil {
		return RequestCompactionResult{}, err
	}
	h.touch()
	return h.Controller.RequestCompaction(ctx)
}

// Inspect returns the persisted metadata of a session (no events).
func (s *SessionService) Inspect(ctx context.Context, id domain.SessionID) (domain.SessionInspection, error) {
	store, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return domain.SessionInspection{}, fmt.Errorf("session inspection is unavailable for this store")
	}
	inspection, err := store.InspectSession(ctx, id)
	if err != nil {
		return domain.SessionInspection{}, err
	}
	inspection.Events = nil
	return inspection, nil
}

// ReadArtifact returns the content of a committed artifact by reference.
// It is used by transport adapters to serve artifact bytes to clients
// (e.g. inline images generated by the generate_image tool).
func (s *SessionService) ReadArtifact(ctx context.Context, ref domain.ArtifactRef) ([]byte, error) {
	if s.proc.Artifact == nil {
		return nil, domain.NewError(domain.ErrUnavailable, "artifact store is not configured")
	}
	return s.proc.Artifact.Read(ctx, ref)
}

// --- share links (public read-only transcript views) ---

// SharedSessionView is the wire shape served to anyone holding a share
// link: the recovered transcript plus display metadata. It is deliberately
// a read-only store projection — no live handle is resumed, so viewing a
// share never spins up a runtime.
type SharedSessionView struct {
	SessionID domain.SessionID `json:"session_id"`
	Title     string           `json:"title"`
	CreatedAt time.Time        `json:"created_at"`
	UpdatedAt time.Time        `json:"updated_at"`
	Messages  []domain.Message `json:"messages"`
}

// shareStore narrows the persisted-store capabilities the share flow needs.
type shareStore interface {
	GetOrCreateShare(ctx context.Context, sessionID domain.SessionID) (string, error)
	ResolveShare(ctx context.Context, token string) (domain.SessionID, error)
	DeleteShare(ctx context.Context, sessionID domain.SessionID) error
	HasArtifactRef(ctx context.Context, sessionID domain.SessionID, artifactID domain.ArtifactID) (bool, error)
}

func (s *SessionService) shareStore() (shareStore, error) {
	store, ok := s.proc.Store.(shareStore)
	if !ok {
		return nil, fmt.Errorf("session sharing is unavailable for this store")
	}
	return store, nil
}

// ShareSession returns the session's public share token, creating the
// share on first use (idempotent).
func (s *SessionService) ShareSession(ctx context.Context, id domain.SessionID) (string, error) {
	store, err := s.shareStore()
	if err != nil {
		return "", err
	}
	return store.GetOrCreateShare(ctx, id)
}

// RevokeShare deletes the session's share link; existing links stop
// resolving immediately (idempotent).
func (s *SessionService) RevokeShare(ctx context.Context, id domain.SessionID) error {
	store, err := s.shareStore()
	if err != nil {
		return err
	}
	return store.DeleteShare(ctx, id)
}

// resolveShare maps a token to its session, normalizing the not-found
// case to ErrShareNotFound for transport mapping.
func (s *SessionService) resolveShare(ctx context.Context, store shareStore, token string) (domain.SessionID, error) {
	sessionID, err := store.ResolveShare(ctx, token)
	if err != nil {
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) && agentErr.Code == domain.ErrInvalidInput {
			return domain.SessionID{}, ErrShareNotFound
		}
		return domain.SessionID{}, err
	}
	return sessionID, nil
}

// SharedView returns the read-only transcript view for a share token.
func (s *SessionService) SharedView(ctx context.Context, token string) (SharedSessionView, error) {
	store, err := s.shareStore()
	if err != nil {
		return SharedSessionView{}, err
	}
	sessionID, err := s.resolveShare(ctx, store, token)
	if err != nil {
		return SharedSessionView{}, err
	}
	sqlite, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return SharedSessionView{}, fmt.Errorf("session sharing is unavailable for this store")
	}
	inspection, err := sqlite.InspectSession(ctx, sessionID)
	if err != nil {
		return SharedSessionView{}, err
	}
	view := SharedSessionView{
		SessionID: sessionID,
		CreatedAt: inspection.Session.CreatedAt,
		UpdatedAt: inspection.Session.UpdatedAt,
		Messages:  inspection.Transcript.Messages,
	}
	// Title enrichment mirrors the session listing: first user prompt,
	// best-effort (failure must not fail the view).
	if titles, err := sqlite.FirstUserMessageTexts(ctx, []domain.SessionID{sessionID}); err == nil {
		view.Title = summarizeSessionTitle(titles[sessionID])
	}
	return view, nil
}

// ReadSharedArtifact serves artifact bytes through a share link, but only
// for blobs the shared session's durable projections actually reference —
// the token must not become a read-anything capability for the
// content-addressed store.
func (s *SessionService) ReadSharedArtifact(ctx context.Context, token string, ref domain.ArtifactRef) ([]byte, error) {
	store, err := s.shareStore()
	if err != nil {
		return nil, err
	}
	sessionID, err := s.resolveShare(ctx, store, token)
	if err != nil {
		return nil, err
	}
	ok, err := store.HasArtifactRef(ctx, sessionID, ref.ID)
	if err != nil {
		return nil, err
	}
	if !ok {
		return nil, ErrSharedArtifactUnknown
	}
	return s.ReadArtifact(ctx, ref)
}

// Transcript returns one page of the session's canonical transcript
// projection (default limit 200, capped at 1000).
func (s *SessionService) Transcript(ctx context.Context, id domain.SessionID, after int64, limit int) (session.TranscriptPage, error) {
	store, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return session.TranscriptPage{}, fmt.Errorf("transcript is unavailable for this store")
	}
	if limit <= 0 {
		limit = 200
	}
	if limit > 1000 {
		limit = 1000
	}
	inspection, err := store.InspectSession(ctx, id)
	if err != nil {
		return session.TranscriptPage{}, err
	}
	transcript := session.Transcript{
		SessionID:         inspection.Transcript.SessionID,
		Messages:          inspection.Transcript.Messages,
		LastEventSequence: inspection.Transcript.LastEventSequence,
	}
	return transcript.Page(after, limit)
}

// SubscribeEvents returns a channel of the session's runtime events with
// Sequence > afterSeq, replaying the buffered tail first and then
// streaming live — the catch-up and the subscription are stitched
// atomically (docs/SERVE_DESIGN.md §4.5). afterSeq is compared against the
// GLOBAL broker sequence: a cursor beyond it comes from a previous process
// lifetime and is rejected with ErrCursorInvalid (the caller resyncs via
// Snapshot, then SubscribeLatest); a cursor between the session's own max
// and the global max simply means "up to date". The channel closes when
// ctx is cancelled, the service shuts down, or the subscription falls too
// far behind.
func (s *SessionService) SubscribeEvents(ctx context.Context, id domain.SessionID, afterSeq uint64) (<-chan runtimeevent.RuntimeEvent, error) {
	h, err := s.handle(id)
	if err != nil {
		return nil, err
	}
	if s.broker != nil && afterSeq > s.broker.Sequence() {
		return nil, ErrCursorInvalid
	}
	// Review M20: the closing check and the wg.Add must complete inside
	// the same s.mu critical section. Shutdown flips closing under s.mu
	// before it ever calls wg.Wait, so an Add that observed a live service
	// always happens-before Wait — an Add racing an in-flight Wait is
	// WaitGroup misuse ("WaitGroup is reused" panic), and a forward
	// goroutine started after Shutdown would have no lifecycle guarantee.
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return nil, ErrDraining
	}
	s.wg.Add(1)
	s.mu.Unlock()
	h.mu.Lock()
	replay, ok := h.Replay.Since(afterSeq)
	if !ok {
		h.mu.Unlock()
		s.wg.Done()
		return nil, ErrCursorInvalid
	}
	return s.subscribeLocked(ctx, h, replay), nil
}

// SubscribeLatest attaches a live-only subscription, skipping the replay
// tail entirely. It is the resync companion of SubscribeEvents: after
// ErrCursorInvalid the caller rebuilds its state from a fresh Snapshot
// (which is always complete — the projection is updated synchronously at
// persistence time, not via the event stream) and then needs only future
// events.
func (s *SessionService) SubscribeLatest(ctx context.Context, id domain.SessionID) (<-chan runtimeevent.RuntimeEvent, error) {
	h, err := s.handle(id)
	if err != nil {
		return nil, err
	}
	// Same M20 contract as SubscribeEvents: closing check and wg.Add under
	// one s.mu critical section.
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return nil, ErrDraining
	}
	s.wg.Add(1)
	s.mu.Unlock()
	h.mu.Lock()
	return s.subscribeLocked(ctx, h, nil), nil
}

// subscribeLocked registers a live subscription on h and returns the
// stitched output channel. Callers must hold h.mu AND must have already
// accounted the forward goroutine in s.wg (under s.mu, while checking the
// service is not closing — see SubscribeEvents, review M20). Replay events
// (nil for live-only) are delivered before live ones; ordering is exact
// because the pump appends to the ring and forwards to live queues under
// the same lock.
func (s *SessionService) subscribeLocked(ctx context.Context, h *SessionHandle, replay []runtimeevent.RuntimeEvent) <-chan runtimeevent.RuntimeEvent {
	subID := h.nextSubID
	h.nextSubID++
	live := make(chan runtimeevent.RuntimeEvent, s.subscriberQueue)
	h.subscribers[subID] = live
	h.mu.Unlock()
	h.touch()

	out := make(chan runtimeevent.RuntimeEvent, s.subscriberQueue)
	go func() {
		defer s.wg.Done()
		defer close(out)
		defer h.removeSubscriber(subID)
		for _, evt := range replay {
			select {
			case out <- evt:
			case <-ctx.Done():
				return
			case <-s.ctx.Done():
				return
			}
		}
		for {
			select {
			case evt, ok := <-live:
				if !ok {
					return
				}
				select {
				case out <- evt:
				case <-ctx.Done():
					return
				case <-s.ctx.Done():
					return
				}
			case <-ctx.Done():
				return
			case <-s.ctx.Done():
				return
			}
		}
	}()
	return out
}

// Shutdown stops accepting new sessions/prompts, disconnects all
// subscribers, and gracefully shuts every controller down.
func (s *SessionService) Shutdown(ctx context.Context) error {
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return nil
	}
	s.closing = true
	handles := make([]*SessionHandle, 0, len(s.sessions))
	for _, h := range s.sessions {
		handles = append(handles, h)
	}
	s.mu.Unlock()

	s.cancel()
	s.dropAllSubscribers()
	var firstErr error
	for _, h := range handles {
		if err := h.Controller.Shutdown(ctx); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	s.wg.Wait()
	return firstErr
}

func (s *SessionService) isClosing() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.closing
}

func (s *SessionService) activeTurns() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	active := 0
	for _, h := range s.sessions {
		switch h.Controller.State() {
		case ControllerStateRunning, ControllerStateAwaitingApproval, ControllerStateCancelling:
			active++
		}
	}
	return active
}

// pump is the service's single broker subscriber (§4.5): it appends every
// event to the owning session's replay log and fans out to live
// subscriptions, all non-blocking. A subscriber whose queue is full is
// disconnected — it resyncs via its cursor; the broker always sees a
// healthy subscriber.
func (s *SessionService) pump() {
	defer s.wg.Done()
	for {
		events, unsubscribe := s.broker.Subscribe()
		broken := false
		for !broken {
			select {
			case <-s.ctx.Done():
				unsubscribe()
				return
			case evt, ok := <-events:
				if !ok {
					broken = true
					break
				}
				s.dispatch(evt)
			}
		}
		select {
		case <-s.ctx.Done():
			return
		default:
		}
		// The broker disconnected the pump (extreme backpressure) — by
		// design this should never happen; alert loudly, poison every
		// pre-gap cursor (the gap's events never reached any ring, so
		// serving them would be silent loss), drop all live streams so
		// clients resync via snapshot, and resubscribe.
		s.logger.Error("event pump lost its broker subscription; clients must resync")
		s.invalidateAll()
		s.dropAllSubscribers()
		select {
		case <-time.After(time.Second):
		case <-s.ctx.Done():
			return
		}
	}
}

func (s *SessionService) dispatch(evt runtimeevent.RuntimeEvent) {
	s.mu.Lock()
	h, ok := s.sessions[evt.SessionID]
	s.mu.Unlock()
	if !ok {
		return
	}
	h.mu.Lock()
	h.Replay.Append(evt)
	var slow []chan runtimeevent.RuntimeEvent
	for id, ch := range h.subscribers {
		select {
		case ch <- evt:
		default:
			slow = append(slow, ch)
			delete(h.subscribers, id)
		}
	}
	h.mu.Unlock()
	if len(slow) > 0 {
		// 慢消费者断连是断流重连循环的 server 侧起点，此前完全无日志。
		s.logger.Warn("dropping slow event subscriber; client must resync",
			"session_id", evt.SessionID, "dropped", len(slow), "queue", s.subscriberQueue)
	}
	for _, ch := range slow {
		close(ch)
	}
}

func (s *SessionService) dropAllSubscribers() {
	s.mu.Lock()
	handles := make([]*SessionHandle, 0, len(s.sessions))
	for _, h := range s.sessions {
		handles = append(handles, h)
	}
	s.mu.Unlock()
	for _, h := range handles {
		h.dropSubscribers()
	}
}

// invalidateAll poisons every handle's replay cursors below the current
// global broker sequence (pump resync path): subscribers holding pre-gap
// cursors get ErrCursorInvalid and resync via snapshot instead of silently
// missing the gap's events.
func (s *SessionService) invalidateAll() {
	if s.broker == nil {
		return
	}
	floor := s.broker.Sequence()
	s.mu.Lock()
	handles := make([]*SessionHandle, 0, len(s.sessions))
	for _, h := range s.sessions {
		handles = append(handles, h)
	}
	s.mu.Unlock()
	for _, h := range handles {
		h.Replay.Invalidate(floor)
	}
}

// sweeper reclaims handles that have been idle longer than the TTL; a
// handle with a non-idle controller is never reclaimed.
func (s *SessionService) sweeper() {
	defer s.wg.Done()
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-ticker.C:
			s.sweepIdle()
		}
	}
}

func (s *SessionService) sweepIdle() {
	cutoff := time.Now().Add(-s.idleTTL).UnixNano()
	var victims []*SessionHandle
	s.mu.Lock()
	for id, h := range s.sessions {
		if h.Controller.State() == ControllerStateIdle && h.lastActiveNanos.Load() < cutoff {
			delete(s.sessions, id)
			victims = append(victims, h)
		}
	}
	s.mu.Unlock()
	for _, h := range victims {
		// Close live subscriptions first: clients must see the stream end
		// (and resync) rather than a silently dead session (review M4).
		h.dropSubscribers()
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		if err := h.Controller.Shutdown(ctx); err != nil {
			s.logger.Warn("idle session shutdown failed", "session_id", h.ID, "error", err)
		}
		cancel()
		s.logger.Info("idle session reclaimed", "session_id", h.ID)
	}
}
