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

package server

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strconv"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// decodeBody decodes a JSON request body with the global size cap.
func decodeBody(w http.ResponseWriter, r *http.Request, v any) error {
	return decodeBodyLimit(w, r, v, maxBodyBytes)
}

// decodeBodyLimit decodes a JSON request body with an explicit size cap,
// used by routes that accept larger payloads (e.g. prompts with inline
// base64 images, whose 4/3 expansion dwarfs the global cap).
func decodeBodyLimit(w http.ResponseWriter, r *http.Request, v any, limit int64) error {
	r.Body = http.MaxBytesReader(w, r.Body, limit)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(v); err != nil {
		return invalidInput("invalid request body: " + err.Error())
	}
	return nil
}

// auditf writes one audit record.
func (s *Server) auditf(action string, sessionID domain.SessionID, kv ...any) {
	args := append([]any{"action", action, "session_id", sessionID.String()}, kv...)
	s.audit.Info("audit", args...)
}

// --- session lifecycle ---

func (s *Server) handleListSessions(w http.ResponseWriter, r *http.Request) {
	limit := 100
	if raw := r.URL.Query().Get("limit"); raw != "" {
		n, err := strconv.Atoi(raw)
		if err != nil || n <= 0 {
			writeError(w, invalidInput("limit must be a positive integer"))
			return
		}
		limit = n
	}
	cursor := r.URL.Query().Get("cursor")
	archived := r.URL.Query().Get("archived") == "1"
	// workspace_id 三态（docs/WORKSPACE_DESIGN.md §8.1）：缺省 = default
	// workspace（单 workspace 前端，如 TUI 的 picker）；"all" = 全部（多
	// workspace 树形）；<id> = 指定 workspace。
	var wsID domain.WorkspaceID
	switch raw := r.URL.Query().Get("workspace_id"); raw {
	case "":
		wsID = s.svc.DefaultWorkspaceID()
	case "all":
		wsID = domain.WorkspaceID{}
	default:
		var err error
		wsID, err = parseWorkspaceIDParam(raw)
		if err != nil {
			writeError(w, err)
			return
		}
	}
	summaries, nextCursor, err := s.svc.ListSessions(r.Context(), cursor, limit, archived, wsID)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"sessions": summaries, "next_cursor": nextCursor})
}

type createSessionRequest struct {
	Resume string `json:"resume,omitempty"`
	// WorkspaceID selects the owning workspace; empty = default workspace.
	WorkspaceID string `json:"workspace_id,omitempty"`
}

type archiveSessionRequest struct {
	Archived *bool `json:"archived"`
}

// handleArchiveSession marks a session archived (hidden from default
// listings) or restores it.
func (s *Server) handleArchiveSession(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var req archiveSessionRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	if req.Archived == nil {
		writeError(w, invalidInput("archived is required"))
		return
	}
	if err := s.svc.SetSessionArchived(r.Context(), id, *req.Archived); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("archive_session", id, "archived", *req.Archived)
	writeJSON(w, http.StatusOK, map[string]any{"archived": *req.Archived})
}

// handleDeleteSession removes a session and all its persisted data.
func (s *Server) handleDeleteSession(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	if err := s.svc.DeleteSession(r.Context(), id); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("delete_session", id)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleCreateSession(w http.ResponseWriter, r *http.Request) {
	var req createSessionRequest
	// An empty body means "new session"; a body may carry a resume target.
	// ContentLength is not a reliable emptiness signal on every transport —
	// chunked/HTTP2 clients and in-process mounts (the Wails AssetServer)
	// report -1 — so decide on the actual bytes.
	if body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBodyBytes)); err != nil {
		writeError(w, invalidInput("invalid request body: "+err.Error()))
		return
	} else if len(bytes.TrimSpace(body)) > 0 {
		decoder := json.NewDecoder(bytes.NewReader(body))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&req); err != nil {
			writeError(w, invalidInput("invalid request body: "+err.Error()))
			return
		}
	}
	if req.Resume != "" {
		id, err := domain.ParseSessionID(req.Resume)
		if err != nil || !domain.HasPrefix(id, "sess_") {
			writeError(w, invalidInput("invalid resume session id"))
			return
		}
		if h, ok := s.svc.Get(id); ok {
			writeJSON(w, http.StatusOK, map[string]any{"session_id": h.ID, "state": h.Controller.State()})
			return
		}
		h, err := s.svc.ResumeSession(r.Context(), id)
		if err != nil {
			writeError(w, err)
			return
		}
		s.auditf("session.resume", h.ID, "workspace_id", h.WorkspaceID.String())
		writeJSON(w, http.StatusCreated, map[string]any{"session_id": h.ID, "state": h.Controller.State(), "workspace_id": h.WorkspaceID.String()})
		return
	}
	wsID, err := parseWorkspaceIDParam(req.WorkspaceID)
	if err != nil {
		writeError(w, err)
		return
	}
	h, err := s.svc.CreateSession(r.Context(), wsID)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("session.create", h.ID, "workspace_id", h.WorkspaceID.String())
	writeJSON(w, http.StatusCreated, map[string]any{"session_id": h.ID, "state": h.Controller.State(), "workspace_id": h.WorkspaceID.String()})
}

