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
// Created: 2026/08/01

package gittools

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

const (
	// maxBlameLines caps one blame window; blame more by paging start_line,
	// mirroring read_file's pagination model.
	maxBlameLines          = 2000
	maxGitBlameStdoutBytes = 1 << 20
)

type gitBlameArgs struct {
	RepoRoot  string `json:"repo_root"`
	Path      string `json:"path"`
	Rev       string `json:"rev,omitempty"`
	StartLine int    `json:"start_line,omitempty"`
	EndLine   int    `json:"end_line,omitempty"`
}

type gitBlameEntry struct {
	Line        int    `json:"line"`
	Commit      string `json:"commit"`
	Author      string `json:"author"`
	Date        string `json:"date"`
	Uncommitted bool   `json:"uncommitted,omitempty"`
}

type gitBlameOutput struct {
	RepoRoot  string          `json:"repo_root"`
	Path      string          `json:"path"`
	Rev       string          `json:"rev,omitempty"`
	StartLine int             `json:"start_line"`
	EndLine   int             `json:"end_line"`
	Entries   []gitBlameEntry `json:"entries"`
	Truncated bool            `json:"truncated"`
}

// GitBlameTool implements git_blame.
type GitBlameTool struct {
	base baseTool
}

// NewGitBlameTool creates a git_blame tool.
func NewGitBlameTool(validator *workspacepkg.PathValidator) (*GitBlameTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "git_blame",
		Description: "Read line-by-line authorship attribution for a tracked file (who last touched each line and in " +
			"which commit), the way `git blame` answers \"where did this code come from\". Entries carry line number, " +
			"short commit, author, and ISO date — not line content; pair with read_file for the code itself. " +
			"Uncommitted working-tree lines are marked uncommitted with an all-zero commit. Results are windowed " +
			"(max 2000 lines): page with start_line/end_line. Set rev to a commit SHA, branch, or tag — optionally " +
			"with ~N/^N ancestry suffixes (e.g. HEAD~3) — to blame a historical revision instead of the working tree.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"path":{"type":"string","minLength":1},"rev":{"type":"string","minLength":1,"maxLength":256},"start_line":{"type":"integer","minimum":1},"end_line":{"type":"integer","minimum":1}},"required":["path"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string"},"path":{"type":"string"},"rev":{"type":"string"},"start_line":{"type":"integer"},"end_line":{"type":"integer"},"entries":{"type":"array","items":{"type":"object","properties":{"line":{"type":"integer"},"commit":{"type":"string"},"author":{"type":"string"},"date":{"type":"string"},"uncommitted":{"type":"boolean"}},"required":["line","commit","author","date"]}},"truncated":{"type":"boolean"}},"required":["repo_root","path","start_line","end_line","entries","truncated"]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &GitBlameTool{base: base}, nil
}

func (t *GitBlameTool) Definition() domain.ToolDefinition {
	return t.base.def
}

// ConcurrentSafe implements domain.ConcurrentSafely: each invocation
// spawns an independent read-only git process.
func (t *GitBlameTool) ConcurrentSafe() bool { return true }

