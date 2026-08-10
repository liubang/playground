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

// Package client defines the protocol-agnostic interface between loom
// frontends and the agent runtime (docs/SERVE_DESIGN.md §10, §17.5).
// Every frontend — the TUI, the web SPA, curl scripts, future IDE plugins —
// is an equal peer client of this interface. Two implementations exist:
// inproc (zero-serialization, wraps app.SessionService) and, from M2 on,
// http (REST+SSE wire protocol). Interface hard constraints:
//
//  1. Every request/response/event type is JSON-serializable (guarded by
//     roundtrip tests);
//  2. inproc never hands out caller-mutable shared references (copy
//     semantics), so inproc and http behave identically;
//  3. errors are typed sentinel errors from the application layer,
//     transport adapters map them to their own model;
//  4. event subscription (replay + live stitching) lives in the
//     application layer — transports only consume SubscribeEvents.
package client

import (
	"context"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Type aliases: the application layer owns the canonical shapes; the client
// package re-exports them so frontends can depend on a single package
// without losing type identity with app internals (docs/SERVE_DESIGN.md §10).
type (
	Snapshot                = app.Snapshot
	PendingRequest          = app.PendingRequest
	PendingRequestKind      = app.PendingRequestKind
	SubmitResult            = app.SubmitResult
	ApprovalBinding         = app.ApprovalBinding
	ApprovalRuleHint        = app.ApprovalRuleHint
	AnswerQuestionResult    = app.AnswerQuestionResult
	ControllerState         = app.ControllerState
	SessionSummary          = app.SessionSummary
	SubagentView            = app.SubagentView
	SkillsListing           = app.SkillsListing
	SkillInfo               = app.SkillInfo
	MCPServerInfo           = app.MCPServerInfo
	SetModelResult          = app.SetModelResult
	SetReasoningResult      = app.SetReasoningResult
	RequestCompactionResult = app.RequestCompactionResult
	CheckpointInfo          = app.CheckpointInfo
	RewindOutcome           = app.RewindOutcome
	ToolchainReport         = app.ToolchainReport
)

// Controller state constants re-exported for frontend state machines.
const (
	ControllerStateBooting          = app.ControllerStateBooting
	ControllerStateIdle             = app.ControllerStateIdle
	ControllerStateRunning          = app.ControllerStateRunning
	ControllerStateAwaitingApproval = app.ControllerStateAwaitingApproval
	ControllerStateCancelling       = app.ControllerStateCancelling
	ControllerStateFatal            = app.ControllerStateFatal
	ControllerStateClosed           = app.ControllerStateClosed
)

// Pending request kind constants.
const (
	PendingRequestApproval = app.PendingRequestApproval
	PendingRequestQuestion = app.PendingRequestQuestion
)

// Client is the session-scoped contract between a frontend and the loom
// runtime. One client is bound to at most one session at a time;
// NewSession/ResumeSession (re)bind it. Methods mirror the application
// layer API (app.SessionService + app.Controller) and map mechanically onto
// the wire protocol (docs/SERVE_DESIGN.md §5.3).
type Client interface {
	// --- session lifecycle ---

	// NewSession starts a fresh session and binds the client to it. The
	// session is created in the process's default workspace.
	NewSession(ctx context.Context) error
	// NewSessionIn starts a fresh session in the given workspace (a zero
	// workspaceID selects the default workspace) and binds the client to it.
	NewSessionIn(ctx context.Context, workspaceID domain.WorkspaceID) error
	// ResumeSession binds the client to an existing persisted session.
	ResumeSession(ctx context.Context, id domain.SessionID) error
	// SessionID returns the bound session (zero before New/Resume).
	SessionID() domain.SessionID
	// State returns the bound session's runtime state.
	State() ControllerState
	// Done closes when the bound session's runtime stops.
	Done() <-chan struct{}

	// --- turn control ---

	// SubmitPrompt sends a user prompt. While a turn is busy the prompt is
	// queued for steering (TUI semantics, docs/SERVE_DESIGN.md §16.3 D1).
	SubmitPrompt(ctx context.Context, prompt string, images []domain.ImageContent) (SubmitResult, error)
	// CancelTurn cancels the active turn.
	CancelTurn(ctx context.Context) error

	// --- approvals & questions ---

	// ResolveApproval resolves a pending approval; the binding must match
	// the canonical prepared call (one-shot CAS).
	ResolveApproval(ctx context.Context, binding ApprovalBinding, decision domain.Decision, hint *ApprovalRuleHint) (string, error)
	// AnswerQuestion resolves a pending ask_user question (one-shot).
	AnswerQuestion(ctx context.Context, id domain.EventID, answer domain.QuestionAnswer) (AnswerQuestionResult, error)

	// --- state & events ---

	// RequestSnapshot returns the live projection, including the event
	// watermark (Snapshot.EventSeq) for a gapless snapshot+delta handoff.
	RequestSnapshot(ctx context.Context) (Snapshot, error)
	// SubscribeEvents streams the bound session's runtime events with
	// Sequence > afterSeq, replaying the buffered tail first (atomically
	// stitched with the live stream). Pass 0 at process start; pass
	// Snapshot.EventSeq on reattach. The channel closes on ctx
	// cancellation, service shutdown, or when the subscription falls too
	// far behind.
	SubscribeEvents(ctx context.Context, afterSeq uint64) (<-chan runtimeevent.RuntimeEvent, error)

	// --- session configuration ---

	// SetModel switches the session's model from the next turn on.
	SetModel(ctx context.Context, ref string) (SetModelResult, error)
	// SetReasoning sets the session-scoped reasoning dial.
	SetReasoning(ctx context.Context, arg string) (SetReasoningResult, error)
	// RequestCompaction schedules a forced compaction for the next turn.
	RequestCompaction(ctx context.Context) (RequestCompactionResult, error)

	// --- history ---

	// ListSessions returns recent persisted sessions of the process's default
	// workspace (the launch directory) — the single-workspace frontend view
	// (TUI/CLI picker).
	ListSessions(ctx context.Context, limit int) ([]SessionSummary, error)
	// ListSessionsIn returns recent persisted sessions of the given workspace;
	// a zero workspaceID lists across all workspaces (the multi-workspace
	// tree view).
	ListSessionsIn(ctx context.Context, limit int, workspaceID domain.WorkspaceID) ([]SessionSummary, error)
	// ListCheckpoints returns the bound session's checkpoints.
	ListCheckpoints(ctx context.Context, limit int) ([]CheckpointInfo, error)
	// Rewind rolls the bound session back to a checkpoint.
	Rewind(ctx context.Context, checkpointSequence int64) (RewindOutcome, error)
	// SubagentView returns a read-only drill-in view of a delegate_task
	// child run.
	SubagentView(ctx context.Context, sessionID domain.SessionID) (SubagentView, error)

	// --- environment ---

	// ListSkills returns the discovered skills.
	ListSkills(ctx context.Context) (SkillsListing, error)
	// ListMCPServers returns the configured MCP servers and their status.
	ListMCPServers(ctx context.Context) ([]MCPServerInfo, error)
	// ToolchainEnvironment returns the PATH-augmentation report behind the
	// /doctor listing and the settings environment card.
	ToolchainEnvironment(ctx context.Context) (*ToolchainReport, error)
	// ListRules returns the effective approval ruleset.
	ListRules(ctx context.Context) (*permission.RuleSet, error)
	// ForgetRule removes a remembered approval rule. Exactly one of
	// prefix/host/tool is consulted, selected by kind.
	ForgetRule(ctx context.Context, kind permission.RuleKind, prefix []string, host, tool string) error

	// --- workspaces ---

	// ListWorkspaces returns every registered workspace, newest first.
	ListWorkspaces(ctx context.Context) ([]domain.Workspace, error)
	// RegisterWorkspace registers (or reuses by canonical root) a workspace.
	RegisterWorkspace(ctx context.Context, root, name string) (domain.Workspace, error)
	// DeleteWorkspace removes a workspace entity (metadata only — the
	// on-disk root directory is never touched). Its sessions survive as
	// read-only history. The default workspace and workspaces with live
	// sessions cannot be deleted.
	DeleteWorkspace(ctx context.Context, id domain.WorkspaceID) error
}
