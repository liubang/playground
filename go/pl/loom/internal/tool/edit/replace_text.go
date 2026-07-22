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
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type replaceTextArgs struct {
	Path         string `json:"path"`
	OldText      string `json:"old_text"`
	NewText      string `json:"new_text"`
	ExpectedHash string `json:"expected_hash"`
	ReplaceAll   bool   `json:"replace_all,omitempty"`
}

// ReplaceTextTool implements a bounded, hash-guarded text replacement tool.
type ReplaceTextTool struct {
	base baseTool
}

// NewReplaceTextTool creates a replace_text tool.
func NewReplaceTextTool(validator *workspacepkg.PathValidator) (*ReplaceTextTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "replace_text",
		Description:  "Safely replace text in a single workspace file using expected_hash guarding.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"old_text":{"type":"string"},"new_text":{"type":"string"},"expected_hash":{"type":"string","minLength":64,"maxLength":64},"replace_all":{"type":"boolean"}},"required":["path","old_text","new_text","expected_hash"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"old_hash":{"type":"string"},"new_hash":{"type":"string"},"size":{"type":"integer"}},"required":["path","old_hash","new_hash","size"]}`),
		Capabilities: []domain.Capability{domain.CapFSWrite},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ReplaceTextTool{base: base}, nil
}

func (t *ReplaceTextTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *ReplaceTextTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[replaceTextArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateReplaceTextArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Replace text in %s", args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *ReplaceTextTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.WritePaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call write paths are invalid"))
	}

	args, err := decodeStrict[replaceTextArgs](prepared.Call.Arguments)
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
	if oldSnapshot.SHA256 != args.ExpectedHash {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file changed since expected_hash was computed"))
	}

	newContent, err := applyStringReplacement(string(data), args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	resultSnapshot, err := t.base.validator.AtomicWrite(pathInfo.Absolute, []byte(newContent), workspacepkg.AtomicWriteOptions{
		ExpectedHash: args.ExpectedHash,
		SyncParent:   true,
	})
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, normalizeAtomicWriteError(err))
	}
	return successResult(prepared.Call.ID, startedAt, editOutput{
		Path:    resultSnapshot.Path,
		OldHash: oldSnapshot.SHA256,
		NewHash: resultSnapshot.SHA256,
		Size:    resultSnapshot.Size,
	})
}

func validateReplaceTextArgs(validator *workspacepkg.PathValidator, args replaceTextArgs) (replaceTextArgs, workspacepkg.ResolvedPath, error) {
	if len(args.NewText) > maxReplacementBytes {
		return replaceTextArgs{}, workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("new_text exceeds %d bytes", maxReplacementBytes))
	}
	if len(args.OldText) == 0 {
		return replaceTextArgs{}, workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrInvalidInput, "old_text must not be empty")
	}
	expectedHash, err := canonicalizeHash(args.ExpectedHash)
	if err != nil {
		return replaceTextArgs{}, workspacepkg.ResolvedPath{}, err
	}
	pathInfo, _, _, err := ensureExistingTextFile(validator, args.Path)
	if err != nil {
		return replaceTextArgs{}, workspacepkg.ResolvedPath{}, err
	}
	args.Path = pathInfo.Display
	args.ExpectedHash = expectedHash
	return args, pathInfo, nil
}

func applyStringReplacement(content string, args replaceTextArgs) (string, error) {
	count := strings.Count(content, args.OldText)
	if count == 0 {
		return "", domain.NewError(domain.ErrConflict, "old_text was not found in the file")
	}
	if !args.ReplaceAll && count > 1 {
		return "", domain.NewError(domain.ErrConflict, "old_text matched multiple locations; set replace_all=true to replace all matches")
	}
	if args.ReplaceAll {
		return strings.ReplaceAll(content, args.OldText, args.NewText), nil
	}
	return strings.Replace(content, args.OldText, args.NewText, 1), nil
}