func (t *GitBlameTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[gitBlameArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, readPaths, err := validateGitBlameArgs(ctx, t.base.validator, t.base.gitPath, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read git blame for %s lines %d-%d", args.Path, args.StartLine, args.EndLine)
	return t.base.prepareCall(ctx, call, canonical, readPaths, approvalDesc)
}

func (t *GitBlameTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 2 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[gitBlameArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	repoRoot, err := resolveRepoRoot(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if repoRoot.Display != args.RepoRoot {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call repo_root binding mismatch"))
	}
	if err := confirmRepoRoot(ctx, t.base.gitPath, repoRoot); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, err := resolveRepoPath(t.base.validator, repoRoot, args.Path)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if prepared.ReadPaths[1] != pathInfo.Absolute {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	result, err := runGit(ctx, t.base.gitPath, buildBlameArgs(repoRoot.Absolute, args.Rev, args.StartLine, args.EndLine, pathInfo.RepoRelative), maxGitBlameStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git blame"))
	}

	entries, err := parseBlamePorcelain(result.stdout)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	return successResult(prepared.Call.ID, startedAt, gitBlameOutput{
		RepoRoot:  args.RepoRoot,
		Path:      args.Path,
		Rev:       args.Rev,
		StartLine: args.StartLine,
		EndLine:   args.EndLine,
		Entries:   entries,
		Truncated: result.truncated,
	})
}

func validateGitBlameArgs(
	ctx context.Context,
	validator *workspacepkg.PathValidator,
	gitPath string,
	args gitBlameArgs,
) (gitBlameArgs, []string, error) {
	repoRoot, err := resolveRepoRoot(validator, args.RepoRoot)
	if err != nil {
		return gitBlameArgs{}, nil, err
	}
	if err := confirmRepoRoot(ctx, gitPath, repoRoot); err != nil {
		return gitBlameArgs{}, nil, err
	}
	if args.Rev != "" {
		if err := verifyCommitRef(ctx, gitPath, repoRoot.Absolute, args.Rev); err != nil {
			return gitBlameArgs{}, nil, err
		}
	}
	if args.StartLine <= 0 {
		args.StartLine = 1
	}
	if args.EndLine == 0 {
		args.EndLine = args.StartLine + maxBlameLines - 1
	}
	if args.EndLine < args.StartLine {
		return gitBlameArgs{}, nil, domain.NewError(domain.ErrInvalidInput, "end_line must be >= start_line")
	}
	if args.EndLine-args.StartLine+1 > maxBlameLines {
		return gitBlameArgs{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("line window must be at most %d lines", maxBlameLines))
	}

	pathInfo, err := resolveRepoPath(validator, repoRoot, args.Path)
	if err != nil {
		return gitBlameArgs{}, nil, err
	}
	args.RepoRoot = repoRoot.Display
	args.Path = pathInfo.Display
	return args, []string{repoRoot.Absolute, pathInfo.Absolute}, nil
}

func buildBlameArgs(repoRoot, rev string, startLine, endLine int, repoRelativePath string) []string {
	args := append(
		gitBaseArgs(repoRoot),
		"blame",
		"--porcelain",
		"-L", fmt.Sprintf("%d,%d", startLine, endLine),
	)
	if rev != "" {
		args = append(args, rev)
	}
	// Unlike git diff, blame resolves the path inside the revision's tree,
	// where pathspec magic (e.g. ":(literal)") is not understood — pass the
	// already-validated repo-relative path verbatim.
	return append(args, "--", repoRelativePath)
}

// blameCommit caches the metadata of one commit the porcelain stream
// described in full (metadata repeats only on a commit's first entry).
type blameCommit struct {
	author string
	time   int64
}

// parseBlamePorcelain condenses `git blame --porcelain` into per-line
// entries. The stream is header line + optional metadata block + a
// tab-prefixed content line per blamed line; a commit's metadata ships only
// with its FIRST entry, so later entries of the same commit are filled from
// a cache. Content lines are dropped (read_file owns content) and only
// attribution is kept.
func parseBlamePorcelain(data []byte) ([]gitBlameEntry, error) {
	text := sanitizeUTF8(data)
	entries := make([]gitBlameEntry, 0, 256)
	commits := make(map[string]blameCommit)

	var pending *gitBlameEntry
	var pendingSHA string
	flush := func() {
		if pending != nil {
			entries = append(entries, *pending)
			pending = nil
		}
	}

	for _, line := range strings.Split(text, "\n") {
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "\t") {
			flush()
			continue
		}
		fields := strings.Fields(line)
		if len(fields) >= 3 && isHexSHA(fields[0]) {
			flush()
			finalLine, err := strconv.Atoi(fields[2])
			if err != nil {
				return nil, domain.NewError(domain.ErrUnavailable, "failed to parse git blame line number", domain.WithCause(err))
			}
			sha := fields[0]
			pendingSHA = sha
			entry := gitBlameEntry{Line: finalLine, Commit: shortSHA(sha)}
			if commit, ok := commits[sha]; ok {
				entry.Author = commit.author
				entry.Date = formatBlameTime(commit.time)
			}
			if isZeroSHA(sha) {
				entry.Uncommitted = true
			}
			pending = &entry
			continue
		}
		if pending == nil {
			continue
		}
		key, value, ok := strings.Cut(line, " ")
		if !ok {
			continue
		}
		cached := commits[pendingSHA]
		switch key {
		case "author":
			cached.author = value
			pending.Author = value
		case "author-time":
			if unix, err := strconv.ParseInt(value, 10, 64); err == nil {
				cached.time = unix
				pending.Date = formatBlameTime(unix)
			}
		default:
			continue
		}
		commits[pendingSHA] = cached
	}
	flush()
	return entries, nil
}

func isHexSHA(s string) bool {
	if len(s) != 40 {
		return false
	}
	for _, c := range s {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return false
		}
	}
	return true
}

func isZeroSHA(s string) bool {
	return strings.Trim(s, "0") == ""
}

func shortSHA(s string) string {
	if len(s) > 8 {
		return s[:8]
	}
	return s
}

func formatBlameTime(unix int64) string {
	if unix <= 0 {
		return ""
	}
	return time.Unix(unix, 0).UTC().Format(time.RFC3339)
}
