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
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type gitStatusArgs struct {
	RepoRoot string `json:"repo_root"`
}

type gitStatusOutput struct {
	RepoRoot  string   `json:"repo_root"`
	Branch    string   `json:"branch"`
	Head      string   `json:"head"`
	Ahead     int      `json:"ahead"`
	Behind    int      `json:"behind"`
	Staged    []string `json:"staged"`
	Unstaged  []string `json:"unstaged"`
	Untracked []string `json:"untracked"`
}

// GitStatusTool implements git_status.
type GitStatusTool struct {
	base baseTool
}

// NewGitStatusTool creates a git_status tool.
func NewGitStatusTool(validator *workspacepkg.PathValidator) (*GitStatusTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name:         "git_status",
		Description:  "Read repository status (branch, head, ahead/behind counts, staged/unstaged/untracked files) using git status porcelain v2 with deterministic JSON output.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1}},"required":[]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string"},"branch":{"type":"string"},"head":{"type":"string"},"ahead":{"type":"integer"},"behind":{"type":"integer"},"staged":{"type":"array","items":{"type":"string"}},"unstaged":{"type":"array","items":{"type":"string"}},"untracked":{"type":"array","items":{"type":"string"}}},"required":["repo_root","branch","head","ahead","behind","staged","unstaged","untracked"]}`),
		Capabilities: []domain.Capability{domain.CapGitRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &GitStatusTool{base: base}, nil
}

func (t *GitStatusTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *GitStatusTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[gitStatusArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, repoRoot, err := validateGitStatusArgs(ctx, t.base.validator, t.base.gitPath, args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Read git status for %s", args.RepoRoot)
	return t.base.prepareCall(ctx, call, canonical, []string{repoRoot.Absolute}, approvalDesc)
}

func (t *GitStatusTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[gitStatusArgs](prepared.Call.Arguments)
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

	result, err := runGit(ctx, t.base.gitPath, buildStatusArgs(repoRoot.Absolute), maxGitStatusStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git status"))
	}
	if result.truncated {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable, "git status output exceeded limit"))
	}

	output, err := parseGitStatusOutput(repoRoot.Display, result.stdout)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	return successResult(prepared.Call.ID, startedAt, output)
}

func validateGitStatusArgs(
	ctx context.Context,
	validator *workspacepkg.PathValidator,
	gitPath string,
	args gitStatusArgs,
) (gitStatusArgs, repoRootResolution, error) {
	repoRoot, err := resolveRepoRoot(validator, args.RepoRoot)
	if err != nil {
		return gitStatusArgs{}, repoRootResolution{}, err
	}
	if err := confirmRepoRoot(ctx, gitPath, repoRoot); err != nil {
		return gitStatusArgs{}, repoRootResolution{}, err
	}
	args.RepoRoot = repoRoot.Display
	return args, repoRoot, nil
}

func parseGitStatusOutput(repoRootDisplay string, data []byte) (gitStatusOutput, error) {
	parts := bytes.Split(data, []byte{0})
	output := gitStatusOutput{
		RepoRoot:  repoRootDisplay,
		Staged:    []string{},
		Unstaged:  []string{},
		Untracked: []string{},
	}
	stagedSet := make(map[string]struct{})
	unstagedSet := make(map[string]struct{})
	untrackedSet := make(map[string]struct{})

	for i := 0; i < len(parts); i++ {
		part := parts[i]
		if len(part) == 0 {
			continue
		}
		line := sanitizeUTF8(part)
		switch {
		case strings.HasPrefix(line, "# branch.head "):
			head := strings.TrimSpace(strings.TrimPrefix(line, "# branch.head "))
			if head == "(detached)" {
				output.Branch = ""
			} else {
				output.Branch = head
			}
		case strings.HasPrefix(line, "# branch.oid "):
			oid := strings.TrimSpace(strings.TrimPrefix(line, "# branch.oid "))
			if oid == "(initial)" {
				oid = ""
			}
			output.Head = oid
		case strings.HasPrefix(line, "# branch.ab "):
			fields := strings.Fields(strings.TrimSpace(strings.TrimPrefix(line, "# branch.ab ")))
			for _, field := range fields {
				switch {
				case strings.HasPrefix(field, "+"):
					value, err := strconv.Atoi(strings.TrimPrefix(field, "+"))
					if err != nil {
						return gitStatusOutput{}, domain.NewError(domain.ErrUnavailable, "failed to parse branch ahead count", domain.WithCause(err))
					}
					output.Ahead = value
				case strings.HasPrefix(field, "-"):
					value, err := strconv.Atoi(strings.TrimPrefix(field, "-"))
					if err != nil {
						return gitStatusOutput{}, domain.NewError(domain.ErrUnavailable, "failed to parse branch behind count", domain.WithCause(err))
					}
					output.Behind = value
				}
			}
		case strings.HasPrefix(line, "1 "), strings.HasPrefix(line, "2 "), strings.HasPrefix(line, "u "):
			xy, pathText, nextIndex, err := parseTrackedStatusEntry(parts, i, line)
			if err != nil {
				return gitStatusOutput{}, err
			}
			i = nextIndex
			_, displayPath, err := normalizeStatusPath(repoRootDisplay, pathText)
			if err != nil {
				return gitStatusOutput{}, err
			}
			if isChangedStatus(xy[0]) {
				stagedSet[displayPath] = struct{}{}
			}
			if isChangedStatus(xy[1]) {
				unstagedSet[displayPath] = struct{}{}
			}
		case strings.HasPrefix(line, "? "):
			pathText := strings.TrimSpace(strings.TrimPrefix(line, "? "))
			_, displayPath, err := normalizeStatusPath(repoRootDisplay, pathText)
			if err != nil {
				return gitStatusOutput{}, err
			}
			untrackedSet[displayPath] = struct{}{}
		case strings.HasPrefix(line, "! "):
			continue
		}
	}

	output.Staged = sortedUniqueStrings(stagedSet)
	output.Unstaged = sortedUniqueStrings(unstagedSet)
	output.Untracked = sortedUniqueStrings(untrackedSet)
	return output, nil
}

func parseTrackedStatusEntry(parts [][]byte, index int, line string) (string, string, int, error) {
	switch {
	case strings.HasPrefix(line, "1 "):
		fields := strings.SplitN(line, " ", 9)
		if len(fields) < 9 {
			return "", "", index, domain.NewError(domain.ErrUnavailable, "failed to parse tracked git status entry")
		}
		return validateStatusCode(fields[1]), fields[8], index, nil
	case strings.HasPrefix(line, "2 "):
		fields := strings.SplitN(line, " ", 9)
		if len(fields) < 9 {
			return "", "", index, domain.NewError(domain.ErrUnavailable, "failed to parse rename git status entry")
		}
		renameParts := strings.SplitN(fields[8], " ", 2)
		if len(renameParts) < 2 {
			return "", "", index, domain.NewError(domain.ErrUnavailable, "failed to parse rename git status target")
		}
		if index+1 >= len(parts) || len(parts[index+1]) == 0 {
			return "", "", index, domain.NewError(domain.ErrUnavailable, "failed to parse rename git status source")
		}
		return validateStatusCode(fields[1]), renameParts[1], index + 1, nil
	case strings.HasPrefix(line, "u "):
		fields := strings.SplitN(line, " ", 11)
		if len(fields) < 11 {
			return "", "", index, domain.NewError(domain.ErrUnavailable, "failed to parse unmerged git status entry")
		}
		return validateStatusCode(fields[1]), fields[10], index, nil
	default:
		return "", "", index, domain.NewError(domain.ErrUnavailable, "unsupported git status entry")
	}
}

func validateStatusCode(xy string) string {
	if len(xy) != 2 {
		return "??"
	}
	return xy
}

func isChangedStatus(code byte) bool {
	return code != '.' && code != ' '
}

func normalizeStatusPath(repoRootDisplay, raw string) (string, string, error) {
	clean := filepath.ToSlash(filepath.Clean(filepath.FromSlash(raw)))
	if clean == "." || strings.HasPrefix(clean, "../") || clean == ".." {
		return "", "", domain.NewError(domain.ErrSecurity, "git status path escapes repository root")
	}
	if containsSensitiveComponent(clean) {
		return "", "", domain.NewError(domain.ErrSecurity, "git status path contains a sensitive component")
	}
	return clean, repoPathDisplay(repoRootDisplay, clean), nil
}
