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
// Created: 2026/08/12

// Package browser implements the browser tool: headless Chrome controlled
// via chromedp, providing navigate/snapshot/screenshot/scroll/click/type/close
// operations with idle-TTL instance reaping and SSRF-aware URL gating through
// URLRequest. The snapshot action captures the accessibility tree (AX tree)
// and assigns ref numbers to interactive elements; click and type use those
// refs to interact with the page without fragile CSS selectors.
package browser

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// baseTool is the browser-local variant of the shared tool skeleton. Like
// webfetch it carries no path validator: browser operations touch no
// workspace paths, so its prepared-call fingerprint binds the canonical
// arguments, risk level, and the typed URLRequest for domain-rule
// evaluation by the policy layer (docs/BROWSER_DESIGN.md §5.3).
type baseTool struct {
	def domain.ToolDefinition
	key [32]byte
}

type preparedFingerprint struct {
	CallID     string             `json:"call_id"`
	ToolName   string             `json:"tool_name"`
	Arguments  json.RawMessage    `json:"arguments"`
	Risk       domain.RiskLevel   `json:"risk"`
	URLRequest *domain.URLRequest `json:"url_request,omitempty"`
}

func newBaseTool(def domain.ToolDefinition) (baseTool, error) {
	if err := def.Validate(); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return baseTool{def: def, key: key}, nil
}

func (b *baseTool) prepareCall(
	ctx context.Context,
	call domain.ToolCall,
	canonicalArgs json.RawMessage,
	approvalDesc string,
	risk domain.RiskLevel,
	urlRequest *domain.URLRequest,
) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != b.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", b.def.Name))
	}

	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      b.def.Name,
			Arguments: cloneRawMessage(canonicalArgs),
		},
		Definition:   b.def,
		Risk:         risk,
		ApprovalDesc: approvalDesc,
		URLRequest:   urlRequest,
	}
	prepared.ArgsHash = b.signPrepared(prepared)
	return prepared, nil
}

func (b *baseTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != b.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Name != b.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	if prepared.Definition.Source != b.def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	// Risk is graded per action (riskForAction), so recompute it from the
	// signed arguments instead of assuming the definition default — the
	// same re-derivation run_cmd performs for riskForArgs.
	args, err := decodeStrict[browserArgs](prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments are unreadable")
	}
	if prepared.Risk != riskForAction(args.Action) {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	if !sameCapabilities(prepared.Definition.Capabilities, b.def.Capabilities) {
		return domain.NewError(domain.ErrSecurity, "prepared call capabilities mismatch")
	}

	expected := b.signPrepared(prepared)
	if !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func (b *baseTool) signPrepared(prepared domain.PreparedCall) string {
	fingerprint := preparedFingerprint{
		CallID:     prepared.Call.ID.String(),
		ToolName:   prepared.Call.Name,
		Arguments:  cloneRawMessage(prepared.Call.Arguments),
		Risk:       prepared.Risk,
		URLRequest: prepared.URLRequest,
	}
	payload, _ := json.Marshal(fingerprint)

	h := hmac.New(sha256.New, b.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

// extractURLRequest parses a URL string and returns a typed URLRequest
// carrying the canonical lowercase host, or nil if the URL is not a valid
// http(s) URL with a host. The policy layer reads this field for domain
// rule evaluation instead of re-parsing tool arguments.
func extractURLRequest(rawURL string) *domain.URLRequest {
	u, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || u.Hostname() == "" {
		return nil
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil
	}
	return &domain.URLRequest{Host: strings.ToLower(u.Hostname())}
}

func decodeStrict[T any](raw json.RawMessage) (T, error) {
	var out T
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&out); err != nil {
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must be valid JSON matching the tool schema", domain.WithCause(err))
	}

	var trailing struct{}
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value")
		}
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value", domain.WithCause(err))
	}
	return out, nil
}

// withOpTimeout derives an operation context from the browser session
// context with a per-action timeout, while still honoring cancellation of
// the caller's context (e.g. the user interrupting the agent loop). The
// returned CancelFunc must be called to release both resources.
func withOpTimeout(ctx, browserCtx context.Context, timeout time.Duration) (context.Context, context.CancelFunc) {
	opCtx, cancel := context.WithTimeout(browserCtx, timeout)
	stop := context.AfterFunc(ctx, cancel)
	return opCtx, func() {
		stop()
		cancel()
	}
}

func cloneRawMessage(raw json.RawMessage) json.RawMessage {
	if raw == nil {
		return nil
	}
	return append(json.RawMessage(nil), raw...)
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

func successResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	content, err := json.Marshal(payload)
	if err != nil {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	finishedAt := time.Now()
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(content)}},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	status := domain.ToolStatusError
	code := string(domain.ErrInternal)
	message := "internal tool error"
	retryable := false

	switch {
	case errors.Is(err, context.Canceled):
		status = domain.ToolStatusCancelled
		code = string(domain.ErrCancelled)
		message = "operation cancelled"
	case errors.Is(err, context.DeadlineExceeded):
		status = domain.ToolStatusTimeout
		code = string(domain.ErrTimeout)
		message = "operation timed out"
	default:
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			code = string(agentErr.Code)
			message = agentErr.Message
			retryable = agentErr.Retryable
			switch agentErr.Code {
			case domain.ErrCancelled:
				status = domain.ToolStatusCancelled
			case domain.ErrTimeout:
				status = domain.ToolStatusTimeout
			}
		}
	}

	finishedAt := time.Now()
	return domain.ToolResult{
		CallID: callID,
		Status: status,
		Error: &domain.ToolError{
			Code:      code,
			Message:   message,
			Retryable: retryable,
		},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}
