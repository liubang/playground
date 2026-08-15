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
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type readFileArgs struct {
	Path   string `json:"path"`
	Offset int    `json:"offset,omitempty"`
	Limit  int    `json:"limit,omitempty"`
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
		Name: "read_file",
		Description: "Read a UTF-8 text file. Relative paths resolve inside the workspace; absolute paths outside " +
			"the workspace are readable too, except credential locations (~/.ssh, ~/.aws, ~/.kube, .env files, " +
			"and similar), which are always denied. Output is cat -n style plain text: a header line " +
			"(path, shown/total line range, size, sha256 content hash) followed by lines rendered as '<number>→<text>'. " +
			"Paginate large files with offset/limit (max 500 lines per call); when the output ends with a truncated " +
			"marker, keep reading with offset until it disappears. Binary files are rejected. " +
			"You MUST read a file before editing it.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1},"offset":{"type":"integer","minimum":1},"limit":{"type":"integer","minimum":1,"maximum":500}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"string","description":"cat -n style numbered lines with a metadata header line and an optional trailing truncation marker"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &ReadFileTool{base: base, book: book}, nil
}

func (t *ReadFileTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: reads are
// independent and the file-state book is mutex-protected.
func (t *ReadFileTool) ConcurrentSafe() bool { return true }

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
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ReadPaths: []string{pathInfo.Absolute}, ApprovalDesc: approvalDesc})
}

func (t *ReadFileTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	result := t.execute(ctx, prepared, startedAt)
	return result
}

func (t *ReadFileTool) execute(ctx context.Context, prepared domain.PreparedCall, startedAt time.Time) domain.ToolResult {
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[readFileArgs](prepared.Call.Arguments)
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
	if pathInfo.Info.IsDir() {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a file"))
	}
	if !pathInfo.Info.Mode().IsRegular() {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file"))
	}

	data, err := readSmallFile(ctx, pathInfo.Absolute, maxReadFileBytes)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if toolkit.IsBinaryContent(data) {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "file appears to be binary or not valid UTF-8"))
	}

	lines, err := splitLines(data, maxReadFileBytes)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	selected, first, last, truncated := sliceReadFileLines(lines, args.Offset, args.Limit)
	contentHash := sha256Hex(data)
	t.book.Record(pathInfo.Absolute, contentHash)
	output := formatReadFileText(args.Path, selected, first, last, len(lines), truncated, args.Offset, args.Limit, int64(len(data)), contentHash)
	return textResult(prepared.Call.ID, startedAt, output)
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
	if args.Limit < 1 {
		return readFileArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "limit must be at least 1")
	}
	if args.Limit > maxReadFileLimit {
		// Models routinely ask for "the rest of the file" with an oversized
		// limit (e.g. the exact remaining line count, 683 > 500 observed in
		// the wild). The output header always reports the actual shown range
		// and total line count, so clamping is transparent and saves a wasted
		// model round trip over a reject-retry cycle.
		args.Limit = maxReadFileLimit
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

// sliceReadFileLines selects the [offset, offset+limit) window (1-based).
// first/last are the 1-based numbers of the first and last selected line;
// they are zero when the window is empty.
func sliceReadFileLines(lines []string, offset, limit int) (selected []string, first, last int, truncated bool) {
	if len(lines) == 0 || offset > len(lines) {
		return nil, 0, 0, false
	}

	start := offset - 1
	end := start + limit
	if end > len(lines) {
		end = len(lines)
	}
	return lines[start:end], start + 1, end, end < len(lines)
}

// formatReadFileText renders the cat -n style plain-text output: a compact
// metadata header, numbered lines, and a trailing truncation marker that
// tells the model exactly how to continue.
func formatReadFileText(path string, selected []string, first, last, total int, truncated bool, offset, limit int, sizeBytes int64, contentHash string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "path: %s · lines %d-%d of %d · %s · sha256:%s\n", path, first, last, total, formatByteSize(sizeBytes), contentHash)
	switch {
	case total == 0:
		b.WriteString("(empty file)\n")
	case len(selected) == 0:
		fmt.Fprintf(&b, "(offset %d is beyond the end of file: %d lines total)\n", offset, total)
	default:
		for i, text := range selected {
			fmt.Fprintf(&b, "%6d→%s\n", first+i, text)
		}
	}
	if truncated {
		remaining := total - last
		noun := "lines"
		if remaining == 1 {
			noun = "line"
		}
		fmt.Fprintf(&b, "[truncated: %d more %s; call read_file with offset=%d to continue]", remaining, noun, last+1)
	}
	return b.String()
}

// formatByteSize renders a byte count in compact human form (B/KB/MB).
func formatByteSize(n int64) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	default:
		return fmt.Sprintf("%.1f MB", float64(n)/(1024*1024))
	}
}

// textResult builds a successful plain-text tool result.
func textResult(callID domain.ToolCallID, startedAt time.Time, text string) domain.ToolResult {
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: text}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
