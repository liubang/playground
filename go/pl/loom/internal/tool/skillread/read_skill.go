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
// Created: 2026/07/26

// Package skillread implements the read_skill builtin tool: whitelist reads
// of discovered skill files (SKILL.md and files inside the skill directory),
// which live outside the workspace and therefore cannot go through read_file.
// See go/pl/loom/docs/SKILL_DESIGN.md §4.4.
package skillread

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultPath          = skill.FileName
	defaultOffset        = 1
	defaultLimit         = 200
	maxLimit             = 500
	maxFileBytes         = skill.MaxSkillFileBytes
	maxNameBytes         = 128
	maxPathBytes         = 4096
	approvalDescMaxBytes = 512
)

// readSkillArgs is the canonical argument shape. ResolvedPath is an internal
// field: it is computed during Prepare and covered by the HMAC signature, so
// the loop's verifyPreparedFreshness detects catalog drift between Prepare
// and Execute (re-Prepare recomputes it). Models never set it; Prepare
// always overwrites it.
type readSkillArgs struct {
	Name         string `json:"name"`
	Path         string `json:"path,omitempty"`
	Offset       int    `json:"offset,omitempty"`
	Limit        int    `json:"limit,omitempty"`
	ResolvedPath string `json:"resolved_path,omitempty"`
}

// ReadSkillTool reads files of discovered skills by name. Authorization is
// the discovered-skill whitelist: paths are resolved inside the owning
// skill's directory via a per-skill workspace.PathValidator (reusing its
// battle-tested Clean + EvalSymlinks + prefix checks).
type ReadSkillTool struct {
	def     domain.ToolDefinition
	catalog *skill.AtomicCatalog
	signer  toolkit.Signer
}

