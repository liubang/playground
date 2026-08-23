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

package client

import (
	"context"
	"errors"
	"fmt"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// inprocClient is the zero-serialization Client implementation: it
// delegates to app.SessionService in the same process. All returned values
// are freshly projected by the controller (messages and pending requests
// are copied at snapshot time), so callers never observe shared mutable
// state — the same contract the http implementation enforces by
// construction (docs/SERVE_DESIGN.md §17.5).
type inprocClient struct {
	service *app.SessionService

	mu     sync.Mutex
	handle *app.SessionHandle
}

// NewInProc returns a Client that talks to the given SessionService
// in-process (the default for `loom chat`).
func NewInProc(service *app.SessionService) Client {
	return &inprocClient{service: service}
}

func (c *inprocClient) bound() (*app.SessionHandle, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.handle == nil {
		return nil, app.ErrSessionNotFound
	}
	return c.handle, nil
}

func (c *inprocClient) bind(h *app.SessionHandle) {
	c.mu.Lock()
	c.handle = h
	c.mu.Unlock()
}

func (c *inprocClient) NewSession(ctx context.Context) error {
	return c.NewSessionIn(ctx, domain.WorkspaceID{})
}

// NewSessionIn starts a fresh session in the given workspace (zero = the
// process's default workspace) and binds the client to it.
func (c *inprocClient) NewSessionIn(ctx context.Context, workspaceID domain.WorkspaceID) error {
	h, err := c.service.CreateSession(ctx, workspaceID)
	if err != nil {
		return err
	}
	c.bind(h)
	return nil
}

func (c *inprocClient) ResumeSession(ctx context.Context, id domain.SessionID) error {
	h, err := c.service.ResumeSession(ctx, id)
	if err != nil {
		return err
	}
	// Resuming is an explicit intent to continue the conversation, so an
	// archived (read-only) session is restored to active. Read-only
	// viewing goes through the snapshot/transcript APIs, never Resume.
	if _, err := c.service.SetSessionArchived(ctx, id, false); err != nil {
		return fmt.Errorf("unarchive session: %w", err)
	}
	c.bind(h)
	return nil
}

func (c *inprocClient) SessionID() domain.SessionID {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.handle == nil {
		return domain.SessionID{}
	}
	return c.handle.ID
}

func (c *inprocClient) State() ControllerState {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.handle == nil {
		return ControllerStateBooting
	}
	return c.handle.Controller.State()
}

func (c *inprocClient) Done() <-chan struct{} {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.handle == nil {
		// No bound session: nothing that can ever finish.
		return make(chan struct{})
	}
	return c.handle.Controller.Done()
}

func (c *inprocClient) SubmitPrompt(ctx context.Context, prompt string, images []domain.ImageContent) (SubmitResult, error) {
	h, err := c.bound()
	if err != nil {
		return SubmitResult{}, err
	}
	result, _, err := c.service.SubmitPrompt(ctx, h.ID, prompt, images, "", false)
	return result, err
}

func (c *inprocClient) SubmitFollowup(ctx context.Context, prompt string) (SubmitResult, error) {
	h, err := c.bound()
	if err != nil {
		return SubmitResult{}, err
	}
	result, _, err := c.service.SubmitPrompt(ctx, h.ID, prompt, nil, "", true)
	return result, err
}

func (c *inprocClient) CancelTurn(ctx context.Context) error {
	h, err := c.bound()
	if err != nil {
		return err
	}
	return c.service.CancelTurn(ctx, h.ID)
}

func (c *inprocClient) ResolveApproval(ctx context.Context, binding ApprovalBinding, decision domain.Decision, hint *ApprovalRuleHint) (string, error) {
	h, err := c.bound()
	if err != nil {
		return "", err
	}
	return c.service.ResolveApproval(ctx, h.ID, binding, decision, hint, "")
}

func (c *inprocClient) AnswerQuestion(ctx context.Context, id domain.EventID, answer domain.QuestionAnswer) (AnswerQuestionResult, error) {
	h, err := c.bound()
	if err != nil {
		return AnswerQuestionResult{}, err
	}
	return c.service.AnswerQuestion(ctx, h.ID, id, answer)
}

func (c *inprocClient) RequestSnapshot(ctx context.Context) (Snapshot, error) {
	h, err := c.bound()
	if err != nil {
		return Snapshot{}, err
	}
	return c.service.Snapshot(ctx, h.ID)
}

func (c *inprocClient) SubscribeEvents(ctx context.Context, afterSeq uint64) (<-chan runtimeevent.RuntimeEvent, error) {
	h, err := c.bound()
	if err != nil {
		return nil, err
	}
	ch, err := c.service.SubscribeEvents(ctx, h.ID, afterSeq)
	if errors.Is(err, app.ErrCursorInvalid) {
		// The cursor can no longer be honored (rotation, or a pump resync
		// gap). In-proc callers always pair re-subscription with a snapshot
		// refresh — and snapshots are complete by construction (the
		// projection updates synchronously at persistence time) — so a
		// live-only re-attach loses nothing durable.
		return c.service.SubscribeLatest(ctx, h.ID)
	}
	return ch, err
}

func (c *inprocClient) SetModel(ctx context.Context, ref string) (SetModelResult, error) {
	h, err := c.bound()
	if err != nil {
		return SetModelResult{}, err
	}
	return h.Controller.SetModel(ctx, ref)
}

func (c *inprocClient) SetReasoning(ctx context.Context, arg string) (SetReasoningResult, error) {
	h, err := c.bound()
	if err != nil {
		return SetReasoningResult{}, err
	}
	return h.Controller.SetReasoning(ctx, arg)
}

func (c *inprocClient) RequestCompaction(ctx context.Context) (RequestCompactionResult, error) {
	h, err := c.bound()
	if err != nil {
		return RequestCompactionResult{}, err
	}
	return h.Controller.RequestCompaction(ctx)
}

func (c *inprocClient) ListSessions(ctx context.Context, limit int) ([]SessionSummary, error) {
	// The TUI is a single-workspace frontend bound to the default workspace
	// (the launch directory): its picker lists only that workspace's sessions
	// (docs/WORKSPACE_DESIGN.md W5), not every workspace's.
	return c.ListSessionsIn(ctx, limit, c.service.DefaultWorkspaceID())
}

// ListSessionsIn returns recent persisted sessions of the given workspace
// (zero = across all workspaces).
func (c *inprocClient) ListSessionsIn(ctx context.Context, limit int, workspaceID domain.WorkspaceID) ([]SessionSummary, error) {
	// The client contract stays cursor-less: the first page is what
	// pickers display; HTTP consumers paginate via the cursor query param.
	summaries, _, err := c.service.ListSessions(ctx, "", limit, false, workspaceID)
	return summaries, err
}

func (c *inprocClient) ListWorkspaces(ctx context.Context) ([]domain.Workspace, error) {
	return c.service.ListWorkspaces(ctx)
}

func (c *inprocClient) RegisterWorkspace(ctx context.Context, root, name string) (domain.Workspace, error) {
	return c.service.RegisterWorkspace(ctx, root, name)
}

func (c *inprocClient) DeleteWorkspace(ctx context.Context, id domain.WorkspaceID) error {
	return c.service.DeleteWorkspace(ctx, id)
}

func (c *inprocClient) ListCheckpoints(ctx context.Context, limit int) ([]CheckpointInfo, error) {
	h, err := c.bound()
	if err != nil {
		return nil, err
	}
	return h.Controller.ListCheckpoints(ctx, limit)
}

func (c *inprocClient) Rewind(ctx context.Context, checkpointSequence int64) (RewindOutcome, error) {
	h, err := c.bound()
	if err != nil {
		return RewindOutcome{}, err
	}
	return h.Controller.Rewind(ctx, checkpointSequence)
}

func (c *inprocClient) SubagentView(ctx context.Context, sessionID domain.SessionID) (SubagentView, error) {
	h, err := c.bound()
	if err != nil {
		return SubagentView{}, err
	}
	return h.Controller.SubagentView(ctx, sessionID)
}

func (c *inprocClient) ListSkills(ctx context.Context) (SkillsListing, error) {
	h, err := c.bound()
	if err != nil {
		return SkillsListing{}, err
	}
	return h.Controller.ListSkills(ctx)
}

func (c *inprocClient) ListMCPServers(ctx context.Context) ([]MCPServerInfo, error) {
	h, err := c.bound()
	if err != nil {
		return nil, err
	}
	return h.Controller.ListMCPServers(ctx)
}

func (c *inprocClient) ToolchainEnvironment(ctx context.Context) (*ToolchainReport, error) {
	h, err := c.bound()
	if err != nil {
		return nil, err
	}
	return h.Controller.ToolchainEnvironment(ctx)
}

func (c *inprocClient) ListPackages(ctx context.Context) ([]permission.Package, error) {
	h, err := c.bound()
	if err != nil {
		return nil, err
	}
	return h.Controller.ListPackages(ctx)
}

func (c *inprocClient) ForgetPackage(ctx context.Context, bind permission.Binding) error {
	h, err := c.bound()
	if err != nil {
		return err
	}
	return h.Controller.ForgetPackage(ctx, bind)
}
