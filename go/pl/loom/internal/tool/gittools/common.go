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
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultGitCommandTimeout        = 5 * time.Second
	defaultGitDiffUnified           = 3
	maxGitDiffUnified               = 20
	maxGitPathBytes                 = 4096
	maxGitStatusStdoutBytes   int64 = 1 << 20
	maxGitDiffStdoutBytes     int64 = 64 << 10
	maxGitRevParseStdoutBytes int64 = 4096
	maxGitStderrBytes         int64 = 16 << 10
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3) plus the git-specific
// dependencies: path validator, process runner (REVIEW A6) and the
// resolved git executable path.
type baseTool struct {
	toolkit.BaseTool
	validator *workspacepkg.PathValidator
	runner    *process.Runner
	gitPath   string
}

type repoRootResolution struct {
	Absolute string
	Display  string
}

type repoPathResolution struct {
	Absolute     string
	Display      string
	RepoRelative string
}

type gitRunResult struct {
	stdout    []byte
	stderr    []byte
	truncated bool
}

func newBaseTool(def domain.ToolDefinition, validator *workspacepkg.PathValidator, runner *process.Runner) (baseTool, error) {
	if validator == nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if runner == nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "process runner is required")
	}

	gitPath, err := exec.LookPath("git")
	if err != nil {
		return baseTool{}, domain.NewError(domain.ErrUnavailable, "git executable not found", domain.WithCause(err))
	}
	gitPath, err = filepath.Abs(gitPath)
	if err != nil {
		return baseTool{}, domain.NewError(domain.ErrInternal, "failed to normalize git executable path", domain.WithCause(err))
	}
	if resolved, err := filepath.EvalSymlinks(gitPath); err == nil {
		gitPath = resolved
	}

	bt, err := toolkit.NewBaseTool(def)
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{BaseTool: bt, validator: validator, runner: runner, gitPath: gitPath}, nil
}

func sortedUniqueStrings(values map[string]struct{}) []string {
	if len(values) == 0 {
		return []string{}
	}
	out := make([]string, 0, len(values))
	for value := range values {
		out = append(out, value)
	}
	sort.Strings(out)
	return out
}

func resolveRepoRoot(validator *workspacepkg.PathValidator, input string) (repoRootResolution, error) {
	if strings.TrimSpace(input) == "" {
		// repo_root is optional across git tools: default to the workspace root.
		input = "."
	}
	if len(input) > maxGitPathBytes {
		return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("repo_root exceeds %d bytes", maxGitPathBytes))
	}
	if rel, ok := lexicalWorkspaceRelativePath(validator, input); ok && containsSensitiveComponent(rel) {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root contains a sensitive component")
	}

	resolved, err := validator.Validate(input)
	if err != nil {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(resolved)
	if err != nil {
		if os.IsNotExist(err) {
			// Echo the offending path so the model can correct course
			// without guessing which repo_root was rejected.
			return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("repo_root does not exist: %q", domain.TruncateForErrorEcho(input)), domain.WithCause(err))
		}
		return repoRootResolution{}, domain.NewError(domain.ErrUnavailable, "failed to stat repo_root", domain.WithCause(err))
	}
	if !info.IsDir() {
		return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, "repo_root must refer to a directory")
	}

	rel, err := filepath.Rel(validator.Root(), resolved)
	if err != nil {
		return repoRootResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize repo_root", domain.WithCause(err))
	}
	if containsSensitiveComponent(rel) {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root contains a sensitive component")
	}
	return repoRootResolution{Absolute: resolved, Display: workspacepkg.DisplayPath(rel)}, nil
}

func resolveRepoPath(
	validator *workspacepkg.PathValidator,
	repoRoot repoRootResolution,
	input string,
) (repoPathResolution, error) {
	if strings.TrimSpace(input) == "" {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	if filepath.IsAbs(input) {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must be workspace-relative")
	}
	if len(input) > maxGitPathBytes {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path exceeds %d bytes", maxGitPathBytes))
	}
	if containsSensitiveComponent(input) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	resolved, err := validator.ResolveLexical(input)
	if err != nil {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid", domain.WithCause(err))
	}
	if !isUnderRoot(resolved.Absolute, repoRoot.Absolute) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes repository root")
	}

	repoRelative, err := filepath.Rel(repoRoot.Absolute, resolved.Absolute)
	if err != nil {
		return repoPathResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize repository path", domain.WithCause(err))
	}
	repoRelative = filepath.ToSlash(filepath.Clean(repoRelative))
	if containsSensitiveComponent(repoRelative) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}
	return repoPathResolution{
		Absolute:     resolved.Absolute,
		Display:      resolved.Display,
		RepoRelative: repoRelative,
	}, nil
}

