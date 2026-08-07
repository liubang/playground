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
// Created: 2026/08/04

package client

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// ErrUnsupported reports a Client method the wire protocol does not cover
// yet (checkpoint/rewind/subagent/skills/MCP/rules endpoints land with the
// web SPA milestone, docs/SERVE_DESIGN.md §16.3 D2).
var ErrUnsupported = errors.New("operation is not supported by the http transport yet")

// httpClient implements Client over the REST+SSE wire protocol
// (docs/SERVE_DESIGN.md §5). It is one of the two reference
// implementations; the contract test suite runs against both this and the
// inproc one.
type httpClient struct {
	base   string
	token  string
	client *http.Client

	mu        sync.Mutex
	sessionID domain.SessionID

	done     chan struct{}
	doneOnce sync.Once
}

// NewHTTP returns a Client talking to a loom serve endpoint
// (e.g. "http://127.0.0.1:7680") with the given bearer token.
func NewHTTP(baseURL, token string) Client {
	return &httpClient{
		base:   strings.TrimRight(baseURL, "/"),
		token:  token,
		client: &http.Client{},
		done:   make(chan struct{}),
	}
}

// Close releases the client: Done() closes and in-flight streams unwind.
func (c *httpClient) Close() { c.doneOnce.Do(func() { close(c.done) }) }

// --- wire helpers ---

type wireError struct {
	Error struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func (c *httpClient) newRequest(ctx context.Context, method, path string, body any) (*http.Request, error) {
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return nil, err
		}
		reader = bytes.NewReader(data)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.base+path, reader)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+c.token)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	return req, nil
}

// do executes a JSON request and decodes the response, mapping the unified
// wire error model back into application-layer errors.
func (c *httpClient) do(ctx context.Context, method, path string, body, out any) error {
	req, err := c.newRequest(ctx, method, path, body)
	if err != nil {
		return err
	}
	resp, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		var we wireError
		if err := json.NewDecoder(resp.Body).Decode(&we); err != nil {
			return fmt.Errorf("%s %s: status %d", method, path, resp.StatusCode)
		}
		return mapWireError(resp.StatusCode, we.Error.Code, we.Error.Message)
	}
	if out != nil {
		if err := json.NewDecoder(resp.Body).Decode(out); err != nil {
			return fmt.Errorf("decode %s %s response: %w", method, path, err)
		}
	}
	return nil
}

// mapWireError converts wire error codes back to application-layer typed
// errors where they exist.
func mapWireError(status int, code, message string) error {
	switch code {
	case "not_found":
		return app.ErrSessionNotFound
	case "workspace_not_found":
		return app.ErrWorkspaceNotFound
	case "workspace_unavailable":
		return app.ErrWorkspaceUnavailable
	case "workspace_in_use":
		return app.ErrWorkspaceInUse
	case "draining":
		return app.ErrDraining
	case "cursor_invalid":
		return app.ErrCursorInvalid
	case "rate_limited":
		return app.ErrTooManyTurns
	}
	return fmt.Errorf("%s: %s", code, message)
}

// --- session lifecycle ---

func (c *httpClient) NewSession(ctx context.Context) error {
	return c.NewSessionIn(ctx, domain.WorkspaceID{})
}

func (c *httpClient) NewSessionIn(ctx context.Context, workspaceID domain.WorkspaceID) error {
	var out struct {
		SessionID domain.SessionID `json:"session_id"`
	}
	body := map[string]any{}
	if !workspaceID.IsZero() {
		body["workspace_id"] = workspaceID.String()
	}
	if err := c.do(ctx, http.MethodPost, "/v1/sessions", body, &out); err != nil {
		return err
	}
	c.mu.Lock()
	c.sessionID = out.SessionID
	c.mu.Unlock()
	return nil
}

func (c *httpClient) ResumeSession(ctx context.Context, id domain.SessionID) error {
	var out struct {
		SessionID domain.SessionID `json:"session_id"`
	}
	if err := c.do(ctx, http.MethodPost, "/v1/sessions", map[string]string{"resume": id.String()}, &out); err != nil {
		return err
	}
	c.mu.Lock()
	c.sessionID = out.SessionID
	c.mu.Unlock()
	return nil
}

func (c *httpClient) SessionID() domain.SessionID {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sessionID
}

func (c *httpClient) sessionPath() (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sessionID.IsZero() {
		return "", app.ErrSessionNotFound
	}
	return "/v1/sessions/" + c.sessionID.String(), nil
}

