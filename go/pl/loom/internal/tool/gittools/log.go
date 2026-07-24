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
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
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
}

type gitLogCommit struct {
	Hash    string `json:"hash"`
	Author  string `json:"author"`
	Date    string `json:"date"`
	Subject string `json:"subject"`
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
func NewGitLogTool(validator *workspacepkg.PathValidator) (*GitLogTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "git_log",
		Description:  "Read recent commit history (hash, author, ISO date, subject) with a bounded limit. Optionally filter by file path.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"limit":{"type":"integer","minimum":1,"maximum":100},"path":{"type":"string","minLength":1}},"required":[]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string"},"limit":{"type":"integer"},"commits":{"type":"array"},"count":{"type":"integer"}},"required":["repo_root","limit","commits","count"]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &GitLogTool{base: base}, nil
}

func (t *GitLogTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *GitLogTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[gitLogArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, repoRoot, err := validateGitLogArgs(ctx, t.base.validator, t.base.gitPath, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read git log for %s (limit=%d)", args.RepoRoot, args.Limit)
	return t.base.prepareCall(ctx, call, canonical, []string{repoRoot.Absolute}, approvalDesc)
}

func (t *GitLogTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[gitLogArgs](prepared.Call.Arguments)
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

	argv := buildLogArgs(repoRoot.Absolute, args.Limit, args.Path)
	result, err := runGit(ctx, t.base.gitPath, argv, maxGitLogStdout, maxGitStderrBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git log"))
	}

	commits := parseLogOutput(result.stdout)
	return successResult(prepared.Call.ID, startedAt, gitLogOutput{
		RepoRoot: args.RepoRoot,
		Limit:    args.Limit,
		Commits:  commits,
		Count:    len(commits),
	})
}

func validateGitLogArgs(ctx context.Context, validator *workspacepkg.PathValidator, gitPath string, args gitLogArgs) (gitLogArgs, repoRootResolution, error) {
	if args.Limit == 0 {
		args.Limit = defaultGitLogLimit
	}
	if args.Limit < 0 || args.Limit > maxGitLogLimit {
		return gitLogArgs{}, repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("limit must be between 1 and %d", maxGitLogLimit))
	}
	repoRoot, err := resolveRepoRoot(validator, args.RepoRoot)
	if err != nil {
		return gitLogArgs{}, repoRootResolution{}, err
	}
	if args.Path != "" {
		if _, err := resolveRepoPath(validator, repoRoot, args.Path); err != nil {
			return gitLogArgs{}, repoRootResolution{}, err
		}
	}
	if err := confirmRepoRoot(ctx, gitPath, repoRoot); err != nil {
		return gitLogArgs{}, repoRootResolution{}, err
	}
	args.RepoRoot = repoRoot.Display
	return args, repoRoot, nil
}

func buildLogArgs(repoRoot string, limit int, repoRelativePath string) []string {
	args := []string{
		"--no-pager",
		"-c", "color.ui=false",
		"-c", "core.pager=cat",
		"-C", repoRoot,
		"log",
		"--format=%H%x09%an%x09%aI%x09%s",
		fmt.Sprintf("-n%d", limit),
	}
	if repoRelativePath != "" {
		args = append(args, "--", literalGitPathspec(repoRelativePath))
	}
	return args
}

func parseLogOutput(stdout []byte) []gitLogCommit {
	commits := []gitLogCommit{}
	for _, line := range strings.Split(strings.TrimRight(sanitizeUTF8(stdout), "\n"), "\n") {
		if line == "" {
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
