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
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type searchArgs struct {
	Pattern       string   `json:"pattern"`
	Path          string   `json:"path,omitempty"`
	Glob          []string `json:"glob,omitempty"`
	Type          string   `json:"type,omitempty"`
	Context       int      `json:"context,omitempty"`
	HeadLimit     int      `json:"head_limit,omitempty"`
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
	Note            string        `json:"note,omitempty"`
}

var typeNamePattern = regexp.MustCompile(`^[A-Za-z0-9_+-]+$`)

// clampMatchBudget resolves the effective match cap: 0 (unset) takes the
// default, and any explicit value stays within it.
func clampMatchBudget(headLimit int) int {
	if headLimit <= 0 || headLimit > maxSearchMatches {
		return maxSearchMatches
	}
	return headLimit
}

// rgTypeSet probes `rg --type-list` once per process and caches the valid
// type-name set. A nil set means ripgrep is unavailable (or the probe
// failed); callers then fall back to syntax-only validation — acceptable
// because the Go fallback engine does not apply type filters either.
var rgTypeSet = sync.OnceValue(func() map[string]bool {
	path, ok := rgLocator()
	if !ok {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, path, "--type-list").Output()
	if err != nil {
		return nil
	}
	set := make(map[string]bool)
	for line := range strings.Lines(string(out)) {
		name, _, _ := strings.Cut(line, ":")
		if name = strings.TrimSpace(name); name != "" {
			set[name] = true
		}
	}
	return set
})

// normalizeTypeFilter maps model-sentinel values to unset and strips stray
// quoting: some models serialize an unset field as the string "null"/"none",
// or double-encode the value with literal quotes (e.g. "\"go\"").
func normalizeTypeFilter(v string) string {
	v = strings.TrimSpace(v)
	v = strings.Trim(v, `"'`)
	v = strings.TrimSpace(v)
	if strings.EqualFold(v, "null") || strings.EqualFold(v, "none") {
		return ""
	}
	return v
}

// SearchTool implements content search with a ripgrep engine and a Go fallback.
type SearchTool struct {
	base   baseTool
	runner rgRunner
}