func confirmRepoRoot(ctx context.Context, b *baseTool, repoRoot repoRootResolution) error {
	result, err := runGit(ctx, b, repoRoot.Absolute, buildRevParseArgs(repoRoot.Absolute), maxGitRevParseStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return classifyGitError(err, result.stderr, "failed to resolve git repository root")
	}
	if result.truncated {
		return domain.NewError(domain.ErrUnavailable, "git repository root output exceeded limit")
	}

	topLevel := strings.TrimSpace(toolkit.SanitizeUTF8(result.stdout))
	if topLevel == "" {
		return domain.NewError(domain.ErrUnavailable, "git repository root output was empty")
	}
	if resolved, err := filepath.EvalSymlinks(topLevel); err == nil {
		topLevel = resolved
	}
	if filepath.Clean(topLevel) != filepath.Clean(repoRoot.Absolute) {
		return domain.NewError(domain.ErrInvalidInput, "repo_root must be the repository root")
	}
	return nil
}

// gitBaseArgs is the shared prefix of every git invocation: no pager, no
// color, and — mirroring codex's git-utils operations.rs — hooks disabled
// via core.hooksPath=/dev/null so a repository's hooks can never execute
// in response to the agent's (read-only) git activity. The fsmonitor and
// untracked-cache features are disabled too: both can be armed through
// repo-local .git/config (core.fsmonitor names an arbitrary hook command
// that git status would execute). Since REVIEW A6 every git invocation
// additionally runs inside the process sandbox, so even a successfully
// injected hook stays confined to the workspace.
func gitBaseArgs(repoRoot string) []string {
	return []string{
		"--no-pager",
		"-c", "color.ui=false",
		"-c", "core.pager=cat",
		"-c", "core.hooksPath=/dev/null",
		"-c", "core.fsmonitor=false",
		"-c", "core.untrackedCache=false",
		"-C", repoRoot,
	}
}

func buildRevParseArgs(repoRoot string) []string {
	return append(
		gitBaseArgs(repoRoot),
		"rev-parse",
		"--show-toplevel",
	)
}

func buildStatusArgs(repoRoot string) []string {
	return append(
		gitBaseArgs(repoRoot),
		"status",
		"--porcelain=v2",
		"-z",
		"--branch",
	)
}

func buildDiffArgs(repoRoot string, staged bool, unified int, base, repoRelativePath string) []string {
	args := append(
		gitBaseArgs(repoRoot),
		"diff",
		"--no-ext-diff",
		"--no-textconv",
		fmt.Sprintf("--unified=%d", unified),
	)
	if staged {
		args = append(args, "--cached")
	}
	if base != "" {
		args = append(args, base)
	}
	if repoRelativePath != "" {
		args = append(args, "--", literalGitPathspec(repoRelativePath))
	}
	return args
}

func buildMergeBaseArgs(repoRoot, headRef, baseRef string) []string {
	return append(gitBaseArgs(repoRoot), "merge-base", headRef, baseRef)
}

func buildRevParseVerifyArgs(repoRoot, rev string) []string {
	return append(gitBaseArgs(repoRoot), "rev-parse", "--verify", "--quiet", rev)
}

func buildUpstreamArgs(repoRoot, branch string) []string {
	return append(gitBaseArgs(repoRoot), "rev-parse", "--abbrev-ref", "--symbolic-full-name", branch+"@{upstream}")
}

func buildRemoteGetURLArgs(repoRoot, remote string) []string {
	return append(gitBaseArgs(repoRoot), "remote", "get-url", remote)
}

func buildLsFilesOthersArgs(repoRoot string) []string {
	return append(gitBaseArgs(repoRoot), "ls-files", "--others", "--exclude-standard", "-z")
}

func buildRemoteHeadArgs(repoRoot, remote string) []string {
	return append(gitBaseArgs(repoRoot), "symbolic-ref", "--quiet", "--short", "refs/remotes/"+remote+"/HEAD")
}

