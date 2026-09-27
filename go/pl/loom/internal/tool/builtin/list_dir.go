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
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type listDirArgs struct {
	Path  string `json:"path"`
	Depth int    `json:"depth,omitempty"`
}

type listDirEntry struct {
	Name    string `json:"name"`
	Path    string `json:"path"`
	Kind    string `json:"kind"`
	Size    int64  `json:"size"`
	Mode    string `json:"mode"`
	ModTime string `json:"mod_time"`
}

type listDirOutput struct {
	Path       string         `json:"path"`
	EntryCount int            `json:"entry_count"`
	Truncated  bool           `json:"truncated"`
	Entries    []listDirEntry `json:"entries"`
}

// ListDirTool implements deterministic directory listing.
type ListDirTool struct {
	base baseTool
}

// NewListDirTool creates a list_dir tool.
func NewListDirTool(validator *workspacepkg.PathValidator) (*ListDirTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "list_dir",
		Description: "List directory contents (name, kind, size, mode, mtime), deterministically " +
			"sorted and capped at 200 entries total. 'depth' (1-5, default 1) recurses into " +
			"subdirectories — use depth 2-3 to orient in an unfamiliar tree instead of chaining " +
			"single-level calls. Relative paths resolve inside the workspace; absolute paths " +
			"outside the workspace work too (credential locations are always excluded). " +
			"Use glob to find files by name across the tree, or grep to look inside file contents.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"depth":{"type":"integer","minimum":1,"maximum":5}},"required":["path"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ListDirTool{base: base}, nil
}

func (t *ListDirTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: listings are
// independent reads.
func (t *ListDirTool) ConcurrentSafe() bool { return true }

func (t *ListDirTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	// decodeLenient repairs the stringified-depth shape deviation models
	// make in the wild (observed in live transcripts, sub-agents included).
	args, err := decodeLenient[listDirArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateListDirArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("List directory %s", args.Path)
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ReadPaths: []string{pathInfo.Absolute}, ApprovalDesc: approvalDesc})
}

func (t *ListDirTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[listDirArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	pathInfo, err := resolveExistingPath(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}
	if !pathInfo.Info.IsDir() {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory"))
	}

	entries, truncated, err := readDirectoryEntries(ctx, t.base.validator, pathInfo, args.Depth)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, listDirOutput{
		Path:       args.Path,
		EntryCount: len(entries),
		Truncated:  truncated,
		Entries:    entries,
	})
}

func validateListDirArgs(validator *workspacepkg.PathValidator, args listDirArgs) (listDirArgs, pathResolution, error) {
	pathInfo, err := resolveExistingPath(validator, args.Path)
	if err != nil {
		return listDirArgs{}, pathResolution{}, err
	}
	if !pathInfo.Info.IsDir() {
		return listDirArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory")
	}
	if args.Depth == 0 {
		args.Depth = 1
	}
	if args.Depth < 1 || args.Depth > maxDirectoryDepth {
		return listDirArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("depth must be 1..%d", maxDirectoryDepth))
	}
	args.Path = pathInfo.Display
	return args, pathInfo, nil
}

// readDirectoryEntries lists dir recursively up to depth levels (1 = direct
// children only), capped at maxDirectoryEntries entries total. Entries are
// emitted in pre-order: each directory's children are sorted (directories
// first, then name) and a directory's subtree follows the directory entry.
func readDirectoryEntries(ctx context.Context, validator *workspacepkg.PathValidator, dir pathResolution, depth int) ([]listDirEntry, bool, error) {
	entries := make([]listDirEntry, 0, maxDirectoryEntries)
	truncated, err := walkDirectory(ctx, validator, dir, depth, &entries)
	if err != nil {
		return nil, false, err
	}
	return entries, truncated, nil
}

func walkDirectory(ctx context.Context, validator *workspacepkg.PathValidator, dir pathResolution, depth int, out *[]listDirEntry) (bool, error) {
	entries, err := os.ReadDir(dir.Absolute)
	if err != nil {
		return false, domain.NewError(domain.ErrUnavailable, "failed to read directory", domain.WithCause(err))
	}

	candidates := make([]listDirEntry, 0, len(entries))
	type childDir struct {
		display  string
		absolute string
	}
	subdirs := make(map[string]childDir, len(entries))
	for _, entry := range entries {
		if err := ctx.Err(); err != nil {
			return false, err
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
			return false, domain.NewError(domain.ErrUnavailable, "failed to stat directory entry", domain.WithCause(err))
		}
		if !info.Mode().IsRegular() && !info.IsDir() {
			continue
		}
		displayPath := joinDisplayPath(dir.Display, name)
		candidates = append(candidates, listDirEntry{
			Name:    name,
			Path:    displayPath,
			Kind:    entryKind(info),
			Size:    info.Size(),
			Mode:    info.Mode().String(),
			ModTime: info.ModTime().UTC().Format(time.RFC3339Nano),
		})
		if info.IsDir() {
			subdirs[name] = childDir{display: displayPath, absolute: fullPath}
		}
	}

	sort.Slice(candidates, func(i, j int) bool {
		if candidates[i].Kind != candidates[j].Kind {
			return candidates[i].Kind < candidates[j].Kind
		}
		return candidates[i].Name < candidates[j].Name
	})

	for _, candidate := range candidates {
		if len(*out) >= maxDirectoryEntries {
			return true, nil
		}
		*out = append(*out, candidate)
		subdir, ok := subdirs[candidate.Name]
		if !ok || depth <= 1 {
			continue
		}
		truncated, err := walkDirectory(ctx, validator, pathResolution{Absolute: subdir.absolute, Display: subdir.display}, depth-1, out)
		if err != nil {
			return false, err
		}
		if truncated {
			return true, nil
		}
	}
	return false, nil
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
