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

package lint

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type baseTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	signer    toolkit.Signer
}

type pathResolution struct {
	Absolute string
	Display  string
	Info     os.FileInfo
}

func newBaseTool(def domain.ToolDefinition, validator *workspacepkg.PathValidator) (baseTool, error) {
	if validator == nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if err := def.Validate(); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	signer, err := toolkit.NewSigner()
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{def: def, validator: validator, signer: signer}, nil
}

func (b *baseTool) prepareCall(
	ctx context.Context,
	call domain.ToolCall,
	canonicalArgs json.RawMessage,
	readPaths []string,
	approvalDesc string,
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
		ReadPaths:    toolkit.SortedStrings(readPaths),
	}
	prepared.ArgsHash = b.signer.Sign(prepared)
	return prepared, nil
}

func (b *baseTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	return b.signer.VerifyWithRisk(prepared, b.def)
}

// --- Local aliases to the shared toolkit helpers ---

func decodeStrict[T any](raw json.RawMessage) (T, error) { return toolkit.DecodeStrict[T](raw) }

func cloneRawMessage(raw json.RawMessage) json.RawMessage { return toolkit.CloneRawMessage(raw) }

func sortedStrings(values []string) []string { return toolkit.SortedStrings(values) }

func sameCapabilities(left, right []domain.Capability) bool {
	return toolkit.SameCapabilities(left, right)
}

func successResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	return toolkit.SuccessResult(callID, startedAt, payload)
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	return toolkit.ErrorResult(callID, startedAt, err)
}

func resolveExistingPath(validator *workspacepkg.PathValidator, input string) (pathResolution, error) {
	if strings.TrimSpace(input) == "" {
		return pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	if rel, ok := lexicalWorkspaceRelativePath(validator, input); ok && containsSensitiveComponent(rel) {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	resolved, err := validator.Validate(input)
	if err != nil {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid", domain.WithCause(err))
	}

	rel, err := filepath.Rel(validator.Root(), resolved)
	if err != nil {
		return pathResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize path", domain.WithCause(err))
	}
	if containsSensitiveComponent(rel) {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	info, err := os.Stat(resolved)
	if err != nil {
		if os.IsNotExist(err) {
			return pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path does not exist: %q", domain.TruncateForErrorEcho(input)))
		}
		return pathResolution{}, domain.NewError(domain.ErrUnavailable, "failed to stat path", domain.WithCause(err))
	}

	return pathResolution{
		Absolute: resolved,
		Display:  displayPath(rel),
		Info:     info,
	}, nil
}

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func lexicalWorkspaceRelativePath(validator *workspacepkg.PathValidator, input string) (string, bool) {
	clean := filepath.Clean(input)
	if !filepath.IsAbs(clean) {
		return clean, true
	}

	root := filepath.Clean(validator.Root())
	candidate := filepath.Clean(clean)
	if candidate != root && !strings.HasPrefix(candidate, root+string(filepath.Separator)) {
		return "", false
	}
	rel, err := filepath.Rel(root, candidate)
	if err != nil {
		return "", false
	}
	return rel, true
}

// containsSensitiveComponent delegates to the single canonical list in the
// workspace package (REVIEW R4).
func containsSensitiveComponent(path string) bool {
	return workspacepkg.ContainsSensitiveComponent(path)
}
