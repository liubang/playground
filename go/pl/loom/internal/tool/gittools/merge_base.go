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

package gittools

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type gitMergeBaseArgs struct {
	RepoRoot string `json:"repo_root"`
	Branch   string `json:"branch"`
}

type gitMergeBaseOutput struct {
	RepoRoot  string `json:"repo_root"`
	Branch    string `json:"branch"`
	MergeBase string `json:"merge_base"`
	BaseRef   string `json:"base_ref"`
}

// GitMergeBaseTool implements git_merge_base (codex git-utils branch.rs).
type GitMergeBaseTool struct {
	base baseTool
}

// NewGitMergeBaseTool creates a git_merge_base tool.
func NewGitMergeBaseTool(validator *workspacepkg.PathValidator, runner *process.Runner) (*GitMergeBaseTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "git_merge_base",
		Description: "Compute the merge-base commit of HEAD and a branch — the correct base for reviewing everything " +
			"a feature branch changed or for a three-way comparison. When the branch's upstream is ahead of the " +
			"local ref, the upstream's fresher merge-base is used and reported in base_ref.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"branch":{"type":"string","minLength":1,"maxLength":256}},"required":["branch"]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator, runner)
	if err != nil {
		return nil, err
	}
	return &GitMergeBaseTool{base: base}, nil
}

func (t *GitMergeBaseTool) Definition() domain.ToolDefinition {
	return t.base.Def
}

// ConcurrentSafe implements domain.ConcurrentSafely: each invocation
// spawns independent read-only git processes.
func (t *GitMergeBaseTool) ConcurrentSafe() bool { return true }

func (t *GitMergeBaseTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := toolkit.DecodeStrict[gitMergeBaseArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	repoRoot, err := resolveRepoRoot(t.base.validator, args.RepoRoot)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if err := confirmRepoRoot(ctx, &t.base, repoRoot); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := validateGitRef(args.Branch); err != nil {
		return domain.PreparedCall{}, err
	}
	args.RepoRoot = repoRoot.Display

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Compute merge-base of HEAD and %s in %s", args.Branch, args.RepoRoot)
	return t.base.PrepareCall(ctx, call, canonical, toolkit.PrepareOptions{ReadPaths: []string{repoRoot.Absolute}, ApprovalDesc: approvalDesc})
}

func (t *GitMergeBaseTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.VerifyPreparedCall(prepared); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := toolkit.DecodeStrict[gitMergeBaseArgs](prepared.Call.Arguments)
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
	if err := confirmRepoRoot(ctx, &t.base, repoRoot); err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}

	sha, usedRef, err := resolveMergeBase(ctx, &t.base, repoRoot.Absolute, args.Branch)
	if err != nil {
		return toolkit.ErrorResult(prepared.Call.ID, startedAt, err)
	}
	return toolkit.SuccessResult(prepared.Call.ID, startedAt, gitMergeBaseOutput{
		RepoRoot:  args.RepoRoot,
		Branch:    args.Branch,
		MergeBase: sha,
		BaseRef:   usedRef,
	})
}
