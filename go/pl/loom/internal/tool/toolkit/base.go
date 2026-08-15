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
// Created: 2026/08/15

package toolkit

import (
	"context"
	"encoding/json"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// BaseTool is the shared skeleton every tool package embeds: the tool
// definition plus a per-instance HMAC signer, and the two protocol
// methods every Prepare/Execute pair uses (REVIEW R3). Tool-specific
// dependencies (path validator, process runner, ...) live in the
// embedding struct; newBaseTool wrappers in each package keep validating
// those before delegating here.
type BaseTool struct {
	Def    domain.ToolDefinition
	signer Signer
}

// NewBaseTool validates the definition and creates a BaseTool with a
// fresh per-instance signer.
func NewBaseTool(def domain.ToolDefinition) (BaseTool, error) {
	if err := def.Validate(); err != nil {
		return BaseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	signer, err := NewSigner()
	if err != nil {
		return BaseTool{}, err
	}
	return BaseTool{Def: def, signer: signer}, nil
}

// PrepareOptions carries the per-call contract fields a tool derives
// during Prepare. The single struct keeps the protocol signature stable
// as new typed contracts (ExecRequest, URLRequest, ...) are added.
type PrepareOptions struct {
	// ReadPaths are the canonical absolute paths this call will read.
	ReadPaths []string
	// WritePaths are the canonical absolute paths this call will write.
	WritePaths []string
	// ApprovalDesc is the human-readable description shown at approval.
	ApprovalDesc string
	// WriteRequest is the typed write contract (edit, write).
	WriteRequest *domain.WriteRequest
	// URLRequest is the typed URL contract (web_fetch, browser).
	URLRequest *domain.URLRequest
	// Risk overrides the definition's default risk level. Tools that
	// grade risk per argument (browser's riskForAction) set it so the
	// signed fingerprint carries the graded level.
	Risk *domain.RiskLevel
}

// PrepareCall validates the incoming call, normalizes it against the
// tool definition, and signs a PreparedCall covering the canonical
// arguments plus every contract field in opts. It is side-effect free.
func (b *BaseTool) PrepareCall(ctx context.Context, call domain.ToolCall, canonicalArgs json.RawMessage, opts PrepareOptions) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if err := ValidateCallName(call, b.Def); err != nil {
		return domain.PreparedCall{}, err
	}

	risk := b.Def.Risk()
	if opts.Risk != nil {
		risk = *opts.Risk
	}
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      b.Def.Name,
			Arguments: CloneRawMessage(canonicalArgs),
		},
		Definition:   b.Def,
		Risk:         risk,
		ApprovalDesc: opts.ApprovalDesc,
		ReadPaths:    SortedStrings(opts.ReadPaths),
		WritePaths:   SortedStrings(opts.WritePaths),
		URLRequest:   opts.URLRequest,
		WriteRequest: opts.WriteRequest,
	}
	prepared.ArgsHash = b.signer.Sign(prepared)
	return prepared, nil
}

// VerifyPreparedCall re-checks the prepared call against this tool's
// definition (name, source, capabilities, risk) and the HMAC signature
// computed at Prepare time.
func (b *BaseTool) VerifyPreparedCall(prepared domain.PreparedCall) error {
	return b.signer.VerifyWithRisk(prepared, b.Def)
}

// VerifyPreparedCallStructural re-checks the prepared call exactly like
// VerifyPreparedCall except the risk level. Tools that re-derive risk
// from the signed arguments (browser's riskForAction, command's
// riskForArgs) call this and then check the derived risk themselves.
func (b *BaseTool) VerifyPreparedCallStructural(prepared domain.PreparedCall) error {
	return b.signer.Verify(prepared, b.Def)
}