func (c *httpClient) State() ControllerState {
	path, err := c.sessionPath()
	if err != nil {
		return ControllerStateBooting
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var snap Snapshot
	if err := c.do(ctx, http.MethodGet, path+"/snapshot", nil, &snap); err != nil {
		return ControllerStateBooting
	}
	return snap.State
}

func (c *httpClient) Done() <-chan struct{} { return c.done }

// --- turn control ---

func (c *httpClient) SubmitPrompt(ctx context.Context, prompt string, images []domain.ImageContent) (SubmitResult, error) {
	path, err := c.sessionPath()
	if err != nil {
		return SubmitResult{}, err
	}
	var out struct {
		Turn     int  `json:"turn"`
		Steered  bool `json:"steered"`
		QueueLen int  `json:"queue_len"`
	}
	if err := c.do(ctx, http.MethodPost, path+"/prompts", map[string]any{"prompt": prompt, "images": images}, &out); err != nil {
		return SubmitResult{}, err
	}
	return SubmitResult{Steered: out.Steered, QueueLen: out.QueueLen, Turn: out.Turn}, nil
}

func (c *httpClient) CancelTurn(ctx context.Context) error {
	path, err := c.sessionPath()
	if err != nil {
		return err
	}
	return c.do(ctx, http.MethodPost, path+"/cancel", struct{}{}, nil)
}

// --- approvals & questions ---

func (c *httpClient) ResolveApproval(ctx context.Context, binding ApprovalBinding, decision domain.Decision, hint *ApprovalRuleHint) (string, error) {
	path, err := c.sessionPath()
	if err != nil {
		return "", err
	}
	body := map[string]any{
		"call_id":   binding.CallID.String(),
		"args_hash": binding.ArgsHash,
		"decision":  string(decision),
	}
	if hint != nil {
		body["rule_hint"] = map[string]any{"tool_name": hint.ToolName, "arguments": hint.Arguments, "trust": hint.Trust}
	}
	var out struct {
		Note string `json:"note"`
	}
	if err := c.do(ctx, http.MethodPost, path+"/approvals/"+binding.ApprovalID.String(), body, &out); err != nil {
		return "", err
	}
	return out.Note, nil
}

func (c *httpClient) AnswerQuestion(ctx context.Context, id domain.EventID, answer domain.QuestionAnswer) (AnswerQuestionResult, error) {
	path, err := c.sessionPath()
	if err != nil {
		return AnswerQuestionResult{}, err
	}
	if err := c.do(ctx, http.MethodPost, path+"/questions/"+id.String(), answer, nil); err != nil {
		// The wire reports an unknown/already-resolved question as a 409
		// binding_mismatch — the one-shot semantic maps to Resolved=false.
		if strings.Contains(err.Error(), "binding_mismatch") {
			return AnswerQuestionResult{Resolved: false}, nil
		}
		return AnswerQuestionResult{}, err
	}
	return AnswerQuestionResult{Resolved: true}, nil
}

// --- state & events ---

func (c *httpClient) RequestSnapshot(ctx context.Context) (Snapshot, error) {
	path, err := c.sessionPath()
	if err != nil {
		return Snapshot{}, err
	}
	var snap Snapshot
	if err := c.do(ctx, http.MethodGet, path+"/snapshot", nil, &snap); err != nil {
		return Snapshot{}, err
	}
	return snap, nil
}

func (c *httpClient) SubscribeEvents(ctx context.Context, afterSeq uint64) (<-chan runtimeevent.RuntimeEvent, error) {
	path, err := c.sessionPath()
	if err != nil {
		return nil, err
	}
	req, err := c.newRequest(ctx, http.MethodGet, fmt.Sprintf("%s/events?after=%d", path, afterSeq), nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 400 {
		defer resp.Body.Close()
		var we wireError
		if err := json.NewDecoder(resp.Body).Decode(&we); err != nil {
			return nil, fmt.Errorf("GET events: status %d", resp.StatusCode)
		}
		return nil, mapWireError(resp.StatusCode, we.Error.Code, we.Error.Message)
	}
	out := make(chan runtimeevent.RuntimeEvent, 64)
	go pumpSSE(ctx, resp.Body, out)
	return out, nil
}

// pumpSSE parses an SSE body into runtime events. A server.resync frame
// closes the stream (callers resync via snapshot and re-subscribe, the
// same contract as the inproc fallback); server.draining does the same
// with the process going away. Writes select on ctx so a stalled consumer
// cannot wedge the goroutine past cancellation.
func pumpSSE(ctx context.Context, body io.ReadCloser, out chan<- runtimeevent.RuntimeEvent) {
	defer body.Close()
	defer close(out)
	var id string
	var event, data string
	scanner := bufio.NewScanner(body)
	scanner.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	flush := func() bool {
		defer func() { id, event, data = "", "", "" }()
		if data == "" {
			return true
		}
		if event == "server.resync" || event == "server.draining" {
			return true
		}
		var evt runtimeevent.RuntimeEvent
		if err := json.Unmarshal([]byte(data), &evt); err != nil {
			return true
		}
		select {
		case out <- evt:
			return true
		case <-ctx.Done():
			return false
		}
	}
	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case line == "":
			if !flush() {
				return
			}
		case strings.HasPrefix(line, ":"):
			// comment (connected banner / heartbeat)
		case strings.HasPrefix(line, "id: "):
			id = strings.TrimPrefix(line, "id: ")
		case strings.HasPrefix(line, "event: "):
			event = strings.TrimPrefix(line, "event: ")
		case strings.HasPrefix(line, "data: "):
			if data != "" {
				data += "\n"
			}
			data += strings.TrimPrefix(line, "data: ")
		}
		_ = id
	}
}