// resolveUpstream returns the upstream ref of the given local ref
// ("" for HEAD), or "" when the ref has no upstream configured.
func resolveUpstream(ctx context.Context, b *baseTool, repoRoot, ref string) string {
	result, err := runGit(ctx, b, repoRoot, buildUpstreamArgs(repoRoot, ref), maxGitRevParseStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return ""
	}
	upstream := strings.TrimSpace(toolkit.SanitizeUTF8(result.stdout))
	if validateGitRef(upstream) != nil {
		return ""
	}
	return upstream
}

// resolveDefaultBranch finds the repository's default branch name,
// mirroring codex's git-utils info.rs default_branch_name fallback chain:
// the origin remote's symbolic HEAD first, then a local main/master probe.
// Returns "" when nothing resolves (a detached, remote-less repository).
func resolveDefaultBranch(ctx context.Context, b *baseTool, repoRoot string) string {
	if result, err := runGit(ctx, b, repoRoot, buildRemoteHeadArgs(repoRoot, "origin"), maxGitRevParseStdoutBytes, maxGitStderrBytes); err == nil {
		short := strings.TrimSpace(toolkit.SanitizeUTF8(result.stdout))
		if branch, ok := strings.CutPrefix(short, "origin/"); ok && branch != "" && validateGitRef(branch) == nil {
			return branch
		}
	}
	for _, candidate := range []string{"main", "master"} {
		if _, err := runGit(ctx, b, repoRoot, buildRevParseVerifyArgs(repoRoot, "refs/heads/"+candidate), maxGitRevParseStdoutBytes, maxGitStderrBytes); err == nil {
			return candidate
		}
	}
	return ""
}

// resolveUpstreamDiffBase picks the ref a "diff my work against the remote"
// review should start from, mirroring codex's git_diff_to_remote: the
// current branch's upstream when one exists, otherwise the merge-base with
// the default branch. Returns the base ref and a human-readable baseRef
// describing what was used; both empty when nothing resolves.
func resolveUpstreamDiffBase(ctx context.Context, b *baseTool, repoRoot string) (string, string, error) {
	if upstream := resolveUpstream(ctx, b, repoRoot, "HEAD"); upstream != "" {
		if err := verifyCommitRef(ctx, b, repoRoot, upstream); err != nil {
			return "", "", err
		}
		return upstream, upstream, nil
	}
	defaultBranch := resolveDefaultBranch(ctx, b, repoRoot)
	if defaultBranch == "" {
		return "", "", domain.NewError(domain.ErrInvalidInput, "no upstream or default branch to diff against (set an upstream or pass an explicit base)")
	}
	sha, usedRef, err := resolveMergeBase(ctx, b, repoRoot, defaultBranch)
	if err != nil {
		return "", "", err
	}
	return sha, mergeBasePrefix + usedRef, nil
}

// gitRefPattern is the whitelist for user-supplied refs and branch names:
// letters, digits, and the punctuation git itself allows in refnames. The
// leading-dash and ".." rejections keep refs from being interpreted as
// flags or revision ranges.
var gitRefPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/@+-]*$`)

// validateGitRef bounds a user-supplied ref/branch to git's refname
// alphabet, rejecting flag injection (leading '-') and range operators.
// Use this for refs that get concatenated into larger expressions
// ("<ref>@{upstream}", "<ref>..<ref>"); for standalone revision arguments
// use validateGitRevision instead.
func validateGitRef(ref string) error {
	if ref == "" {
		return domain.NewError(domain.ErrInvalidInput, "ref is required")
	}
	if len(ref) > 256 {
		return domain.NewError(domain.ErrInvalidInput, "ref exceeds 256 bytes")
	}
	if strings.Contains(ref, "..") {
		return domain.NewError(domain.ErrInvalidInput, "ref must not contain '..' (revision ranges are not supported)")
	}
	if !gitRefPattern.MatchString(ref) {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("ref %q is not a valid git refname (use a branch, tag, or commit SHA)", ref))
	}
	return nil
}

// gitRevisionPattern extends gitRefPattern with trailing ancestry operators
// (~N, ^N, digits optional, repeatable), matching the revision expressions
// models naturally produce (HEAD~3, abc123^, feature/foo~2^1). The base
// alphabet still excludes '~' and '^', so the suffix grammar is unambiguous.
var gitRevisionPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/@+-]*(?:[~^][0-9]*)*$`)

