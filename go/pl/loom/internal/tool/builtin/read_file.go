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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type readFileArgs struct {
	Path   string `json:"path"`
	Offset int    `json:"offset,omitempty"`
	Limit  int    `json:"limit,omitempty"`
}

type readFileLine struct {
	Number int    `json:"number"`
	Text   string `json:"text"`
}

type readFileOutput struct {
	Path        string         `json:"path"`
	Offset      int            `json:"offset"`
	Limit       int            `json:"limit"`
	TotalLines  int            `json:"total_lines"`
	Truncated   bool           `json:"truncated"`
	Lines       []readFileLine `json:"lines"`
	SizeBytes   int64          `json:"size_bytes"`
	ContentHash string         `json:"content_hash"`
}

// ReadFileTool implements the builtin read-only file reader. Successful
// reads record the file's content hash into the shared file-state book so
// edit can detect external modification later.
type ReadFileTool struct {
	base baseTool
	book *workspacepkg.FileStateBook
}

// NewReadFileTool creates a read_file tool bound to the workspace validator.
// A nil book disables state recording (used by tests).
func NewReadFileTool(validator *workspacepkg.PathValidator, book *workspacepkg.FileStateBook) (*ReadFileTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "read_file",
		Description:  "Read a UTF-8 text file within the workspace with line numbers. Paginate large files with offset/limit (max 500 lines per call). Binary files are rejected. You MUST read a file before editing it.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"offset":{"type":"integer","minimum":1},"limit":{"type":"integer","minimum":1,"maximum":500}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"integer"},"limit":{"type":"integer"},"total_lines":{"type":"integer"},"truncated":{"type":"boolean"},"lines":{"type":"array"},"size_bytes":{"type":"integer"},"content_hash":{"type":"string"}},"required":["path","offset","limit","total_lines","truncated","lines","size_bytes","content_hash"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ReadFileTool{base: base, book: book}, nil
}

func (t *ReadFileTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *ReadFileTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[readFileArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateReadFileArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read %s (offset=%d, limit=%d)", args.Path, args.Offset, args.Limit)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *ReadFileTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	result := t.execute(ctx, prepared, startedAt)
	return result
}

func (t *ReadFileTool) execute(ctx context.Context, prepared domain.PreparedCall, startedAt time.Time) domain.ToolResult {
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[readFileArgs](prepared.Call.Arguments)
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
	if pathInfo.Info.IsDir() {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a file"))
	}
	if !pathInfo.Info.Mode().IsRegular() {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file"))
	}

	data, err := readSmallFile(ctx, pathInfo.Absolute, maxReadFileBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if isBinaryContent(data) {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "file appears to be binary or not valid UTF-8"))
	}

	lines, err := splitLines(data, maxReadFileBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	selected, truncated := sliceReadFileLines(lines, args.Offset, args.Limit)
	contentHash := sha256Hex(data)
	t.book.Record(pathInfo.Absolute, contentHash)
	output := readFileOutput{
		Path:        args.Path,
		Offset:      args.Offset,
		Limit:       args.Limit,
		TotalLines:  len(lines),
		Truncated:   truncated,
		Lines:       selected,
		SizeBytes:   int64(len(data)),
		ContentHash: contentHash,
	}
	return successResult(prepared.Call.ID, startedAt, output)
}

func validateReadFileArgs(validator *workspacepkg.PathValidator, args readFileArgs) (readFileArgs, pathResolution, error) {
	if args.Offset == 0 {
		args.Offset = defaultReadFileOffset
	}
	if args.Limit == 0 {
		args.Limit = defaultReadFileLimit
	}
	if args.Offset < 1 {
		return readFileArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "offset must be at least 1")
	}
	if args.Limit < 1 || args.Limit > maxReadFileLimit {
		return readFileArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("limit must be between 1 and %d", maxReadFileLimit))
	}

	pathInfo, err := resolveExistingPath(validator, args.Path)
	if err != nil {
		return readFileArgs{}, pathResolution{}, err
	}
	if pathInfo.Info.IsDir() {
		return readFileArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a file")
	}
	if !pathInfo.Info.Mode().IsRegular() {
		return readFileArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file")
	}

	args.Path = pathInfo.Display
	return args, pathInfo, nil
}

func sliceReadFileLines(lines []string, offset, limit int) ([]readFileLine, bool) {
	if len(lines) == 0 {
		return []readFileLine{}, false
	}
	if offset > len(lines) {
		return []readFileLine{}, false
	}

	start := offset - 1
	end := start + limit
	if end > len(lines) {
		end = len(lines)
	}
	out := make([]readFileLine, 0, end-start)
	for i := start; i < end; i++ {
		out = append(out, readFileLine{Number: i + 1, Text: lines[i]})
	}
	return out, end < len(lines)
}
