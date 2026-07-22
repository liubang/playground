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
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type applyPatchArgs struct {
	Path         string `json:"path"`
	Patch        string `json:"patch"`
	ExpectedHash string `json:"expected_hash"`
}

type parsedPatch struct {
	OldPath string
	NewPath string
	Hunks   []patchHunk
}

type patchHunk struct {
	OldStart int
	OldCount int
	NewStart int
	NewCount int
	Lines    []patchLine
}

type patchLine struct {
	Kind    byte
	Content string
}

// ApplyPatchTool implements a safe single-file unified diff applier.
type ApplyPatchTool struct {
	base baseTool
}

// NewApplyPatchTool creates an apply_patch tool.
func NewApplyPatchTool(validator *workspacepkg.PathValidator) (*ApplyPatchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "apply_patch",
		Description:  "Apply a safe single-file unified diff to a workspace file using expected_hash guarding.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"patch":{"type":"string","minLength":1},"expected_hash":{"type":"string","minLength":64,"maxLength":64}},"required":["path","patch","expected_hash"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"old_hash":{"type":"string"},"new_hash":{"type":"string"},"size":{"type":"integer"}},"required":["path","old_hash","new_hash","size"]}`),
		Capabilities: []domain.Capability{domain.CapFSWrite},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ApplyPatchTool{base: base}, nil
}

func (t *ApplyPatchTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *ApplyPatchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[applyPatchArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateApplyPatchArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	patch, err := parseUnifiedPatch(args.Patch)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if err := ensurePatchTargetsPath(pathInfo, patch); err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Apply patch to %s", args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *ApplyPatchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.WritePaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call write paths are invalid"))
	}

	args, err := decodeStrict[applyPatchArgs](prepared.Call.Arguments)
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

	patch, err := parseUnifiedPatch(args.Patch)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if err := ensurePatchTargetsPath(pathInfo, patch); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	newData, err := applyPatchToData(data, patch)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	resultSnapshot, err := t.base.validator.AtomicWrite(pathInfo.Absolute, newData, workspacepkg.AtomicWriteOptions{
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

func validateApplyPatchArgs(validator *workspacepkg.PathValidator, args applyPatchArgs) (applyPatchArgs, workspacepkg.ResolvedPath, error) {
	if len(args.Patch) == 0 {
		return applyPatchArgs{}, workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrInvalidInput, "patch is required")
	}
	if len(args.Patch) > maxPatchBytes {
		return applyPatchArgs{}, workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("patch exceeds %d bytes", maxPatchBytes))
	}
	expectedHash, err := canonicalizeHash(args.ExpectedHash)
	if err != nil {
		return applyPatchArgs{}, workspacepkg.ResolvedPath{}, err
	}
	pathInfo, _, _, err := ensureExistingTextFile(validator, args.Path)
	if err != nil {
		return applyPatchArgs{}, workspacepkg.ResolvedPath{}, err
	}
	args.Path = pathInfo.Display
	args.ExpectedHash = expectedHash
	return args, pathInfo, nil
}

func parseUnifiedPatch(text string) (parsedPatch, error) {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	lines := strings.Split(text, "\n")
	if len(lines) > 0 && lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	if len(lines) < 3 {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must contain unified diff headers and hunks")
	}
	for _, line := range lines {
		if strings.HasPrefix(line, "Binary files ") || strings.HasPrefix(line, "rename ") || strings.HasPrefix(line, "new file mode ") || strings.HasPrefix(line, "deleted file mode ") {
			return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must not contain binary, rename, create, or delete operations")
		}
	}
	if !strings.HasPrefix(lines[0], "--- ") || !strings.HasPrefix(lines[1], "+++ ") {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must start with --- and +++ headers")
	}
	oldPath := strings.TrimPrefix(lines[0], "--- ")
	newPath := strings.TrimPrefix(lines[1], "+++ ")
	if oldPath == "/dev/null" || newPath == "/dev/null" {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must not create or delete files")
	}
	oldPath = normalizePatchPath(oldPath)
	newPath = normalizePatchPath(newPath)
	if oldPath == "" || newPath == "" {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch paths must be non-empty")
	}
	if oldPath != newPath {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must target exactly one existing file without rename")
	}
	if !isSafePatchPath(oldPath) {
		return parsedPatch{}, domain.NewError(domain.ErrSecurity, "patch path escapes workspace or is invalid")
	}

	parsed := parsedPatch{OldPath: oldPath, NewPath: newPath}
	for i := 2; i < len(lines); {
		line := lines[i]
		if strings.HasPrefix(line, "diff ") || strings.HasPrefix(line, "--- ") || strings.HasPrefix(line, "+++ ") {
			return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must contain only one file")
		}
		if !strings.HasPrefix(line, "@@") {
			return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "unexpected patch content outside a hunk")
		}
		hunk, next, err := parseHunk(lines, i)
		if err != nil {
			return parsedPatch{}, err
		}
		parsed.Hunks = append(parsed.Hunks, hunk)
		i = next
	}
	if len(parsed.Hunks) == 0 {
		return parsedPatch{}, domain.NewError(domain.ErrInvalidInput, "patch must contain at least one hunk")
	}
	return parsed, nil
}