// validateGitRevision bounds a user-supplied revision to a refname plus
// optional ~N/^N ancestry suffixes. Unlike validateGitRef, the result must
// only be passed verbatim to git as a standalone revision argument (diff
// base, blame rev) — never concatenated into larger expressions like
// "<ref>@{upstream}" or "<ref>..<ref>", where a '~'/'^' suffix would
// corrupt the syntax. Leading-dash and '..' rejections still apply, and
// argv-based exec means '~'/'^' add no injection surface; resolvability is
// verified separately via rev-parse.
func validateGitRevision(rev string) error {
	if rev == "" {
		return domain.NewError(domain.ErrInvalidInput, "revision is required")
	}
	if len(rev) > 256 {
		return domain.NewError(domain.ErrInvalidInput, "revision exceeds 256 bytes")
	}
	if strings.Contains(rev, "..") {
		return domain.NewError(domain.ErrInvalidInput, "revision must not contain '..' (revision ranges are not supported)")
	}
	if !gitRevisionPattern.MatchString(rev) {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("revision %q is not valid (use a branch, tag, or commit SHA, optionally with ~N/^N ancestry suffixes)", rev))
	}
	return nil
}

func literalGitPathspec(path string) string {
	return ":(literal)" + path
}

// resolveMergeBase computes the merge-base of HEAD and branch, following
// codex's git-utils branch.rs: when the branch has an upstream that is
// ahead of the local ref, the upstream's merge-base is the fresher review
// base (the local branch is stale). Returns the base SHA and the ref that
// produced it (branch or its upstream), so callers can show their work.
func resolveMergeBase(ctx context.Context, b *baseTool, repoRoot, branch string) (string, string, error) {
	if err := validateGitRef(branch); err != nil {
		return "", "", err
	}

	usedRef := branch
	if upstreamResult, err := runGit(ctx, b, repoRoot, buildUpstreamArgs(repoRoot, branch), maxGitRevParseStdoutBytes, maxGitStderrBytes); err == nil {
		if upstream := strings.TrimSpace(toolkit.SanitizeUTF8(upstreamResult.stdout)); upstream != "" && upstream != branch && validateGitRef(upstream) == nil {
			countResult, countErr := runGit(ctx, b, repoRoot, append(gitBaseArgs(repoRoot), "rev-list", "--count", branch+".."+upstream), maxGitRevParseStdoutBytes, maxGitStderrBytes)
			if countErr == nil && strings.TrimSpace(toolkit.SanitizeUTF8(countResult.stdout)) != "0" {
				usedRef = upstream
			}
		}
	}

	result, err := runGit(ctx, b, repoRoot, buildMergeBaseArgs(repoRoot, "HEAD", usedRef), maxGitRevParseStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return "", "", classifyGitError(err, result.stderr, fmt.Sprintf("failed to compute merge-base of HEAD and %s", usedRef))
	}
	sha := strings.TrimSpace(toolkit.SanitizeUTF8(result.stdout))
	if sha == "" {
		return "", "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("no merge-base between HEAD and %s (unrelated histories?)", usedRef))
	}
	return sha, usedRef, nil
}

// verifyCommitRef confirms that rev resolves to a commit, so a diff base
// fails at prepare time with a clear error instead of inside git diff.
// Callers pass rev verbatim to git as a standalone revision argument, so
// ancestry suffixes (~N/^N) are accepted here via validateGitRevision.
func verifyCommitRef(ctx context.Context, b *baseTool, repoRoot, rev string) error {
	if err := validateGitRevision(rev); err != nil {
		return err
	}
	result, err := runGit(ctx, b, repoRoot, buildRevParseVerifyArgs(repoRoot, rev+"^{commit}"), maxGitRevParseStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("revision %q does not resolve to a commit", rev), domain.WithCause(err))
	}
	_ = result
	return nil
}

// gitExitError is the error returned when a git command exits non-zero
// through the process runner. The runner itself reports exit status in
// Result.ExitCode and returns a nil error for it (interpretWaitError), so
// runGit wraps that into an error here to keep the callers' error path
// (classifyGitError) unchanged.
type gitExitError struct {
	code int
}

func (e *gitExitError) Error() string {
	return fmt.Sprintf("git exited with code %d", e.code)
}