// NewReadSkillTool creates the tool bound to the shared catalog snapshot.
func NewReadSkillTool(catalog *skill.AtomicCatalog) (*ReadSkillTool, error) {
	if catalog == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "skill catalog is required")
	}
	def := domain.ToolDefinition{
		Name: "read_skill",
		Description: "Read a discovered skill's SKILL.md or a file inside its directory by skill name. " +
			"Output is cat -n style plain text: a header line (skill name, path, the skill's absolute 'dir' — usable " +
			"to build absolute script paths for run_cmd — shown/total line range, size) followed by lines rendered " +
			"as '<number>→<text>'. Only 'name' is required: path defaults to 'SKILL.md' and must be a relative path " +
			"inside the skill directory; offset/limit paginate long files (max 500 lines per call, read until the " +
			"truncated marker disappears). The skills catalog in the system prompt lists the available names.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"name":{"type":"string","minLength":1,"maxLength":128},"path":{"type":"string","maxLength":4096},"offset":{"type":"integer","minimum":1},"limit":{"type":"integer","minimum":1,"maximum":500}},"required":["name"]}`),
		OutputSchema: json.RawMessage(`{"type":"string","description":"cat -n style numbered lines with a metadata header line and an optional trailing truncation marker"}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}
	signer, err := toolkit.NewSigner()
	if err != nil {
		return nil, err
	}
	return &ReadSkillTool{def: def, catalog: catalog, signer: signer}, nil
}

// Definition returns the tool definition.
func (t *ReadSkillTool) Definition() domain.ToolDefinition { return t.def }

// ConcurrentSafe implements domain.ConcurrentSafely: skill reads are
// independent file reads.
func (t *ReadSkillTool) ConcurrentSafe() bool { return true }

// Prepare locates and resolves the target file without reading its contents
// (side-effect free and deterministic across the freshness re-Prepare).
func (t *ReadSkillTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}

	raw, err := decodeStrict[readSkillArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, resolved, err := t.resolveArgs(raw)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: cloneRawMessage(canonical),
		},
		Definition:   t.def,
		Risk:         t.def.Risk(),
		ReadPaths:    []string{resolved},
		ApprovalDesc: approvalDescription(args),
	}
	prepared.ArgsHash = t.signer.Sign(prepared)
	return prepared, nil
}

// resolveArgs validates args and resolves the target file inside the owning
// skill's directory. The returned args carry the resolved absolute path.
func (t *ReadSkillTool) resolveArgs(raw readSkillArgs) (readSkillArgs, string, error) {
	args := raw
	args.Name = strings.TrimSpace(args.Name)
	if args.Name == "" {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, "name is required")
	}
	if len(args.Name) > maxNameBytes || strings.ContainsRune(args.Name, 0) {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, "name is invalid")
	}
	if strings.TrimSpace(args.Path) == "" {
		args.Path = defaultPath
	}
	if len(args.Path) > maxPathBytes || strings.ContainsRune(args.Path, 0) {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, "path is invalid")
	}
	if filepath.IsAbs(args.Path) {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, "path must be relative to the skill directory")
	}
	if args.Offset == 0 {
		args.Offset = defaultOffset
	}
	if args.Limit == 0 {
		args.Limit = defaultLimit
	}
	if args.Offset < 1 {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, "offset must be at least 1")
	}
	if args.Limit < 1 || args.Limit > maxLimit {
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("limit must be between 1 and %d", maxLimit))
	}

	sk := t.catalog.Get().Find(args.Name)
	if sk == nil {
		available := t.catalog.Get().Names()
		if len(available) == 0 {
			return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput,
				fmt.Sprintf("skill %q not found: no skills are available in this session", args.Name))
		}
		return readSkillArgs{}, "", domain.NewError(domain.ErrInvalidInput,
			fmt.Sprintf("skill %q not found: available skills: %s", args.Name, strings.Join(available, ", ")))
	}
	resolved, err := resolveInsideSkillDir(sk, args.Path)
	if err != nil {
		return readSkillArgs{}, "", err
	}
	args.ResolvedPath = resolved
	return args, resolved, nil
}

// resolveInsideSkillDir resolves rel inside the skill directory using a
// per-skill workspace.PathValidator (Clean + EvalSymlinks + prefix check),
// rejects sensitive components, and stats the result.
func resolveInsideSkillDir(sk *skill.Skill, rel string) (string, error) {
	validator, err := workspacepkg.NewPathValidator(sk.Dir)
	if err != nil {
		return "", domain.NewError(domain.ErrInternal, "failed to initialize skill path validator", domain.WithCause(err))
	}
	resolved, err := validator.Validate(rel)
	if err != nil {
		return "", domain.NewError(domain.ErrSecurity, "path escapes the skill directory or is invalid", domain.WithCause(err))
	}
	relToSkill, err := filepath.Rel(sk.Dir, resolved)
	if err != nil {
		return "", domain.NewError(domain.ErrInternal, "failed to normalize path", domain.WithCause(err))
	}
	if hasSensitiveComponent(relToSkill) {
		return "", domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}
	info, err := os.Stat(resolved)
	if err != nil {
		if os.IsNotExist(err) {
			// Echo the offending path so the model can tell which file
			// inside the skill directory was missing.
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path does not exist: %q", domain.TruncateForErrorEcho(rel)))
		}
		return "", domain.NewError(domain.ErrUnavailable, "failed to stat path", domain.WithCause(err))
	}
	if !info.Mode().IsRegular() {
		return "", domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file")
	}
	if info.Size() > maxFileBytes {
		return "", domain.NewError(domain.ErrInvalidInput, "file exceeds 256KB limit")
	}
	return resolved, nil
}

// Execute re-verifies the binding against the CURRENT catalog snapshot
// (fail closed on drift), then reads and paginates the file.
func (t *ReadSkillTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := decodeStrict[readSkillArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if args.ResolvedPath == "" || len(prepared.ReadPaths) != 1 || prepared.ReadPaths[0] != args.ResolvedPath {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding is invalid"))
	}

	// Re-resolve from the current snapshot: the skill must still exist and
	// the relative path must still resolve to the signed absolute path.
	sk := t.catalog.Get().Find(args.Name)
	if sk == nil {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity,
			fmt.Sprintf("skill %q disappeared between prepare and execute", args.Name)))
	}
	resolved, err := resolveInsideSkillDir(sk, args.Path)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if resolved != args.ResolvedPath {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	data, err := readFileBounded(ctx, resolved, maxFileBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if toolkit.IsBinaryContent(data) {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "file appears to be binary or not valid UTF-8"))
	}
	lines, err := splitLines(data)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	selected, first, last, truncated := sliceLines(lines, args.Offset, args.Limit)
	return textResult(prepared.Call.ID, startedAt, formatReadSkillText(
		args.Name, args.Path, sk.Dir, selected, first, last, len(lines), truncated, args.Offset, int64(len(data))))
}

// formatReadSkillText renders the cat -n style plain-text output, keeping
// the skill's absolute directory in the header so the model can build
// script paths for run_cmd without a second call.
func formatReadSkillText(name, path, dir string, selected []string, first, last, total int, truncated bool, offset int, sizeBytes int64) string {
	var b strings.Builder
	fmt.Fprintf(&b, "skill: %s · path: %s · dir: %s · lines %d-%d of %d · %s\n", name, path, dir, first, last, total, formatByteSize(sizeBytes))
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
		fmt.Fprintf(&b, "[truncated: %d more %s; call read_skill with offset=%d to continue]", remaining, noun, last+1)
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

func (t *ReadSkillTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	return t.signer.VerifyWithRisk(prepared, t.def)
}

func approvalDescription(args readSkillArgs) string {
	desc := fmt.Sprintf("Read skill %s: %s", args.Name, args.Path)
	if len(desc) > approvalDescMaxBytes {
		desc = desc[:approvalDescMaxBytes]
	}
	return desc
}

// hasSensitiveComponent reports whether any path component is sensitive
// (reuses the exported workspace.IsSensitive per-component check).
func hasSensitiveComponent(path string) bool {
	clean := filepath.Clean(path)
	if clean == "." || clean == string(filepath.Separator) {
		return false
	}
	for _, part := range strings.Split(clean, string(filepath.Separator)) {
		if workspacepkg.IsSensitive(part) {
			return true
		}
	}
	return false
}

// sliceLines selects the [offset, offset+limit) window (1-based); first
// and last are the 1-based numbers of the selected range, zero when empty.
func sliceLines(lines []string, offset, limit int) (selected []string, first, last int, truncated bool) {
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

func splitLines(data []byte) ([]string, error) {
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(make([]byte, 0, 4096), maxFileBytes)
	var lines []string
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	if err := scanner.Err(); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to read text content", domain.WithCause(err))
	}
	return lines, nil
}

func readFileBounded(ctx context.Context, path string, maxBytes int64) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to open file", domain.WithCause(err))
	}
	defer func() { _ = file.Close() }()
	info, err := file.Stat()
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to stat file", domain.WithCause(err))
	}
	if !info.Mode().IsRegular() {
		return nil, domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file")
	}
	if info.Size() > maxBytes {
		return nil, domain.NewError(domain.ErrInvalidInput, "file exceeds 256KB limit")
	}
	data, err := io.ReadAll(io.LimitReader(file, maxBytes+1))
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to read file", domain.WithCause(err))
	}
	if int64(len(data)) > maxBytes {
		return nil, domain.NewError(domain.ErrInvalidInput, "file exceeds 256KB limit")
	}
	return data, nil
}

func decodeStrict[T any](raw json.RawMessage) (T, error) { return toolkit.DecodeStrict[T](raw) }

func cloneRawMessage(raw json.RawMessage) json.RawMessage { return toolkit.CloneRawMessage(raw) }

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

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	return toolkit.ErrorResult(callID, startedAt, err)
}