func (s *Server) handleInspectSession(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	inspection, err := s.svc.Inspect(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, inspection)
}

func (s *Server) handleTranscript(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var after, limit int64
	if raw := r.URL.Query().Get("after"); raw != "" {
		if after, err = strconv.ParseInt(raw, 10, 64); err != nil || after < 0 {
			writeError(w, invalidInput("after must be a non-negative integer"))
			return
		}
	}
	if raw := r.URL.Query().Get("limit"); raw != "" {
		if limit, err = strconv.ParseInt(raw, 10, 64); err != nil || limit <= 0 {
			writeError(w, invalidInput("limit must be a positive integer"))
			return
		}
	}
	page, err := s.svc.Transcript(r.Context(), id, after, int(limit))
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, page)
}

func (s *Server) handleSnapshot(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	snap, err := s.svc.Snapshot(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, snap)
}

// --- turn control ---

type submitPromptRequest struct {
	Prompt         string                `json:"prompt"`
	Images         []domain.ImageContent `json:"images,omitempty"`
	IdempotencyKey string                `json:"idempotency_key,omitempty"`
	// Followup queues the prompt for AFTER the busy turn (next-turn
	// delivery) instead of steering into it. Text-only.
	Followup bool `json:"followup,omitempty"`
}

func (s *Server) handleSubmitPrompt(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var req submitPromptRequest
	if err := decodeBodyLimit(w, r, &req, maxPromptBodyBytes); err != nil {
		writeError(w, err)
		return
	}
	if req.Prompt == "" && len(req.Images) == 0 {
		writeError(w, invalidInput("prompt is required"))
		return
	}
	idemKey := r.Header.Get("Idempotency-Key")
	if idemKey == "" {
		idemKey = req.IdempotencyKey
	}
	result, deduplicated, err := s.svc.SubmitPrompt(r.Context(), id, req.Prompt, req.Images, idemKey, req.Followup)
	if err != nil {
		writeError(w, err)
		return
	}
	promptHash := sha256.Sum256([]byte(req.Prompt))
	s.auditf("prompt.submit", id, "prompt_len", len(req.Prompt), "prompt_hash", hex.EncodeToString(promptHash[:8]), "steered", result.Steered, "followup", result.Followup, "deduplicated", deduplicated)
	status := http.StatusAccepted
	if deduplicated {
		status = http.StatusOK
	}
	writeJSON(w, status, map[string]any{
		"turn":         result.Turn,
		"steered":      result.Steered,
		"followup":     result.Followup,
		"queue_len":    result.QueueLen,
		"deduplicated": deduplicated,
	})
}

func (s *Server) handleCancelTurn(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	if err := s.svc.CancelTurn(r.Context(), id); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("turn.cancel", id)
	writeJSON(w, http.StatusAccepted, map[string]string{"status": "cancelling"})
}

// --- feedback ---

type submitFeedbackRequest struct {
	RunID string `json:"run_id"`
	// Value is the vote: 1 = thumbs up, 0 = thumbs down (BOOLEAN score).
	Value   float64 `json:"value"`
	Comment string  `json:"comment,omitempty"`
}

