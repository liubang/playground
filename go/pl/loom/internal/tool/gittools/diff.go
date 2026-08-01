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
	"os"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type gitDiffArgs struct {
	RepoRoot         string `json:"repo_root"`
	Staged           bool   `json:"staged,omitempty"`
	Base             string `json:"base,omitempty"`
	Path             string `json:"path,omitempty"`
	Unified          *int   `json:"unified,omitempty"`
	IncludeUntracked bool   `json:"include_untracked,omitempty"`
}

type gitDiffOutput struct {
	RepoRoot         string   `json:"repo_root"`
	Staged           bool     `json:"staged"`
	Base             string   `json:"base,omitempty"`
	BaseRef          string   `json:"base_ref,omitempty"`
	Path             string   `json:"path,omitempty"`
	Unified          int      `json:"unified"`
	Diff             string   `json:"diff"`
	UntrackedFiles   []string `json:"untracked_files,omitempty"`
	UntrackedSkipped []string `json:"untracked_skipped,omitempty"`
	Truncated        bool     `json:"truncated"`
	SizeBytes        int      `json:"size_bytes"`
}

// GitDiffTool implements git_diff.
type GitDiffTool struct {
	base baseTool
}

// NewGitDiffTool creates a git_diff tool.
func NewGitDiffTool(validator *workspacepkg.PathValidator) (*GitDiffTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "git_diff",
		Description: "Read repository diff with bounded output. Default mode: working tree vs index (or the index vs HEAD " +
			"with staged=true). Set base to a commit/branch ref to diff that ref against the working tree; to " +
			"'merge-base:<branch>' to diff from the merge-base of HEAD and the branch — the right base for reviewing " +
			"everything a feature branch changed (when the branch's upstream is ahead, the upstream's fresher " +
			"merge-base is used and reported in base_ref); or to 'upstream' to diff against the branch's upstream " +
			"(falling back to the merge-base with the default branch) — the right base for reviewing everything " +
			"unpushed. Set include_untracked=true to fold untracked files into the diff as new-file entries " +
			"(binary/oversized files are skipped and reported in untracked_skipped). base cannot be combined with staged.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string","minLength":1},"staged":{"type":"boolean"},"base":{"type":"string","minLength":1,"maxLength":256},"path":{"type":"string","minLength":1},"unified":{"type":"integer","minimum":0,"maximum":20},"include_untracked":{"type":"boolean"}},"required":[]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"repo_root":{"type":"string"},"staged":{"type":"boolean"},"base":{"type":"string"},"base_ref":{"type":"string"},"path":{"type":"string"},"unified":{"type":"integer"},"diff":{"type":"string"},"untracked_files":{"type":"array","items":{"type":"string"}},"untracked_skipped":{"type":"array","items":{"type":"string"}},"truncated":{"type":"boolean"},"size_bytes":{"type":"integer"}},"required":["repo_root","staged","unified","diff","truncated","size_bytes"]}`),
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

// ConcurrentSafe implements domain.ConcurrentSafely: each invocation
// spawns an independent read-only git process.
func (t *GitDiffTool) ConcurrentSafe() bool { return true }

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

	diffBase, baseRef, err := resolveDiffBase(ctx, t.base.gitPath, repoRoot.Absolute, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	result, err := runGit(ctx, t.base.gitPath, buildDiffArgs(repoRoot.Absolute, args.Staged, requiredUnified(args), diffBase, repoRelativePath), maxGitDiffStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyGitError(err, result.stderr, "failed to read git diff"))
	}

	diffText := sanitizeUTF8(result.stdout)
	truncated := result.truncated
	sizeBytes := len(result.stdout)
	output := gitDiffOutput{
		RepoRoot:  args.RepoRoot,
		Staged:    args.Staged,
		Base:      args.Base,
		BaseRef:   baseRef,
		Path:      args.Path,
		Unified:   requiredUnified(args),
		Truncated: truncated,
		SizeBytes: sizeBytes,
	}
	if args.IncludeUntracked {
		extra, included, skipped, err := t.untrackedDiff(ctx, repoRoot, repoRelativePath, requiredUnified(args), int(maxGitDiffStdoutBytes)-sizeBytes)
		if err != nil {
			return errorResult(prepared.Call.ID, startedAt, err)
		}
		output.UntrackedFiles = included
		output.UntrackedSkipped = skipped
		if extra != "" {
			if diffText == "" {
				diffText = extra
			} else {
				diffText = strings.TrimSuffix(diffText, "\n") + "\n" + extra
			}
			sizeBytes += len(extra)
		}
	}
	output.Diff = diffText
	output.SizeBytes = sizeBytes
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
	if args.Base != "" && args.Staged {
		return gitDiffArgs{}, nil, domain.NewError(domain.ErrInvalidInput, "base cannot be combined with staged")
	}
	if args.IncludeUntracked && args.Staged {
		return gitDiffArgs{}, nil, domain.NewError(domain.ErrInvalidInput, "include_untracked cannot be combined with staged")
	}
	if args.Base != "" && !strings.HasPrefix(args.Base, mergeBasePrefix) && args.Base != upstreamBase {
		if err := verifyCommitRef(ctx, gitPath, repoRoot.Absolute, args.Base); err != nil {
			return gitDiffArgs{}, nil, err
		}
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

// mergeBasePrefix selects the merge-base mode of the base parameter.
const mergeBasePrefix = "merge-base:"

// upstreamBase selects the upstream mode of the base parameter: diff the
// working tree against the branch's upstream (or the default branch's
// merge-base), i.e. codex's git_diff_to_remote review base.
const upstreamBase = "upstream"

// resolveDiffBase maps the base parameter onto the ref passed to git diff:
// empty stays empty (working-tree/index modes), merge-base:<branch>
// resolves through resolveMergeBase, upstream resolves through
// resolveUpstreamDiffBase, and plain refs pass through after prepare-time
// verification. baseRef reports which ref a merge-base or upstream came
// from (the branch or its fresher upstream).
func resolveDiffBase(ctx context.Context, gitPath, repoRoot string, args gitDiffArgs) (string, string, error) {
	if args.Base == "" {
		return "", "", nil
	}
	if args.Base == upstreamBase {
		return resolveUpstreamDiffBase(ctx, gitPath, repoRoot)
	}
	if branch, ok := strings.CutPrefix(args.Base, mergeBasePrefix); ok {
		sha, usedRef, err := resolveMergeBase(ctx, gitPath, repoRoot, branch)
		if err != nil {
			return "", "", err
		}
		return sha, usedRef, nil
	}
	if err := verifyCommitRef(ctx, gitPath, repoRoot, args.Base); err != nil {
		return "", "", err
	}
	return args.Base, "", nil
}

// maxUntrackedFileBytes caps one synthesized untracked file so a single
// large new file cannot consume the whole diff budget.
const maxUntrackedFileBytes = 16 << 10

// untrackedDiff synthesizes new-file diff entries for untracked files,
// mirroring codex's git_diff_to_remote including untracked content so a
// "review my work" diff is complete. Files are read from disk (git cannot
// diff what it does not track); binary, oversized, sensitive, and
// budget-exceeding files are skipped and named in skipped. budget bounds
// the total synthesized bytes appended to the regular diff.
func (t *GitDiffTool) untrackedDiff(ctx context.Context, repoRoot repoRootResolution, repoRelativePath string, unified, budget int) (string, []string, []string, error) {
	_ = unified // synthesized entries are pure additions; context is meaningless.
	result, err := runGit(ctx, t.base.gitPath, buildLsFilesOthersArgs(repoRoot.Absolute), maxGitStatusStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return "", nil, nil, classifyGitError(err, result.stderr, "failed to list untracked files")
	}

	var b strings.Builder
	var included, skipped []string
	for _, raw := range bytes.Split(result.stdout, []byte{0}) {
		if len(raw) == 0 {
			continue
		}
		rel := sanitizeUTF8(raw)
		clean, display, err := normalizeStatusPath(repoRoot.Display, rel)
		if err != nil {
			skipped = append(skipped, rel+" (unsafe path)")
			continue
		}
		if repoRelativePath != "" && clean != repoRelativePath {
			continue
		}
		if budget <= 0 {
			skipped = append(skipped, display+" (diff budget exhausted)")
			continue
		}

		data, err := os.ReadFile(repoRoot.Absolute + string(os.PathSeparator) + strings.ReplaceAll(clean, "/", string(os.PathSeparator)))
		if err != nil {
			skipped = append(skipped, display+" (unreadable)")
			continue
		}
		switch {
		case isBinaryContent(data):
			skipped = append(skipped, display+" (binary)")
			continue
		case !utf8.Valid(data):
			skipped = append(skipped, display+" (not utf-8)")
			continue
		case len(data) > maxUntrackedFileBytes:
			skipped = append(skipped, fmt.Sprintf("%s (%d bytes > %d limit)", display, len(data), maxUntrackedFileBytes))
			continue
		}

		entry := synthesizeNewFileDiff(clean, string(data))
		if len(entry) > budget {
			skipped = append(skipped, display+" (diff budget exhausted)")
			continue
		}
		budget -= len(entry)
		b.WriteString(entry)
		included = append(included, display)
	}
	return b.String(), included, skipped, nil
}

// isBinaryContent approximates git's binary heuristic: a NUL byte in the
// first 8000 bytes marks the file binary.
func isBinaryContent(data []byte) bool {
	sample := data
	if len(sample) > 8000 {
		sample = sample[:8000]
	}
	return bytes.IndexByte(sample, 0) >= 0
}

// synthesizeNewFileDiff renders content as a unified new-file diff entry,
// byte-compatible with what git diff would emit for an added file.
func synthesizeNewFileDiff(path, content string) string {
	var b strings.Builder
	b.WriteString("diff --git a/" + path + " b/" + path + "\n")
	b.WriteString("new file mode 100644\n")
	b.WriteString("index 0000000..0000000\n")
	if content == "" {
		return b.String()
	}
	b.WriteString("--- /dev/null\n")
	b.WriteString("+++ b/" + path + "\n")

	lines := strings.SplitAfter(content, "\n")
	noEOL := !strings.HasSuffix(content, "\n")
	if !noEOL {
		lines = lines[:len(lines)-1] // SplitAfter leaves an empty tail after the final newline.
	}
	fmt.Fprintf(&b, "@@ -0,0 +1,%d @@\n", len(lines))
	for _, line := range lines {
		b.WriteString("+" + line)
		if !strings.HasSuffix(line, "\n") {
			b.WriteString("\n")
		}
	}
	if noEOL {
		b.WriteString("\\ No newline at end of file\n")
	}
	return b.String()
}
