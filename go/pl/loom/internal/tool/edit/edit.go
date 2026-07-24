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

package edit

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type editArgs struct {
	Path       string `json:"path"`
	OldString  string `json:"old_string"`
	NewString  string `json:"new_string"`
	ReplaceAll bool   `json:"replace_all,omitempty"`
	// ExpectedHash is an optional advanced guard; drift detection works
	// without it via the shared file-state book.
	ExpectedHash string `json:"expected_hash,omitempty"`
}

// EditTool implements exact old_string replacement with internalized drift
// detection. It supersedes replace_text and apply_patch.
type EditTool struct {
	base baseTool
	book *workspacepkg.FileStateBook
}

// NewEditTool creates an edit tool. A nil book disables drift detection (used
// by tests); production assembly shares one book with read_file.
func NewEditTool(validator *workspacepkg.PathValidator, book *workspacepkg.FileStateBook) (*EditTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "edit",
		Description: "Replace exact text in a single workspace file. old_string must match exactly one location " +
			"(or use replace_all=true). You MUST read_file the target first: edits are rejected if the file " +
			"changed since your last read. expected_hash is optional and rarely needed.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"old_string":{"type":"string"},"new_string":{"type":"string"},"replace_all":{"type":"boolean"},"expected_hash":{"type":"string","minLength":64,"maxLength":64}},"required":["path","old_string","new_string"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"old_hash":{"type":"string"},"new_hash":{"type":"string"},"size":{"type":"integer"}},"required":["path","old_hash","new_hash","size"]}`),
		Capabilities: []domain.Capability{domain.CapFSWrite},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &EditTool{base: base, book: book}, nil
}

func (t *EditTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *EditTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[editArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, data, err := validateEditArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	newContent, recoveryErr := applyEditReplacement(string(data), args)
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Edit %s", args.Path)
	prepared, err := t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if recoveryErr == nil {
		prepared.Recovery = &domain.RecoverySpec{
			Kind:         "file_replace",
			Path:         pathInfo.Absolute,
			ExpectedHash: sha256Hex(data),
			ResultHash:   hashBytes([]byte(newContent)),
		}
	}
	return prepared, nil
}

func (t *EditTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.WritePaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call write paths are invalid"))
	}

	args, err := decodeStrict[editArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, oldSnapshot, data, err := ensureExistingTextFile(t.base.validator, prepared.WritePaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	// Drift checks: the explicit hash (when supplied) is authoritative;
	// otherwise the shared file-state book detects external modification
	// since the agent's last read.
	if args.ExpectedHash != "" && oldSnapshot.SHA256 != args.ExpectedHash {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file changed since expected_hash was computed"))
	}
	if args.ExpectedHash == "" {
		if known, stale := t.book.Stale(pathInfo.Absolute, oldSnapshot.SHA256); known && stale {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file changed since your last read; read it again and re-apply the edit"))
		}
	}

	newContent, err := applyEditReplacement(string(data), args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	resultSnapshot, err := t.base.validator.AtomicWrite(pathInfo.Absolute, []byte(newContent), workspacepkg.AtomicWriteOptions{
		ExpectedHash: oldSnapshot.SHA256,
		SyncParent:   true,
	})
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, normalizeAtomicWriteError(err))
	}
	t.book.Record(pathInfo.Absolute, resultSnapshot.SHA256)
	return successResult(prepared.Call.ID, startedAt, editOutput{
		Path:    resultSnapshot.Path,
		OldHash: oldSnapshot.SHA256,
		NewHash: resultSnapshot.SHA256,
		Size:    resultSnapshot.Size,
	})
}

func validateEditArgs(validator *workspacepkg.PathValidator, args editArgs) (editArgs, workspacepkg.ResolvedPath, []byte, error) {
	if len(args.NewString) > maxReplacementBytes {
		return editArgs{}, workspacepkg.ResolvedPath{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("new_string exceeds %d bytes", maxReplacementBytes))
	}
	if len(args.OldString) == 0 {
		return editArgs{}, workspacepkg.ResolvedPath{}, nil, domain.NewError(domain.ErrInvalidInput, "old_string must not be empty")
	}
	if args.ExpectedHash != "" {
		expectedHash, err := canonicalizeHash(args.ExpectedHash)
		if err != nil {
			return editArgs{}, workspacepkg.ResolvedPath{}, nil, err
		}
		args.ExpectedHash = expectedHash
	}
	pathInfo, _, data, err := ensureExistingTextFile(validator, args.Path)
	if err != nil {
		return editArgs{}, workspacepkg.ResolvedPath{}, nil, err
	}
	args.Path = pathInfo.Display
	return args, pathInfo, data, nil
}

func applyEditReplacement(content string, args editArgs) (string, error) {
	count := strings.Count(content, args.OldString)
	if count == 0 {
		return "", domain.NewError(domain.ErrConflict, "old_string was not found in the file")
	}
	if !args.ReplaceAll && count > 1 {
		return "", domain.NewError(domain.ErrConflict, "old_string matched multiple locations; set replace_all=true to replace all matches")
	}
	if args.ReplaceAll {
		return strings.ReplaceAll(content, args.OldString, args.NewString), nil
	}
	return strings.Replace(content, args.OldString, args.NewString, 1), nil
}
