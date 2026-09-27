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

package gittools

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultGitLogLimit = 20
	maxGitLogLimit     = 100
	maxGitLogStdout    = 256 << 10
)

type gitLogArgs struct {
	RepoRoot string `json:"repo_root,omitempty"`
	Limit    int    `json:"limit,omitempty"`
	Path     string `json:"path,omitempty"`
	Stat     bool   `json:"stat,omitempty"`
}

// gitLogFileStat is one changed file of a commit (numstat form). Binary
// files carry no line counts from git; they are flagged instead.
type gitLogFileStat struct {
	Path    string `json:"path"`
	Added   int    `json:"added"`
	Deleted int    `json:"deleted"`
	Binary  bool   `json:"binary,omitempty"`
}

type gitLogCommit struct {
	Hash    string           `json:"hash"`
	Author  string           `json:"author"`
	Date    string           `json:"date"`
	Subject string           `json:"subject"`
	Files   []gitLogFileStat `json:"files,omitempty"`
}

type gitLogOutput struct {
	RepoRoot string         `json:"repo_root"`
	Limit    int            `json:"limit"`
	Commits  []gitLogCommit `json:"commits"`
	Count    int            `json:"count"`
}

// GitLogTool implements bounded read-only commit history.
type GitLogTool struct {
	base baseTool
}

// NewGitLogTool creates a git_log tool.
func NewGitLogTool(validator *workspacepkg.PathValidator, runner *process.Runner) (*GitLogTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "git_log",
		Description: "Read recent commit history (hash, author, ISO date, subject) with a bounded limit. " +
			"Set stat=true to also list each commit's changed files with added/deleted line counts (git log --numstat " +
			"form) — use this to learn WHAT a commit changed instead of falling back to run_cmd. Optionally filter by file path.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"limit":{"type":"integer","minimum":1,"maximum":100},"path":{"type":"string","minLength":1},"stat":{"type":"boolean","description":"Include per-commit changed files with added/deleted line counts. Default false."}},"required":[]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator, runner)
	if err != nil {
		return nil, err
	}
	return &GitLogTool{base: base}, nil
}

func (t *GitLogTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: each invocation
// spawns an independent read-only git process.
func (t *GitLogTool) ConcurrentSafe() bool { return true }

func (t *GitLogTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeLenient[gitLogArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, readPaths, err := validateGitLogArgs(ctx, &t.base, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read git log for %s (limit=%d)", args.RepoRoot, args.Limit)
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ReadPaths: readPaths, ApprovalDesc: approvalDesc})
}

func (t *GitLogTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) < 1 || len(prepared.ReadPaths) > 2 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := toolkit.DecodeStrict[gitLogArgs](prepared.Call.Arguments)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	repoRoot, err := resolveRepoRoot(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if repoRoot.Display != args.RepoRoot {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call repo_root binding mismatch"))
	}

	// git log interprets the pathspec relative to the repo root (git -C
	// repoRoot), so the workspace-relative display path would silently match
	// nothing whenever repo_root is a subdirectory — resolve to the
	// repo-relative form the way git_blame/git_diff do.
	repoRelativePath := ""
	if args.Path != "" {
		if len(prepared.ReadPaths) != 2 {
			return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding is invalid"))
		}
		pathInfo, err := resolveRepoPath(t.base.validator, repoRoot, args.Path)
		if err != nil {
			return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
		}
		if prepared.ReadPaths[1] != pathInfo.Absolute {
			return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
		}
		repoRelativePath = pathInfo.RepoRelative
	} else if len(prepared.ReadPaths) != 1 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding is invalid"))
	}

	argv := buildLogArgs(repoRoot.Absolute, args.Limit, repoRelativePath, args.Stat)
	result, err := runGit(ctx, &t.base, repoRoot.Absolute, argv, maxGitLogStdout, maxGitStderrBytes)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git log"))
	}

	commits := parseLogOutput(result.stdout, args.Stat)
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, gitLogOutput{
		RepoRoot: args.RepoRoot,
		Limit:    args.Limit,
		Commits:  commits,
		Count:    len(commits),
	})
}

