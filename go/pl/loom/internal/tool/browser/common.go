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
	"context"
	"encoding/json"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// baseTool is the browser-local variant of the shared tool skeleton,
// delegating signing and verification to the toolkit package. The
// fingerprint includes the typed URLRequest for domain-rule evaluation
// by the policy layer (docs/BROWSER_DESIGN.md §5.3).
type baseTool struct {
	def    domain.ToolDefinition
	signer toolkit.Signer
}

func newBaseTool(def domain.ToolDefinition) (baseTool, error) {
	if err := def.Validate(); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	signer, err := toolkit.NewSigner()
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{def: def, signer: signer}, nil
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
	if err := toolkit.ValidateCallName(call, b.def); err != nil {
		return domain.PreparedCall{}, err
	}

	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      b.def.Name,
			Arguments: toolkit.CloneRawMessage(canonicalArgs),
		},
		Definition:   b.def,
		Risk:         risk,
		ApprovalDesc: approvalDesc,
		URLRequest:   urlRequest,
	}
	prepared.ArgsHash = b.signer.Sign(prepared)
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
	args, err := toolkit.DecodeStrict[browserArgs](prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments are unreadable")
	}
	if prepared.Risk != riskForAction(args.Action) {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	if !toolkit.SameCapabilities(prepared.Definition.Capabilities, b.def.Capabilities) {
		return domain.NewError(domain.ErrSecurity, "prepared call capabilities mismatch")
	}

	return b.signer.Verify(prepared, b.def)
}

// extractURLRequest parses a URL string and returns a typed URLRequest
// carrying the canonical lowercase host, or nil if the URL is not a valid
// http(s) URL with a host. The policy layer reads this field for domain
// rule evaluation instead of re-parsing tool arguments.
func extractURLRequest(rawURL string) *domain.URLRequest {
	host, ok := domain.HostFromURL(rawURL)
	if !ok {
		return nil
	}
	return &domain.URLRequest{Host: host}
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

// --- Local aliases to the shared toolkit helpers ---
// These thin wrappers let the rest of the package keep using the short
// names without a bulk import change. They are zero-cost: the compiler
// inlines them away.

func decodeStrict[T any](raw json.RawMessage) (T, error) { return toolkit.DecodeStrict[T](raw) }

func cloneRawMessage(raw json.RawMessage) json.RawMessage { return toolkit.CloneRawMessage(raw) }

func sameCapabilities(left, right []domain.Capability) bool {
	return toolkit.SameCapabilities(left, right)
}

func successResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	return toolkit.SuccessResult(callID, startedAt, payload)
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	return toolkit.ErrorResult(callID, startedAt, err)
}
