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

package gittools

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type gitDiffArgs struct {
	RepoRoot string `json:"repo_root"`
	Staged   bool   `json:"staged,omitempty"`
	Path     string `json:"path,omitempty"`
	Unified  *int   `json:"unified,omitempty"`
}

type gitDiffOutput struct {
	RepoRoot  string `json:"repo_root"`
	Staged    bool   `json:"staged"`
	Path      string `json:"path,omitempty"`
	Unified   int    `json:"unified"`
	Diff      string `json:"diff"`
	Truncated bool   `json:"truncated"`
	SizeBytes int    `json:"size_bytes"`
}

// GitDiffTool implements git_diff.
type GitDiffTool struct {
	base baseTool
}

// NewGitDiffTool creates a git_diff tool.
func NewGitDiffTool(validator *workspacepkg.PathValidator) (*GitDiffTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "git_diff",
		Description:  "Read repository diff of the working tree or the index (staged) with bounded output.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"staged":{"type":"boolean"},"path":{"type":"string","minLength":1},"unified":{"type":"integer","minimum":0,"maximum":20}},"required":[]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string"},"staged":{"type":"boolean"},"path":{"type":"string"},"unified":{"type":"integer"},"diff":{"type":"string"},"truncated":{"type":"boolean"},"size_bytes":{"type":"integer"}},"required":["repo_root","staged","unified","diff","truncated","size_bytes"]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &GitDiffTool{base: base}, nil
}

func (t *GitDiffTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *GitDiffTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[gitDiffArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, readPaths, err := validateGitDiffArgs(ctx, t.base.validator, t.base.gitPath, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read git diff for %s", args.RepoRoot)
	if args.Path != "" {
		approvalDesc = fmt.Sprintf("Read git diff for %s limited to %s", args.RepoRoot, args.Path)
	}
	return t.base.prepareCall(ctx, call, canonical, readPaths, approvalDesc)
}

func (t *GitDiffTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) < 1 || len(prepared.ReadPaths) > 2 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[gitDiffArgs](prepared.Call.Arguments)
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

	repoRelativePath := ""
	if args.Path != "" {
		if len(prepared.ReadPaths) != 2 {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding is missing"))
		}
		pathInfo, err := resolveRepoPath(t.base.validator, repoRoot, args.Path)
		if err != nil {
			return errorResult(prepared.Call.ID, startedAt, err)
		}
		if prepared.ReadPaths[1] != pathInfo.Absolute {
			return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
		}
		repoRelativePath = pathInfo.RepoRelative
	} else if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding is invalid"))
	}

	result, err := runGit(ctx, t.base.gitPath, buildDiffArgs(repoRoot.Absolute, args.Staged, requiredUnified(args), repoRelativePath), maxGitDiffStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git diff"))
	}
	output := gitDiffOutput{
		RepoRoot:  args.RepoRoot,
		Staged:    args.Staged,
		Path:      args.Path,
		Unified:   requiredUnified(args),
		Diff:      sanitizeUTF8(result.stdout),
		Truncated: result.truncated,
		SizeBytes: len(result.stdout),
	}
	return successResult(prepared.Call.ID, startedAt, output)
}

func validateGitDiffArgs(
	ctx context.Context,
	validator *workspacepkg.PathValidator,
	gitPath string,
	args gitDiffArgs,
) (gitDiffArgs, []string, error) {
	repoRoot, err := resolveRepoRoot(validator, args.RepoRoot)
	if err != nil {
		return gitDiffArgs{}, nil, err
	}
	if err := confirmRepoRoot(ctx, gitPath, repoRoot); err != nil {
		return gitDiffArgs{}, nil, err
	}
	if args.Unified == nil {
		args.Unified = intPtr(defaultGitDiffUnified)
	}
	if *args.Unified < 0 || *args.Unified > maxGitDiffUnified {
		return gitDiffArgs{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unified must be between 0 and %d", maxGitDiffUnified))
	}

	args.RepoRoot = repoRoot.Display
	readPaths := []string{repoRoot.Absolute}
	if args.Path != "" {
		pathInfo, err := resolveRepoPath(validator, repoRoot, args.Path)
		if err != nil {
			return gitDiffArgs{}, nil, err
		}
		args.Path = pathInfo.Display
		readPaths = append(readPaths, pathInfo.Absolute)
	}
	return args, readPaths, nil
}

func requiredUnified(args gitDiffArgs) int {
	if args.Unified == nil {
		return defaultGitDiffUnified
	}
	return *args.Unified
}

func intPtr(value int) *int {
	return &value
}