func validateGitLogArgs(ctx context.Context, b *baseTool, args gitLogArgs) (gitLogArgs, []string, error) {
	args.RepoRoot = normalizeNullStringArg(args.RepoRoot)
	args.Path = normalizeNullStringArg(args.Path)
	if args.Limit == 0 {
		args.Limit = defaultGitLogLimit
	}
	if args.Limit < 0 || args.Limit > maxGitLogLimit {
		return gitLogArgs{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("limit must be between 1 and %d", maxGitLogLimit))
	}
	repoRoot, err := resolveRepoRoot(b.validator, args.RepoRoot)
	if err != nil {
		return gitLogArgs{}, nil, err
	}
	if err := confirmRepoRoot(ctx, b, repoRoot); err != nil {
		return gitLogArgs{}, nil, err
	}
	args.RepoRoot = repoRoot.Display
	readPaths := []string{repoRoot.Absolute}
	if args.Path != "" {
		pathInfo, err := resolveRepoPath(b.validator, repoRoot, args.Path)
		if err != nil {
			return gitLogArgs{}, nil, err
		}
		args.Path = pathInfo.Display
		readPaths = append(readPaths, pathInfo.Absolute)
	}
	return args, readPaths, nil
}

func buildLogArgs(repoRoot string, limit int, repoRelativePath string, stat bool) []string {
	// In stat mode the header line ends with a record separator so the
	// parser can tell it apart from numstat lines unconditionally.
	headerFormat := "--format=%H%x09%an%x09%aI%x09%s"
	if stat {
		headerFormat += "%x1e"
	}
	args := append(
		gitBaseArgs(repoRoot),
		"log",
		headerFormat,
		fmt.Sprintf("-n%d", limit),
	)
	if stat {
		args = append(args, "--numstat")
	}
	if repoRelativePath != "" {
		args = append(args, "--", literalGitPathspec(repoRelativePath))
	}
	return args
}

func parseLogOutput(stdout []byte, stat bool) []gitLogCommit {
	commits := []gitLogCommit{}
	for _, line := range strings.Split(strings.TrimRight(toolkit.SanitizeUTF8(stdout), "\n"), "\n") {
		if line == "" {
			continue
		}
		if stat {
			if commit, ok := parseLogHeaderLine(line); ok {
				commits = append(commits, commit)
				continue
			}
			// A numstat line belongs to the commit that precedes it.
			if len(commits) == 0 {
				continue
			}
			if fileStat, ok := parseNumstatLine(line); ok {
				commits[len(commits)-1].Files = append(commits[len(commits)-1].Files, fileStat)
			}
			continue
		}
		fields := strings.SplitN(line, "\t", 4)
		if len(fields) != 4 {
			continue
		}
		commits = append(commits, gitLogCommit{
			Hash:    fields[0],
			Author:  fields[1],
			Date:    fields[2],
			Subject: fields[3],
		})
	}
	return commits
}

// parseLogHeaderLine parses a stat-mode header line (fields terminated by
// the \x1e record separator).
func parseLogHeaderLine(line string) (gitLogCommit, bool) {
	header, ok := strings.CutSuffix(line, "\x1e")
	if !ok {
		return gitLogCommit{}, false
	}
	fields := strings.SplitN(header, "\t", 4)
	if len(fields) != 4 {
		return gitLogCommit{}, false
	}
	return gitLogCommit{Hash: fields[0], Author: fields[1], Date: fields[2], Subject: fields[3]}, true
}

// parseNumstatLine parses one "added<TAB>deleted<TAB>path" line; binary
// files report "-" for both counts.
func parseNumstatLine(line string) (gitLogFileStat, bool) {
	fields := strings.SplitN(line, "\t", 3)
	if len(fields) != 3 || fields[2] == "" {
		return gitLogFileStat{}, false
	}
	stat := gitLogFileStat{Path: fields[2]}
	if fields[0] == "-" || fields[1] == "-" {
		stat.Binary = true
		return stat, true
	}
	added, errA := strconv.Atoi(fields[0])
	deleted, errD := strconv.Atoi(fields[1])
	if errA != nil || errD != nil {
		return gitLogFileStat{}, false
	}
	stat.Added, stat.Deleted = added, deleted
	return stat, true
}
