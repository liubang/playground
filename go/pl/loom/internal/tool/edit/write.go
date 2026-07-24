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
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const maxWriteBytes = 1 << 20

// writeArgs is the model-visible schema: only path and content. The model
// cannot forge freshness state because it never sees the canonical form.
type writeArgs struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

// writeCanonical is the signed canonical form carried by the PreparedCall.
// It binds the file state observed at prepare time, so a drift between
// approval and execution is caught by the freshness re-check and by the
// state validation in Execute.
type writeCanonical struct {
	Path    string `json:"path"`
	Content string `json:"content"`
	Created bool   `json:"created"`
	OldHash string `json:"old_hash"`
}

type writeOutput struct {
	Path    string `json:"path"`
	Size    int64  `json:"size"`
	Created bool   `json:"created"`
	OldHash string `json:"old_hash,omitempty"`
	NewHash string `json:"new_hash"`
}

// WriteTool implements whole-file create/overwrite with state binding.
type WriteTool struct {
	base baseTool
}

// NewWriteTool creates a write tool.
func NewWriteTool(validator *workspacepkg.PathValidator) (*WriteTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "write",
		Description: "Write a UTF-8 text file within the workspace: create it (parent directories are created as needed) " +
			"or overwrite it entirely. Prefer edit for partial modifications. " +
			"The approval shows the byte count and whether the call creates or overwrites.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"content":{"type":"string"}},"required":["path","content"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"size":{"type":"integer"},"created":{"type":"boolean"},"old_hash":{"type":"string"},"new_hash":{"type":"string"}},"required":["path","size","created","new_hash"]}`),
		Capabilities: []domain.Capability{domain.CapFSWrite},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &WriteTool{base: base}, nil
}

func (t *WriteTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *WriteTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[writeArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if len(args.Content) > maxWriteBytes {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("content exceeds %d bytes", maxWriteBytes))
	}
	if !utf8.ValidString(args.Content) {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "content must be valid UTF-8 text")
	}

	pathInfo, err := resolveWritePath(t.base.validator, args.Path)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical := writeCanonical{
		Path:    pathInfo.Display,
		Content: args.Content,
		Created: true,
		OldHash: workspacepkg.EmptyFileSHA256,
	}
	snapshot, statErr := t.base.validator.Snapshot(pathInfo.Absolute)
	switch {
	case statErr == nil:
		canonical.Created = false
		canonical.OldHash = snapshot.SHA256
	case errors.Is(statErr, os.ErrNotExist):
		// Creating a new file: parent directories are materialized in Execute.
		// Note: errors.Is (not os.IsNotExist) is required here because the
		// validator wraps lstat failures with fmt.Errorf %%w chains, which
		// os.IsNotExist does not unwrap.
	default:
		return domain.PreparedCall{}, domain.NewError(domain.ErrSecurity, "path is not a writable regular file", domain.WithCause(statErr))
	}

	rawCanonical, err := json.Marshal(canonical)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	action := "create"
	if !canonical.Created {
		action = "overwrite"
	}
	approvalDesc := fmt.Sprintf("Write %s (%d bytes, %s)", canonical.Path, len(canonical.Content), action)
	prepared, err := t.base.prepareCall(ctx, call, rawCanonical, []string{pathInfo.Absolute}, approvalDesc)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if !canonical.Created {
		prepared.Recovery = &domain.RecoverySpec{
			Kind:         "file_replace",
			Path:         pathInfo.Absolute,
			ExpectedHash: canonical.OldHash,
			ResultHash:   hashBytes([]byte(canonical.Content)),
		}
	}
	return prepared, nil
}

func (t *WriteTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.WritePaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call write paths are invalid"))
	}

	canonical, err := decodeStrict[writeCanonical](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, err := resolveWritePath(t.base.validator, prepared.WritePaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != canonical.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	// The file state must still match what the approval was bound to.
	snapshot, statErr := t.base.validator.Snapshot(pathInfo.Absolute)
	switch {
	case statErr == nil:
		if canonical.Created {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file was created since approval; re-check and re-issue the write"))
		}
		if snapshot.SHA256 != canonical.OldHash {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file changed since approval; read it again and re-issue the write"))
		}
	case errors.Is(statErr, os.ErrNotExist):
		if !canonical.Created {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrConflict, "file was deleted since approval"))
		}
	default:
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "path is not a writable regular file", domain.WithCause(statErr)))
	}

	if canonical.Created {
		if err := os.MkdirAll(filepath.Dir(pathInfo.Absolute), 0o755); err != nil {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable, "failed to create parent directories", domain.WithCause(err)))
		}
	}
	resultSnapshot, err := t.base.validator.AtomicWrite(pathInfo.Absolute, []byte(canonical.Content), workspacepkg.AtomicWriteOptions{
		ExpectedHash: canonical.OldHash,
		SyncParent:   true,
	})
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, normalizeAtomicWriteError(err))
	}

	out := writeOutput{
		Path:    resultSnapshot.Path,
		Size:    resultSnapshot.Size,
		Created: canonical.Created,
		NewHash: resultSnapshot.SHA256,
	}
	if !canonical.Created {
		out.OldHash = canonical.OldHash
	}
	return successResult(prepared.Call.ID, startedAt, out)
}
