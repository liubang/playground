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

package builtin

import (
	"context"
	"encoding/json"
	"fmt"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type searchArgs struct {
	Pattern       string   `json:"pattern"`
	Path          string   `json:"path,omitempty"`
	Glob          []string `json:"glob,omitempty"`
	Type          string   `json:"type,omitempty"`
	Context       int      `json:"context,omitempty"`
	CaseSensitive bool     `json:"case_sensitive,omitempty"`
	FixedStrings  bool     `json:"fixed_strings,omitempty"`
	NoIgnore      bool     `json:"no_ignore,omitempty"`
}

type searchMatch struct {
	Path   string        `json:"path"`
	Line   int           `json:"line"`
	Text   string        `json:"text"`
	Before []contextLine `json:"before,omitempty"`
	After  []contextLine `json:"after,omitempty"`
}

type searchOutput struct {
	Path            string        `json:"path"`
	Pattern         string        `json:"pattern"`
	Engine          string        `json:"engine"`
	CaseSensitive   bool          `json:"case_sensitive"`
	MatchCount      int           `json:"match_count"`
	Truncated       bool          `json:"truncated"`
	ScannedFiles    int           `json:"scanned_files,omitempty"`
	SkippedBinary   int           `json:"skipped_binary,omitempty"`
	SkippedTooLarge int           `json:"skipped_too_large,omitempty"`
	Matches         []searchMatch `json:"matches"`
}

var typeNamePattern = regexp.MustCompile(`^[A-Za-z0-9_+-]+$`)

// SearchTool implements content search with a ripgrep engine and a Go fallback.
type SearchTool struct {
	base   baseTool
	runner rgRunner
}

// NewSearchTool creates a search tool. The runner enables the ripgrep engine
// through the sandboxed process runner; a nil runner forces the Go fallback.
func NewSearchTool(validator *workspacepkg.PathValidator, runner rgRunner) (*SearchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "search",
		Description: "Search file contents recursively within the workspace. 'pattern' is a ripgrep regular expression " +
			"(use fixed_strings=true for literal text). Filter with glob (e.g. '*.go') or type. " +
			"Files matched by .gitignore are skipped by default. " +
			"Uses the ripgrep engine when available and falls back to a built-in literal search otherwise (noted in the output).",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"pattern":{"type":"string","minLength":1,"maxLength":4096},"path":{"type":"string","minLength":1},"glob":{"type":"array","maxItems":16,"items":{"type":"string","minLength":1,"maxLength":256}},"type":{"type":"string","minLength":1,"maxLength":64},"context":{"type":"integer","minimum":0,"maximum":5},"case_sensitive":{"type":"boolean"},"fixed_strings":{"type":"boolean"},"no_ignore":{"type":"boolean"}},"required":["pattern"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"pattern":{"type":"string"},"engine":{"type":"string"},"case_sensitive":{"type":"boolean"},"match_count":{"type":"integer"},"truncated":{"type":"boolean"},"scanned_files":{"type":"integer"},"skipped_binary":{"type":"integer"},"skipped_too_large":{"type":"integer"},"matches":{"type":"array"}},"required":["path","pattern","engine","case_sensitive","match_count","truncated","matches"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &SearchTool{base: base, runner: runner}, nil
}

func (t *SearchTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *SearchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[searchArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, pathInfo, err := validateSearchArgs(t.base.validator, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Search %q under %s", args.Pattern, args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *SearchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[searchArgs](prepared.Call.Arguments)
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

	if rgAvailable(t.runner) {
		return t.executeRipgrep(ctx, prepared, pathInfo, args, startedAt)
	}
	return t.executeGoFallback(ctx, prepared, pathInfo, args, startedAt)
}

func validateSearchArgs(validator *workspacepkg.PathValidator, args searchArgs) (searchArgs, pathResolution, error) {
	if strings.TrimSpace(args.Pattern) == "" {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "pattern is required")
	}
	if len(args.Pattern) > maxSearchQueryBytes {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("pattern exceeds %d bytes", maxSearchQueryBytes))
	}
	if args.Context < 0 || args.Context > maxSearchContextLines {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("context must be between 0 and %d", maxSearchContextLines))
	}
	if args.Type != "" && !typeNamePattern.MatchString(args.Type) {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "type must be an alphanumeric ripgrep type name")
	}
	for i, g := range args.Glob {
		if strings.TrimSpace(g) == "" || len(g) > 256 {
			return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("glob[%d] must be 1..256 bytes", i))
		}
	}
	if args.Path == "" {
		args.Path = "."
	}
	pathInfo, err := resolveExistingPath(validator, args.Path)
	if err != nil {
		return searchArgs{}, pathResolution{}, err
	}
	if !pathInfo.Info.IsDir() {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory")
	}
	args.Path = pathInfo.Display
	return args, pathInfo, nil
}