// handleSubmitFeedback records a user vote for one run as a Langfuse score
// on the run's trace. The endpoint is deliberately cheap: the controller
// resolves run_id → trace_id from the transcript projection and the score
// submission itself is fire-and-forget.
func (s *Server) handleSubmitFeedback(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var req submitFeedbackRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	runID, err := domain.ParseRunID(req.RunID)
	if err != nil || !domain.HasPrefix(runID, "run_") {
		writeError(w, invalidInput("invalid run_id"))
		return
	}
	if req.Value != 0 && req.Value != 1 {
		writeError(w, invalidInput("value must be 0 (down) or 1 (up)"))
		return
	}
	if err := s.svc.SubmitFeedback(r.Context(), id, runID.String(), req.Value, req.Comment); err != nil {
		writeError(w, err)
		return
	}
	s.auditf("feedback.submit", id, "run_id", runID.String(), "value", req.Value)
	writeJSON(w, http.StatusOK, map[string]bool{"recorded": true})
}

// --- approvals & questions ---

type resolveApprovalRequest struct {
	CallID   string `json:"call_id"`
	ArgsHash string `json:"args_hash"`
	Decision string `json:"decision"`
	RuleHint *struct {
		ToolName  string          `json:"tool_name"`
		Arguments json.RawMessage `json:"arguments"`
		Trust     string          `json:"trust,omitempty"`
	} `json:"rule_hint,omitempty"`
	Client string `json:"client,omitempty"`
}

func (s *Server) handleResolveApproval(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	approvalID, err := parseEventParam(r, "approvalID")
	if err != nil {
		writeError(w, err)
		return
	}
	var req resolveApprovalRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	decision := domain.Decision(req.Decision)
	if decision != domain.DecisionAllow && decision != domain.DecisionDeny {
		writeError(w, invalidInput("decision must be allow or deny"))
		return
	}
	callID, err := domain.ParseToolCallID(req.CallID)
	if err != nil {
		writeError(w, invalidInput("invalid call_id"))
		return
	}
	var hint *app.ApprovalRuleHint
	if req.RuleHint != nil {
		hint = &app.ApprovalRuleHint{ToolName: req.RuleHint.ToolName, Arguments: req.RuleHint.Arguments, Trust: req.RuleHint.Trust}
	}
	note, err := s.svc.ResolveApproval(r.Context(), id, app.ApprovalBinding{
		ApprovalID: approvalID, CallID: callID, ArgsHash: req.ArgsHash,
	}, decision, hint, req.Client)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("approval.resolve", id, "approval_id", approvalID, "decision", decision, "actor", req.Client)
	writeJSON(w, http.StatusOK, map[string]string{"note": note})
}

type answerQuestionRequest struct {
	Selected   []string `json:"selected,omitempty"`
	CustomText string   `json:"custom_text,omitempty"`
	Skipped    bool     `json:"skipped,omitempty"`
}

func (s *Server) handleAnswerQuestion(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	questionID, err := parseEventParam(r, "questionID")
	if err != nil {
		writeError(w, err)
		return
	}
	var req answerQuestionRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	result, err := s.svc.AnswerQuestion(r.Context(), id, questionID, domain.QuestionAnswer{
		Selected:   req.Selected,
		CustomText: req.CustomText,
		Skipped:    req.Skipped,
	})
	if err != nil {
		writeError(w, err)
		return
	}
	if !result.Resolved {
		writeError(w, &statusError{status: http.StatusConflict, code: "binding_mismatch", message: "question unknown or already resolved"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"resolved": true})
}

// --- session configuration ---

type setModelRequest struct {
	Provider string `json:"provider"`
	Model    string `json:"model"`
}

func (s *Server) handleSetModel(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var req setModelRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	if req.Provider == "" || req.Model == "" {
		writeError(w, invalidInput("provider and model are required"))
		return
	}
	result, err := s.svc.SetModel(r.Context(), id, req.Provider+"/"+req.Model)
	if err != nil {
		writeError(w, err)
		return
	}
	s.auditf("session.set_model", id, "model", req.Provider+"/"+req.Model)
	writeJSON(w, http.StatusOK, result)
}

type setReasoningRequest struct {
	Effort string `json:"effort"`
}

func (s *Server) handleSetReasoning(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	var req setReasoningRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, err)
		return
	}
	result, err := s.svc.SetReasoning(r.Context(), id, req.Effort)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (s *Server) handleRequestCompaction(w http.ResponseWriter, r *http.Request) {
	id, err := parseSessionParam(r)
	if err != nil {
		writeError(w, err)
		return
	}
	result, err := s.svc.RequestCompaction(r.Context(), id)
	if err != nil {
		writeError(w, err)
		return
	}
	writeJSON(w, http.StatusAccepted, result)
}