func parseHunk(lines []string, start int) (patchHunk, int, error) {
	header := lines[start]
	parts := strings.Split(header, "@@")
	if len(parts) < 3 {
		return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk header")
	}
	ranges := strings.Fields(strings.TrimSpace(parts[1]))
	if len(ranges) != 2 {
		return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk header ranges")
	}
	oldStart, oldCount, err := parseHunkRange(ranges[0], '-')
	if err != nil {
		return patchHunk{}, 0, err
	}
	newStart, newCount, err := parseHunkRange(ranges[1], '+')
	if err != nil {
		return patchHunk{}, 0, err
	}
	hunk := patchHunk{OldStart: oldStart, OldCount: oldCount, NewStart: newStart, NewCount: newCount}
	oldSeen := 0
	newSeen := 0
	i := start + 1
	for i < len(lines) {
		line := lines[i]
		if strings.HasPrefix(line, "@@") {
			break
		}
		if strings.HasPrefix(line, "\\ No newline at end of file") {
			if len(hunk.Lines) == 0 {
				return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "no-newline marker must follow a patch line")
			}
			hunk.Lines[len(hunk.Lines)-1].Content = strings.TrimSuffix(hunk.Lines[len(hunk.Lines)-1].Content, "\n")
			i++
			continue
		}
		if len(line) == 0 {
			return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "patch lines must include a prefix character")
		}
		kind := line[0]
		if kind != ' ' && kind != '+' && kind != '-' {
			return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "invalid patch line prefix")
		}
		content := line[1:] + "\n"
		hunk.Lines = append(hunk.Lines, patchLine{Kind: kind, Content: content})
		switch kind {
		case ' ':
			oldSeen++
			newSeen++
		case '-':
			oldSeen++
		case '+':
			newSeen++
		}
		i++
	}
	if oldSeen != oldCount || newSeen != newCount {
		return patchHunk{}, 0, domain.NewError(domain.ErrInvalidInput, "hunk line counts do not match header")
	}
	return hunk, i, nil
}

func parseHunkRange(text string, prefix byte) (int, int, error) {
	if len(text) < 2 || text[0] != prefix {
		return 0, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk range prefix")
	}
	body := text[1:]
	parts := strings.Split(body, ",")
	start, err := strconv.Atoi(parts[0])
	if err != nil || start < 0 {
		return 0, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk start line")
	}
	count := 1
	if len(parts) == 2 {
		count, err = strconv.Atoi(parts[1])
		if err != nil || count < 0 {
			return 0, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk line count")
		}
	} else if len(parts) > 2 {
		return 0, 0, domain.NewError(domain.ErrInvalidInput, "invalid hunk range")
	}
	return start, count, nil
}

func ensurePatchTargetsPath(path workspacepkg.ResolvedPath, patch parsedPatch) error {
	if patch.NewPath != path.Display {
		return domain.NewError(domain.ErrConflict, fmt.Sprintf("patch targets %q but request path is %q", patch.NewPath, path.Display))
	}
	return nil
}

func applyPatchToData(data []byte, patch parsedPatch) ([]byte, error) {
	lines := splitFileLines(data)
	result := make([]fileLine, 0, len(lines))
	cursor := 0
	for _, hunk := range patch.Hunks {
		target := hunk.OldStart
		if hunk.OldCount == 0 {
			target++
		}
		copyUntil := target - 1
		if copyUntil < cursor {
			return nil, domain.NewError(domain.ErrConflict, "patch hunks overlap or are out of order")
		}
		if copyUntil > len(lines) {
			return nil, domain.NewError(domain.ErrConflict, "patch hunk starts beyond end of file")
		}
		result = append(result, lines[cursor:copyUntil]...)
		cursor = copyUntil

		for _, line := range hunk.Lines {
			expected := fileLine{Text: strings.TrimSuffix(line.Content, "\n"), HasNewline: strings.HasSuffix(line.Content, "\n")}
			switch line.Kind {
			case ' ':
				if cursor >= len(lines) || lines[cursor] != expected {
					return nil, domain.NewError(domain.ErrConflict, "patch context does not match file content")
				}
				result = append(result, lines[cursor])
				cursor++
			case '-':
				if cursor >= len(lines) || lines[cursor] != expected {
					return nil, domain.NewError(domain.ErrConflict, "patch deletion does not match file content")
				}
				cursor++
			case '+':
				result = append(result, expected)
			}
		}
	}
	result = append(result, lines[cursor:]...)
	joined, err := joinFileLines(result)
	if err != nil {
		return nil, err
	}
	return joined, nil
}

func normalizePatchPath(path string) string {
	path = strings.TrimSpace(path)
	path = strings.TrimPrefix(path, "a/")
	path = strings.TrimPrefix(path, "b/")
	return strings.TrimSpace(path)
}

func isSafePatchPath(path string) bool {
	if path == "" || strings.HasPrefix(path, "/") {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if part == ".." || part == "." || part == "" {
			return false
		}
	}
	return true
}