// runGit executes one git command through the shared process runner
// (REVIEW A6: sandboxed, process-group isolated, bounded output, and
// timeout handling are all the runner's, not a parallel implementation).
// The command's working directory is the repository root, and the -C flag
// in the built args redundantly points at the same directory so the
// argument builders stay repo-relative.
func runGit(ctx context.Context, b *baseTool, repoRoot string, args []string, stdoutLimit, stderrLimit int64) (gitRunResult, error) {
	outputLimit := stdoutLimit
	if stderrLimit > outputLimit {
		outputLimit = stderrLimit
	}
	result, err := b.runner.Run(ctx, process.CommandSpec{
		Program:     b.gitPath,
		Args:        args,
		Cwd:         repoRoot,
		Env:         gitCommandEnv,
		Timeout:     defaultGitCommandTimeout,
		OutputLimit: outputLimit,
	})
	gr := gitRunResult{
		stdout:    result.Stdout,
		stderr:    result.Stderr,
		truncated: result.Truncated,
	}
	if err != nil {
		return gr, err
	}
	switch {
	case result.TimedOut:
		return gr, context.DeadlineExceeded
	case result.Cancelled:
		return gr, context.Canceled
	case result.ExitCode != 0:
		return gr, &gitExitError{code: result.ExitCode}
	}
	return gr, nil
}

// gitCommandEnv is the environment every git invocation runs with: a
// stable locale, system and global configuration disabled (a malicious
// $HOME/.gitconfig or /etc/gitconfig must never arm hooks or aliases for
// the agent's read-only git activity), and credential prompts disabled
// (git must fail instead of hanging on a missing credential helper).
// The keys are on the process runner's env allowlist, so they survive the
// sandbox's minimal-environment filter (REVIEW A6).
var gitCommandEnv = map[string]string{
	"LANG":                "C",
	"LC_ALL":              "C",
	"GIT_CONFIG_NOSYSTEM": "1",
	"GIT_CONFIG_GLOBAL":   "/dev/null",
	"GIT_TERMINAL_PROMPT": "0",
}

func classifyGitError(err error, stderr []byte, fallback string) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
		return err
	}

	stderrText := strings.TrimSpace(toolkit.SanitizeUTF8(stderr))
	var gitErr *gitExitError
	if errors.As(err, &gitErr) {
		// git's own stderr is an external protocol, not our error text, so
		// there is no sentinel to match against (REVIEW N2). "not a git
		// repository" is stable git core output across versions, and the
		// match is deliberately narrow: everything else falls through to a
		// generic unavailable error carrying the stderr text.
		if strings.Contains(stderrText, "not a git repository") {
			return domain.NewError(domain.ErrInvalidInput, "repo_root is not a git repository root", domain.WithCause(err))
		}
		if stderrText == "" {
			stderrText = fallback
		}
		return domain.NewError(domain.ErrUnavailable, stderrText, domain.WithCause(err))
	}
	if stderrText == "" {
		stderrText = fallback
	}
	return domain.NewError(domain.ErrUnavailable, stderrText, domain.WithCause(err))
}

func lexicalWorkspaceRelativePath(validator *workspacepkg.PathValidator, input string) (string, bool) {
	clean := filepath.Clean(input)
	if !filepath.IsAbs(clean) {
		return clean, true
	}

	root := filepath.Clean(validator.Root())
	candidate := filepath.Clean(clean)
	if candidate != root && !strings.HasPrefix(candidate, root+string(filepath.Separator)) {
		return "", false
	}
	rel, err := filepath.Rel(root, candidate)
	if err != nil {
		return "", false
	}
	return rel, true
}

// containsSensitiveComponent delegates to the single canonical list in the
// workspace package (REVIEW R4).
func containsSensitiveComponent(path string) bool {
	return workspacepkg.ContainsSensitiveComponent(path)
}

func repoPathDisplay(repoRootDisplay, repoRelative string) string {
	clean := filepath.Clean(filepath.FromSlash(repoRelative))
	if clean == "." || clean == string(filepath.Separator) {
		return repoRootDisplay
	}
	if repoRootDisplay == "." {
		return filepath.ToSlash(clean)
	}
	return filepath.ToSlash(filepath.Join(repoRootDisplay, clean))
}

func isUnderRoot(path, root string) bool {
	normalized := filepath.Clean(path)
	rootNorm := filepath.Clean(root)
	if normalized == rootNorm {
		return true
	}
	return strings.HasPrefix(normalized, rootNorm+string(filepath.Separator))
}
