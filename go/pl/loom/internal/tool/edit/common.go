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
// Created: 2026/07/22 21:10

package edit

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxTextFileBytes    int64 = 1 << 20
	maxPatchBytes             = 1 << 20
	maxReplacementBytes       = 256 << 10
)

type baseTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	signer    toolkit.Signer
}

type editOutput struct {
	Path    string `json:"path"`
	OldHash string `json:"old_hash"`
	NewHash string `json:"new_hash"`
	Size    int64  `json:"size"`
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
	writePaths []string,
	approvalDesc string,
	writeRequest *domain.WriteRequest,
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
		WritePaths:   toolkit.SortedStrings(writePaths),
		WriteRequest: writeRequest,
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

// resolveWritePath resolves a write target. Paths confined to the
// workspace + scratch roots behave exactly as before; any other absolute
// path resolves to its canonical non-sensitive form and external=true
// marks the call as boundary-crossing — the producing tool must declare
// it via WriteRequest so the policy layer (path rules, session memory,
// per-mode baseline) gates the write. Sensitive locations stay denied
// here, before any policy evaluation.
func resolveWritePath(validator *workspacepkg.PathValidator, input string) (workspacepkg.ResolvedPath, bool, error) {
	if strings.TrimSpace(input) == "" {
		return workspacepkg.ResolvedPath{}, false, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	resolved, external, err := validator.ResolveWrite(input)
	if err != nil {
		return workspacepkg.ResolvedPath{}, false, domain.NewError(domain.ErrSecurity, "path is not writable", domain.WithCause(err))
	}
	return resolved, external, nil
}

// writeRequestOf derives the typed write contract for a resolved target.
func writeRequestOf(resolved workspacepkg.ResolvedPath, external bool) *domain.WriteRequest {
	return &domain.WriteRequest{Path: resolved.Absolute, OutsideRoots: external}
}

// verifyWriteRequestBinding re-checks at Execute time that the signed
// WriteRequest matches the re-resolved write target (defense in depth on
// top of the HMAC over the fingerprint).
func verifyWriteRequestBinding(prepared domain.PreparedCall, resolved workspacepkg.ResolvedPath) error {
	if prepared.WriteRequest == nil {
		return domain.NewError(domain.ErrSecurity, "prepared call carries no write request")
	}
	if prepared.WriteRequest.Path != resolved.Absolute {
		return domain.NewError(domain.ErrSecurity, "prepared call write request binding mismatch")
	}
	return nil
}

func ensureExistingTextFile(validator *workspacepkg.PathValidator, input string) (workspacepkg.ResolvedPath, bool, workspacepkg.Snapshot, []byte, error) {
	resolved, external, err := resolveWritePath(validator, input)
	if err != nil {
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, err
	}
	snapshot, err := validator.SnapshotResolved(resolved)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path does not exist: %q", domain.TruncateForErrorEcho(input)), domain.WithCause(err))
		}
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrSecurity, "path is not a writable regular file", domain.WithCause(err))
	}

	if snapshot.Size > maxTextFileBytes {
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("file exceeds size limit of %d bytes", maxTextFileBytes))
	}
	data, err := os.ReadFile(resolved.Absolute)
	if err != nil {
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrUnavailable, "failed to read file", domain.WithCause(err))
	}
	if int64(len(data)) > maxTextFileBytes {
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("file exceeds size limit of %d bytes", maxTextFileBytes))
	}
	if bytes.IndexByte(data, 0) >= 0 || !utf8.Valid(data) {
		return workspacepkg.ResolvedPath{}, false, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, "file appears to be binary or not valid UTF-8")
	}
	return resolved, external, snapshot, data, nil
}

func canonicalizeHash(value string) (string, error) {
	if value == "" {
		return "", domain.NewError(domain.ErrInvalidInput, "expected_hash is required")
	}
	if len(value) != 64 {
		return "", domain.NewError(domain.ErrInvalidInput, "expected_hash must be a lowercase SHA256 hex string")
	}
	for _, r := range value {
		if (r < '0' || r > '9') && (r < 'a' || r > 'f') {
			return "", domain.NewError(domain.ErrInvalidInput, "expected_hash must be a lowercase SHA256 hex string")
		}
	}
	return value, nil
}

func normalizeAtomicWriteError(err error) error {
	if err == nil {
		return nil
	}
	if strings.Contains(err.Error(), "expected hash mismatch") {
		return domain.NewError(domain.ErrConflict, "file changed since expected_hash was computed", domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "failed to write file atomically", domain.WithCause(err))
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