// --- ripgrep engine ---

func (t *SearchTool) executeRipgrep(ctx context.Context, prepared domain.PreparedCall, root pathResolution, args searchArgs, startedAt time.Time) domain.ToolResult {
	// Per-file cap guards against single-file floods; the aggregate layer
	// applies the global match budget on top.
	argv := rgCommonArgs(args.Context, args.CaseSensitive, args.FixedStrings, args.NoIgnore, args.Glob, args.Type, rgMaxCountHint)
	argv = append(argv, "--", args.Pattern, root.Absolute)

	stdout, err := runRipgrep(ctx, t.runner, root.Absolute, argv)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	events, err := decodeRgEvents(stdout)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	matches, truncated := aggregateRgMatches(events, root, args.Context, maxSearchMatches)

	return successResult(prepared.Call.ID, startedAt, searchOutput{
		Path:          args.Path,
		Pattern:       args.Pattern,
		Engine:        string(engineRipgrep),
		CaseSensitive: args.CaseSensitive,
		MatchCount:    len(matches),
		Truncated:     truncated,
		Matches:       matches,
	})
}

// aggregateRgMatches folds `rg --json` events into match entries with bounded
// before/after context, normalized to workspace-relative display paths.
func aggregateRgMatches(events []rgEvent, root pathResolution, contextLines, maxMatches int) ([]searchMatch, bool) {
	matches := []searchMatch{}
	var pendingBefore []contextLine
	lastMatch := -1
	truncated := false

	for _, evt := range events {
		switch evt.Type {
		case "match":
			if len(matches) >= maxMatches {
				truncated = true
				continue
			}
			m := searchMatch{
				Path:   displayUnderRoot(root, evt.Data.Path.Text),
				Line:   evt.Data.LineNumber,
				Text:   strings.TrimSuffix(evt.Data.Lines.Text, "\n"),
				Before: pendingBefore,
			}
			pendingBefore = nil
			matches = append(matches, m)
			lastMatch = len(matches) - 1
		case "context":
			line := contextLine{Line: evt.Data.LineNumber, Text: strings.TrimSuffix(evt.Data.Lines.Text, "\n")}
			if lastMatch >= 0 && len(matches[lastMatch].After) < contextLines {
				matches[lastMatch].After = append(matches[lastMatch].After, line)
				continue
			}
			pendingBefore = append(pendingBefore, line)
			if len(pendingBefore) > contextLines {
				pendingBefore = pendingBefore[1:]
			}
		}
	}
	return matches, truncated
}

// displayUnderRoot maps an rg-produced path back to the workspace-relative
// display form. rg --json reports absolute paths (rooted at root.Absolute),
// while rg --files reports paths relative to the search root's cwd.
func displayUnderRoot(root pathResolution, name string) string {
	if !filepath.IsAbs(name) {
		return filepath.ToSlash(filepath.Join(root.Display, name))
	}
	rel, err := filepath.Rel(root.Absolute, name)
	if err != nil {
		return name
	}
	return filepath.ToSlash(filepath.Join(root.Display, rel))
}

// --- Go fallback engine (literal substring search, no ignore rules) ---

func (t *SearchTool) executeGoFallback(ctx context.Context, prepared domain.PreparedCall, root pathResolution, args searchArgs, startedAt time.Time) domain.ToolResult {
	legacy := searchTextArgs{
		Path:          args.Path,
		Query:         args.Pattern,
		CaseSensitive: args.CaseSensitive,
		Before:        args.Context,
		After:         args.Context,
	}
	output, err := searchDirectory(ctx, t.base.validator, root, legacy)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	matches := make([]searchMatch, 0, len(output.Matches))
	for _, m := range output.Matches {
		matches = append(matches, searchMatch(m))
	}
	return successResult(prepared.Call.ID, startedAt, searchOutput{
		Path:            args.Path,
		Pattern:         args.Pattern,
		Engine:          string(engineGoFallback),
		CaseSensitive:   args.CaseSensitive,
		MatchCount:      len(matches),
		Truncated:       output.Truncated,
		ScannedFiles:    output.ScannedFiles,
		SkippedBinary:   output.SkippedBinary,
		SkippedTooLarge: output.SkippedTooLarge,
		Matches:         matches,
	})
}
