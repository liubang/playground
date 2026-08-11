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
// Created: 2026/07/24

package webfetch

import (
	"context"
	"encoding/json"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
)

// baseTool is the webfetch-local variant of the shared tool skeleton,
// delegating signing and verification to the toolkit package. The
// fingerprint includes the typed URLRequest for domain-rule evaluation
// by the policy layer.
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
		Risk:         b.def.Risk(),
		ApprovalDesc: approvalDesc,
		URLRequest:   urlRequest,
	}
	prepared.ArgsHash = b.signer.Sign(prepared)
	return prepared, nil
}

func (b *baseTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	return b.signer.VerifyWithRisk(prepared, b.def)
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
