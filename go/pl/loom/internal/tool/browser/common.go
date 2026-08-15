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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3). Risk is graded per action
// (riskForAction), so Prepare passes an explicit Risk override and
// Execute re-derives it from the signed arguments before verifying.
type baseTool struct {
	toolkit.BaseTool
}

func newBaseTool(def domain.ToolDefinition) (baseTool, error) {
	bt, err := toolkit.NewBaseTool(def)
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{BaseTool: bt}, nil
}

// verifyPreparedCall verifies the prepared call with the shared structural
// checks (name, definition, source, capabilities, HMAC) and then re-derives
// the risk from the signed action — the same re-derivation run_cmd
// performs for riskForArgs. The shared VerifyPreparedCall cannot be used
// directly because the definition-level default risk is not the graded one.
func (b *baseTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if err := b.BaseTool.VerifyPreparedCallStructural(prepared); err != nil {
		return err
	}
	args, err := toolkit.DecodeStrict[browserArgs](prepared.Call.Arguments)
	if err != nil {
		return domain.NewError(domain.ErrSecurity, "prepared call arguments are unreadable")
	}
	if prepared.Risk != riskForAction(args.Action) {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	return nil
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