// --- session configuration ---

func (c *httpClient) SetModel(ctx context.Context, ref string) (SetModelResult, error) {
	path, err := c.sessionPath()
	if err != nil {
		return SetModelResult{}, err
	}
	provider, model, _ := strings.Cut(ref, "/")
	var out SetModelResult
	if err := c.do(ctx, http.MethodPost, path+"/model", map[string]string{"provider": provider, "model": model}, &out); err != nil {
		return SetModelResult{}, err
	}
	return out, nil
}

func (c *httpClient) SetReasoning(ctx context.Context, arg string) (SetReasoningResult, error) {
	path, err := c.sessionPath()
	if err != nil {
		return SetReasoningResult{}, err
	}
	var out SetReasoningResult
	if err := c.do(ctx, http.MethodPost, path+"/reasoning", map[string]string{"effort": arg}, &out); err != nil {
		return SetReasoningResult{}, err
	}
	return out, nil
}

func (c *httpClient) RequestCompaction(ctx context.Context) (RequestCompactionResult, error) {
	path, err := c.sessionPath()
	if err != nil {
		return RequestCompactionResult{}, err
	}
	var out RequestCompactionResult
	if err := c.do(ctx, http.MethodPost, path+"/compact", struct{}{}, &out); err != nil {
		return RequestCompactionResult{}, err
	}
	return out, nil
}

// --- history ---

func (c *httpClient) ListSessions(ctx context.Context, limit int) ([]SessionSummary, error) {
	// The bare /v1/sessions endpoint defaults to the process's default
	// workspace — matching the inproc client (single-workspace frontend view).
	var out struct {
		Sessions []SessionSummary `json:"sessions"`
	}
	if err := c.do(ctx, http.MethodGet, fmt.Sprintf("/v1/sessions?limit=%d", limit), nil, &out); err != nil {
		return nil, err
	}
	return out.Sessions, nil
}

func (c *httpClient) ListSessionsIn(ctx context.Context, limit int, workspaceID domain.WorkspaceID) ([]SessionSummary, error) {
	var out struct {
		Sessions []SessionSummary `json:"sessions"`
	}
	path := fmt.Sprintf("/v1/sessions?limit=%d", limit)
	if workspaceID.IsZero() {
		path += "&workspace_id=all" // 零值 = 全部 workspace（树形视图）
	} else {
		path += "&workspace_id=" + workspaceID.String()
	}
	if err := c.do(ctx, http.MethodGet, path, nil, &out); err != nil {
		return nil, err
	}
	return out.Sessions, nil
}

func (c *httpClient) ListWorkspaces(ctx context.Context) ([]domain.Workspace, error) {
	var out struct {
		Workspaces []domain.Workspace `json:"workspaces"`
	}
	if err := c.do(ctx, http.MethodGet, "/v1/workspaces", nil, &out); err != nil {
		return nil, err
	}
	return out.Workspaces, nil
}

func (c *httpClient) RegisterWorkspace(ctx context.Context, root, name string) (domain.Workspace, error) {
	var out struct {
		Workspace domain.Workspace `json:"workspace"`
	}
	if err := c.do(ctx, http.MethodPost, "/v1/workspaces", map[string]string{"root_path": root, "name": name}, &out); err != nil {
		return domain.Workspace{}, err
	}
	return out.Workspace, nil
}

func (c *httpClient) DeleteWorkspace(ctx context.Context, id domain.WorkspaceID) error {
	return c.do(ctx, http.MethodDelete, "/v1/workspaces/"+id.String(), nil, nil)
}

func (c *httpClient) ListCheckpoints(ctx context.Context, limit int) ([]CheckpointInfo, error) {
	return nil, ErrUnsupported
}

func (c *httpClient) Rewind(ctx context.Context, checkpointSequence int64) (RewindOutcome, error) {
	return RewindOutcome{}, ErrUnsupported
}

func (c *httpClient) SubagentView(ctx context.Context, sessionID domain.SessionID) (SubagentView, error) {
	return SubagentView{}, ErrUnsupported
}

// --- environment ---

func (c *httpClient) ListSkills(ctx context.Context) (SkillsListing, error) {
	return SkillsListing{}, ErrUnsupported
}

func (c *httpClient) ListMCPServers(ctx context.Context) ([]MCPServerInfo, error) {
	return nil, ErrUnsupported
}

func (c *httpClient) ListRules(ctx context.Context) (*permission.RuleSet, error) {
	return nil, ErrUnsupported
}

func (c *httpClient) ForgetRule(ctx context.Context, kind permission.RuleKind, prefix []string, host, tool string) error {
	return ErrUnsupported
}
