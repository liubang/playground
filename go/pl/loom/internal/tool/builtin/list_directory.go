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

package builtin

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type listDirectoryArgs struct {
	Path string `json:"path"`
}

type listDirectoryEntry struct {
	Name    string `json:"name"`
	Path    string `json:"path"`
	Kind    string `json:"kind"`
	Size    int64  `json:"size"`
	Mode    string `json:"mode"`
	ModTime string `json:"mod_time"`
}

type listDirectoryOutput struct {
	Path       string               `json:"path"`
	EntryCount int                  `json:"entry_count"`
	Truncated  bool                 `json:"truncated"`
	Entries    []listDirectoryEntry `json:"entries"`
}

// ListDirectoryTool implements deterministic directory listing.
type ListDirectoryTool struct {
	base baseTool
}

// NewListDirectoryTool creates a list_directory tool.
func NewListDirectoryTool(validator *workspacepkg.PathValidator) (*ListDirectoryTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "list_directory",
		Description:  "List directory entries within the workspace in deterministic order.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"entry_count":{"type":"integer"},"truncated":{"type":"boolean"},"entries":{"type":"array"}},"required":["path","entry_count","truncated","entries"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ListDirectoryTool{base: base}, nil
}

func (t *ListDirectoryTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *ListDirectoryTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[listDirectoryArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateListDirectoryArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("List directory %s", args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *ListDirectoryTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[listDirectoryArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	pathInfo, err := resolveExistingPath(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}
	if !pathInfo.Info.IsDir() {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory"))
	}

	entries, truncated, err := readDirectoryEntries(ctx, t.base.validator, pathInfo)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	return successResult(prepared.Call.ID, startedAt, listDirectoryOutput{
		Path:       args.Path,
		EntryCount: len(entries),
		Truncated:  truncated,
		Entries:    entries,
	})
}

func validateListDirectoryArgs(validator *workspacepkg.PathValidator, args listDirectoryArgs) (listDirectoryArgs, pathResolution, error) {
	pathInfo, err := resolveExistingPath(validator, args.Path)
	if err != nil {
		return listDirectoryArgs{}, pathResolution{}, err
	}
	if !pathInfo.Info.IsDir() {
		return listDirectoryArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory")
	}
	args.Path = pathInfo.Display
	return args, pathInfo, nil
}

func readDirectoryEntries(ctx context.Context, validator *workspacepkg.PathValidator, dir pathResolution) ([]listDirectoryEntry, bool, error) {
	entries, err := os.ReadDir(dir.Absolute)
	if err != nil {
		return nil, false, domain.NewError(domain.ErrUnavailable, "failed to read directory", domain.WithCause(err))
	}

	candidates := make([]listDirectoryEntry, 0, len(entries))
	for _, entry := range entries {
		if err := ctx.Err(); err != nil {
			return nil, false, err
		}
		name := entry.Name()
		if containsSensitiveComponent(name) {
			continue
		}
		if entry.Type()&os.ModeSymlink != 0 {
			continue
		}
		fullPath := filepath.Join(dir.Absolute, name)
		if _, err := resolveExistingPath(validator, fullPath); err != nil {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			return nil, false, domain.NewError(domain.ErrUnavailable, "failed to stat directory entry", domain.WithCause(err))
		}
		if !info.Mode().IsRegular() && !info.IsDir() {
			continue
		}
		displayPath := joinDisplayPath(dir.Display, name)
		candidates = append(candidates, listDirectoryEntry{
			Name:    name,
			Path:    displayPath,
			Kind:    entryKind(info),
			Size:    info.Size(),
			Mode:    info.Mode().String(),
			ModTime: info.ModTime().UTC().Format(time.RFC3339Nano),
		})
	}

	sort.Slice(candidates, func(i, j int) bool {
		if candidates[i].Kind != candidates[j].Kind {
			return candidates[i].Kind < candidates[j].Kind
		}
		return candidates[i].Name < candidates[j].Name
	})

	truncated := len(candidates) > maxDirectoryEntries
	if truncated {
		candidates = candidates[:maxDirectoryEntries]
	}
	return candidates, truncated, nil
}

func joinDisplayPath(base, name string) string {
	if base == "." {
		return filepath.ToSlash(name)
	}
	return filepath.ToSlash(filepath.Join(base, name))
}

func entryKind(info os.FileInfo) string {
	if info.IsDir() {
		return "directory"
	}
	return "file"
}