// NewSearchTool creates a search tool. The runner enables the ripgrep engine
// through the sandboxed process runner; a nil runner forces the Go fallback.
func NewSearchTool(validator *workspacepkg.PathValidator, runner rgRunner) (*SearchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "grep",
		Description: "Search file contents with ripgrep semantics. 'pattern' is a ripgrep regular expression " +
			"(fixed_strings=true for literal text). " +
			"'path' defaults to the workspace root and may point to a directory (searched recursively) or a single " +
			"file, including absolute paths outside the workspace (credential locations are always excluded). " +
			"Filter with glob (a pattern without '/' matches the file name at any depth, one with '/' matches the " +
			"workspace-relative path; prefix with '!' to exclude; multiple patterns union) or with type (a ripgrep " +
			"type name like 'go'; omit it when unsure — prefer glob for file-name filtering). " +
			"head_limit caps the number of returned matches (default " + strconv.Itoa(maxSearchMatches) + "); " +
			"context adds up to 5 surrounding lines per match. " +
			"Files matched by .gitignore are skipped unless no_ignore=true. " +
			"Uses the ripgrep engine when available and falls back to a built-in literal search otherwise (the " +
			"fallback treats 'pattern' as literal text; unapplied filters are disclosed in the output's note).",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"pattern":{"type":"string","minLength":1,"maxLength":4096},"path":{"type":"string","minLength":1},"glob":{"type":"array","maxItems":16,"items":{"type":"string","minLength":1,"maxLength":256}},"type":{"type":"string","minLength":1,"maxLength":64},"context":{"type":"integer","minimum":0,"maximum":5},"head_limit":{"type":"integer","minimum":1,"maximum":200},"case_sensitive":{"type":"boolean"},"fixed_strings":{"type":"boolean"},"no_ignore":{"type":"boolean"}},"required":["pattern"]}`),
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
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: each search spawns
// an independent rg process and shares no state across calls.
func (t *SearchTool) ConcurrentSafe() bool { return true }

// normalizeSearchJSON repairs the argument-shape deviations models make
// in the wild before the strict decode, mirroring the tolerance the
// type filter already applies: a lone glob string becomes the array the
// schema names (some models inherit a string-typed glob from other
// toolkits), and boolean fields arrive stringified. Everything the
// patcher does not recognize passes through untouched, so the strict
// decoder still rejects genuinely unknown shapes.
func normalizeSearchJSON(raw json.RawMessage) json.RawMessage {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		return raw
	}
	patched := false
	fix := func(key string, rewrite func(json.RawMessage) (json.RawMessage, bool)) {
		value, ok := fields[key]
		if !ok {
			return
		}
		if next, changed := rewrite(value); changed {
			fields[key] = next
			patched = true
		}
	}
	fix("glob", func(value json.RawMessage) (json.RawMessage, bool) {
		var single string
		if err := json.Unmarshal(value, &single); err != nil {
			return nil, false
		}
		out, err := json.Marshal([]string{single})
		if err != nil {
			return nil, false
		}
		return out, true
	})
	for _, key := range []string{"case_sensitive", "fixed_strings", "no_ignore"} {
		fix(key, func(value json.RawMessage) (json.RawMessage, bool) {
			var text string
			if err := json.Unmarshal(value, &text); err != nil {
				return nil, false
			}
			out, err := json.Marshal(strings.EqualFold(strings.TrimSpace(text), "true"))
			if err != nil {
				return nil, false
			}
			return out, true
		})
	}
	if !patched {
		return raw
	}
	out, err := json.Marshal(fields)
	if err != nil {
		return raw
	}
	return out
}

func (t *SearchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[searchArgs](normalizeSearchJSON(call.Arguments))
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
	approvalDesc := fmt.Sprintf("Grep %q under %s", args.Pattern, args.Path)
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ReadPaths: []string{pathInfo.Absolute}, ApprovalDesc: approvalDesc})
}

func (t *SearchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[searchArgs](prepared.Call.Arguments)
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
	if !pathInfo.Info.IsDir() && !pathInfo.Info.Mode().IsRegular() {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "path must refer to a file or directory"))
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
	if args.Context < 0 {
		args.Context = 0
	}
	// Clamp rather than reject: models routinely probe a slightly larger
	// window, and clamping costs nothing the output header does not
	// already disclose (the read_file limit precedent).
	if args.Context > maxSearchContextLines {
		args.Context = maxSearchContextLines
	}
	args.HeadLimit = clampMatchBudget(args.HeadLimit)
	args.Type = normalizeTypeFilter(args.Type)
	if args.Type != "" {
		if !typeNamePattern.MatchString(args.Type) {
			return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "type must be an alphanumeric ripgrep type name")
		}
		// Syntax-valid names can still be unknown to rg (e.g. "null" slips
		// past the pattern above and rg exits 2). Validate against the
		// probed type list and tell the model how to recover, so a bad
		// call is correctable instead of retried blind.
		if types := rgTypeSet(); types != nil && !types[args.Type] {
			return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unknown ripgrep type %q; omit the type filter or use glob for file-name filtering (valid names: see `rg --type-list`)", args.Type))
		}
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
	// Accept single files as well as directories: models routinely scope a
	// search to one file (rg handles file targets natively).
	if !pathInfo.Info.IsDir() && !pathInfo.Info.Mode().IsRegular() {
		return searchArgs{}, pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a file or directory")
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

	// cwd must be a directory: a single-file target runs from its parent.
	cwd := root.Absolute
	if !root.Info.IsDir() {
		cwd = filepath.Dir(root.Absolute)
	}
	stdout, rgTruncated, err := runRipgrep(ctx, t.runner, cwd, argv)
	if err != nil {
		if isSandboxFailure(err) {
			// The sandbox cannot execute anything on this platform (e.g.
			// Linux fails closed); degrade to the built-in engine.
			return t.executeGoFallback(ctx, prepared, root, args, startedAt)
		}
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	events, partial, err := decodeRgEvents(stdout)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	matches, truncated := aggregateRgMatches(events, root, args.Context, args.HeadLimit)

	return toolkit.SuccessResult(prepared.Call.ID, startedAt, searchOutput{
		Path:          args.Path,
		Pattern:       args.Pattern,
		Engine:        string(engineRipgrep),
		CaseSensitive: args.CaseSensitive,
		MatchCount:    len(matches),
		Truncated:     truncated || rgTruncated || partial,
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
	var output searchTextOutput
	var err error
	if root.Info.IsDir() {
		output, err = searchDirectory(ctx, t.base.validator, root, legacy)
	} else {
		output, err = searchSingleFile(ctx, root, legacy)
	}
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	matches := make([]searchMatch, 0, len(output.Matches))
	for _, m := range output.Matches {
		matches = append(matches, searchMatch(m))
	}
	// Apply the glob filters the rg engine would have applied; the other
	// filters have no fallback equivalent and must be disclosed in the note
	// instead of silently ignored.
	matches = filterMatchesByGlobs(matches, args.Glob)
	if len(matches) > args.HeadLimit {
		matches = matches[:args.HeadLimit]
		output.Truncated = true
	}
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, searchOutput{
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
		Note:            fallbackFilterNote(args),
	})
}

// matchSearchGlob mirrors rg --glob semantics closely enough for the
// fallback: a pattern without a slash matches the basename in any
// directory, a pattern with a slash matches the workspace-relative path.
func matchSearchGlob(pattern, relPath string) bool {
	if !strings.Contains(pattern, "/") {
		return matchGlobPath("**/"+pattern, relPath)
	}
	return matchGlobPath(pattern, relPath)
}

// filterMatchesByGlobs applies rg-style glob filters: positive patterns
// union-include, "!"-prefixed patterns exclude (exclusion wins).
func filterMatchesByGlobs(matches []searchMatch, globs []string) []searchMatch {
	var positives, negatives []string
	for _, g := range globs {
		if rest, ok := strings.CutPrefix(g, "!"); ok {
			negatives = append(negatives, rest)
		} else {
			positives = append(positives, g)
		}
	}
	if len(positives) == 0 && len(negatives) == 0 {
		return matches
	}
	out := make([]searchMatch, 0, len(matches))
	for _, m := range matches {
		excluded := false
		for _, n := range negatives {
			if matchSearchGlob(n, m.Path) {
				excluded = true
				break
			}
		}
		if excluded {
			continue
		}
		if len(positives) > 0 {
			included := false
			for _, p := range positives {
				if matchSearchGlob(p, m.Path) {
					included = true
					break
				}
			}
			if !included {
				continue
			}
		}
		out = append(out, m)
	}
	return out
}

// fallbackFilterNote discloses which search filters the Go fallback could
// not honor, so the model does not mistake an unfiltered result set for a
// filtered one.
func fallbackFilterNote(args searchArgs) string {
	var notes []string
	if !args.FixedStrings {
		notes = append(notes, "pattern matched as literal text by the go fallback engine (regex not supported)")
	}
	if args.Type != "" {
		notes = append(notes, fmt.Sprintf("type filter %q not applied by the go fallback engine", args.Type))
	}
	if !args.NoIgnore {
		notes = append(notes, ".gitignore rules not applied by the go fallback engine")
	}
	return strings.Join(notes, "; ")
}
