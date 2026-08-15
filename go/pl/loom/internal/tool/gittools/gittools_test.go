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
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestGitStatusToolClassifiesChanges(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\nworktree\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "staged.txt"), []byte("staged\n"))
	gitRun(t, repoRoot, "add", "staged.txt")
	mustWriteFile(t, filepath.Join(repoRoot, "mixed.txt"), []byte("v1\n"))
	gitRun(t, repoRoot, "add", "mixed.txt")
	mustWriteFile(t, filepath.Join(repoRoot, "mixed.txt"), []byte("v2\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "untracked.txt"), []byte("u\n"))

	tool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{RepoRoot: filepath.Join(workspaceRoot, "repo")}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output gitStatusOutput
	decodeToolResult(t, result, &output)
	if output.RepoRoot != "repo" {
		t.Fatalf("output.RepoRoot = %q, want repo", output.RepoRoot)
	}
	if output.Branch != currentBranchName(t, repoRoot) {
		t.Fatalf("output.Branch = %q, want current branch", output.Branch)
	}
	if len(output.Head) != 40 {
		t.Fatalf("output.Head = %q, want 40-char commit hash", output.Head)
	}
	if output.Ahead != 0 || output.Behind != 0 {
		t.Fatalf("ahead/behind = %d/%d, want 0/0", output.Ahead, output.Behind)
	}
	assertStringSliceEqual(t, output.Staged, []string{"repo/mixed.txt", "repo/staged.txt"})
	assertStringSliceEqual(t, output.Unstaged, []string{"repo/mixed.txt", "repo/tracked.txt"})
	assertStringSliceEqual(t, output.Untracked, []string{"repo/untracked.txt"})
}

func TestGitDiffToolSupportsStagedAndPath(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("one\ntwo\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other\n"))
	gitRun(t, repoRoot, "add", "tracked.txt", "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("one\nTWO\nthree\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other changed\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}

	unstagedPrepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/other.txt",
		Unified:  intPtr(1),
	}))
	if err != nil {
		t.Fatalf("Prepare(unstaged) error = %v", err)
	}
	unstagedResult := tool.Execute(context.Background(), unstagedPrepared)
	if unstagedResult.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute(unstaged) status = %s, want success: %+v", unstagedResult.Status, unstagedResult.Error)
	}
	var unstagedOutput gitDiffOutput
	decodeToolResult(t, unstagedResult, &unstagedOutput)
	if unstagedOutput.Path != "repo/other.txt" {
		t.Fatalf("unstaged path = %q, want repo/other.txt", unstagedOutput.Path)
	}
	if unstagedOutput.Unified != 1 || unstagedOutput.Staged {
		t.Fatalf("unexpected unstaged diff options: %+v", unstagedOutput)
	}
	if !strings.Contains(unstagedOutput.Diff, "diff --git a/other.txt b/other.txt") || !strings.Contains(unstagedOutput.Diff, "+other changed") {
		t.Fatalf("unexpected unstaged diff:\n%s", unstagedOutput.Diff)
	}
	if strings.Contains(unstagedOutput.Diff, "tracked.txt") {
		t.Fatalf("path-limited diff should not include tracked.txt:\n%s", unstagedOutput.Diff)
	}
	if unstagedOutput.Truncated {
		t.Fatal("did not expect unstaged diff truncation")
	}
	if unstagedOutput.SizeBytes != len(unstagedOutput.Diff) {
		t.Fatalf("unstaged size_bytes = %d, want %d", unstagedOutput.SizeBytes, len(unstagedOutput.Diff))
	}

	stagedPrepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Staged:   true,
		Path:     "repo/tracked.txt",
		Unified:  intPtr(0),
	}))
	if err != nil {
		t.Fatalf("Prepare(staged) error = %v", err)
	}
	stagedResult := tool.Execute(context.Background(), stagedPrepared)
	if stagedResult.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute(staged) status = %s, want success: %+v", stagedResult.Status, stagedResult.Error)
	}
	var stagedOutput gitDiffOutput
	decodeToolResult(t, stagedResult, &stagedOutput)
	if !stagedOutput.Staged {
		t.Fatal("expected staged diff output")
	}
	if stagedOutput.Unified != 0 {
		t.Fatalf("staged unified = %d, want 0", stagedOutput.Unified)
	}
	if !strings.Contains(stagedOutput.Diff, "diff --git a/tracked.txt b/tracked.txt") || !strings.Contains(stagedOutput.Diff, "+three") {
		t.Fatalf("unexpected staged diff:\n%s", stagedOutput.Diff)
	}
	if strings.Contains(stagedOutput.Diff, "other.txt") {
		t.Fatalf("staged path-limited diff should not include other.txt:\n%s", stagedOutput.Diff)
	}
}

func TestGitToolsRejectTamperingAndEscapes(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	statusTool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	diffTool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}

	_, err = statusTool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "git_status",
		Arguments: json.RawMessage(`{"repo_root":"repo","extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, err = diffTool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "../outside.txt",
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	_, err = diffTool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/.git/config",
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	prepared, err := diffTool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/tracked.txt",
	}))
	if err != nil {
		t.Fatalf("Prepare(valid diff) error = %v", err)
	}
	prepared.Call.Arguments = mustMarshalRaw(t, gitDiffArgs{
		RepoRoot: "repo",
		Staged:   true,
		Path:     "repo/tracked.txt",
		Unified:  intPtr(defaultGitDiffUnified),
	})
	tampered := diffTool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)

	statusPrepared, err := statusTool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{RepoRoot: filepath.Join(workspaceRoot, "repo")}))
	if err != nil {
		t.Fatalf("Prepare(valid status) error = %v", err)
	}
	statusPrepared.ReadPaths = []string{filepath.Join(workspaceRoot, "repo"), filepath.Join(workspaceRoot, "repo", "tracked.txt")}
	statusTampered := statusTool.Execute(context.Background(), statusPrepared)
	assertToolResultError(t, statusTampered, domain.ToolStatusError, domain.ErrSecurity)
}

func TestGitDiffToolRejectsPathspecMagicExpansion(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	literalMagic := ":(glob)*.txt"
	mustWriteFile(t, filepath.Join(repoRoot, literalMagic), []byte("one\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("two\n"))
	gitRun(t, repoRoot, "add", literalMagic, "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, literalMagic), []byte("ONE\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("TWO\n"))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     filepath.ToSlash(filepath.Join("repo", literalMagic)),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	expectedPath, err := validator.ResolveLexical(filepath.ToSlash(filepath.Join("repo", literalMagic)))
	if err != nil {
		t.Fatalf("ResolveLexical() error = %v", err)
	}
	if got := prepared.ReadPaths; len(got) != 2 || got[1] != expectedPath.Absolute {
		t.Fatalf("prepared.ReadPaths = %v, want binding to %q", got, expectedPath.Absolute)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if !strings.Contains(output.Diff, "diff --git a/"+literalMagic+" b/"+literalMagic) {
		t.Fatalf("literal magic diff missing target file:\n%s", output.Diff)
	}
	if strings.Contains(output.Diff, "other.txt") {
		t.Fatalf("pathspec magic must not expand to other.txt:\n%s", output.Diff)
	}
	if output.Path != filepath.ToSlash(filepath.Join("repo", literalMagic)) {
		t.Fatalf("output.Path = %q, want literal magic path", output.Path)
	}
	if output.Truncated {
		t.Fatal("did not expect truncation for literal diff")
	}
}

func TestGitDiffToolSupportsLiteralGlobCharacters(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	literalName := "star[1]?.txt"
	mustWriteFile(t, filepath.Join(repoRoot, literalName), []byte("before\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other\n"))
	gitRun(t, repoRoot, "add", literalName, "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, literalName), []byte("after\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other changed\n"))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     filepath.ToSlash(filepath.Join("repo", literalName)),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if !strings.Contains(output.Diff, "diff --git a/"+literalName+" b/"+literalName) {
		t.Fatalf("literal diff missing target file:\n%s", output.Diff)
	}
	if strings.Contains(output.Diff, "other.txt") {
		t.Fatalf("literal path diff should not include other.txt:\n%s", output.Diff)
	}
}

func TestGitDiffToolTruncatesLargeOutput(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	base := strings.Repeat("base line for large diff\n", 5000)
	changed := strings.Repeat("changed line for large diff\n", 5000)
	mustWriteFile(t, filepath.Join(repoRoot, "large.txt"), []byte(base))
	gitRun(t, repoRoot, "add", "large.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")
	mustWriteFile(t, filepath.Join(repoRoot, "large.txt"), []byte(changed))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/large.txt",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if !output.Truncated {
		t.Fatal("expected truncated diff output")
	}
	if output.SizeBytes != int(maxGitDiffStdoutBytes) {
		t.Fatalf("size_bytes = %d, want %d", output.SizeBytes, int(maxGitDiffStdoutBytes))
	}
	if len(output.Diff) != int(maxGitDiffStdoutBytes) {
		t.Fatalf("diff length = %d, want %d", len(output.Diff), int(maxGitDiffStdoutBytes))
	}
}

func ensureGitAvailable(t *testing.T) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skipf("git not available: %v", err)
	}
}

func TestGitLogToolReturnsRecentCommits(t *testing.T) {
	validator, runner, _, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "first commit")
	mustWriteFile(t, filepath.Join(repoRoot, "b.txt"), []byte("two\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "second commit")

	tool, err := NewGitLogTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitLogTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_log", gitLogArgs{RepoRoot: "repo", Limit: 10}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output gitLogOutput
	decodeToolResult(t, result, &output)
	if output.Count != 2 {
		t.Fatalf("output.Count = %d, want 2", output.Count)
	}
	if output.Commits[0].Subject != "second commit" || output.Commits[1].Subject != "first commit" {
		t.Fatalf("commit order = %v, want newest first", []string{output.Commits[0].Subject, output.Commits[1].Subject})
	}
	if output.Commits[0].Hash == "" || output.Commits[0].Author != "Loom Test" || output.Commits[0].Date == "" {
		t.Fatalf("incomplete commit fields: %+v", output.Commits[0])
	}
}

// The path filter must be resolved relative to repo_root: git log runs with
// -C repoRoot, so a workspace-relative path ("repo/a.txt") matches nothing
// when the repo lives in a workspace subdirectory (REVIEW H8).
func TestGitLogToolPathFilterWithSubdirRepoRoot(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, _, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "touch a")
	mustWriteFile(t, filepath.Join(repoRoot, "b.txt"), []byte("two\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "touch b")

	tool, err := NewGitLogTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitLogTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_log", gitLogArgs{RepoRoot: "repo", Limit: 10, Path: "repo/a.txt"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output gitLogOutput
	decodeToolResult(t, result, &output)
	if output.Count != 1 || output.Commits[0].Subject != "touch a" {
		t.Fatalf("path filter = %+v, want only the 'touch a' commit", output)
	}
}

// repo_root and limit are optional: empty values default to "." (workspace
// root) and 20 respectively.
func TestGitToolsDefaultRepoRootAndLimit(t *testing.T) {
	validator, runner, workspaceRoot, _ := newGitValidator(t)
	configureGitRepo(t, workspaceRoot)
	mustWriteFile(t, filepath.Join(workspaceRoot, "a.txt"), []byte("one\n"))
	gitRun(t, workspaceRoot, "add", ".")
	gitRun(t, workspaceRoot, "commit", "-m", "initial")

	statusTool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := statusTool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{}))
	if err != nil {
		t.Fatalf("git_status Prepare() with empty repo_root error = %v", err)
	}
	result := statusTool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("git_status Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	logTool, err := NewGitLogTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitLogTool() error = %v", err)
	}
	prepared, err = logTool.Prepare(context.Background(), newToolCall(t, "git_log", gitLogArgs{}))
	if err != nil {
		t.Fatalf("git_log Prepare() with empty args error = %v", err)
	}
	var canonical gitLogArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &canonical); err != nil {
		t.Fatal(err)
	}
	if canonical.RepoRoot != "." || canonical.Limit != 20 {
		t.Fatalf("canonical defaults = %+v, want repo_root=. limit=20", canonical)
	}
	result = logTool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("git_log Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
}

// A missing repo_root must be named in the error so the model can
// correct course without guessing.
func TestResolveRepoRootErrorNamesPath(t *testing.T) {
	validator, _, _, _ := newGitValidator(t)
	_, err := resolveRepoRoot(validator, "no/such/repo")
	if err == nil || !strings.Contains(err.Error(), `repo_root does not exist: "no/such/repo"`) {
		t.Fatalf("error = %v, want the offending path named", err)
	}
}

func newGitValidator(t *testing.T) (*workspacepkg.PathValidator, *process.Runner, string, string) {
	t.Helper()
	workspaceRoot := filepath.Join(t.TempDir(), "workspace")
	repoRoot := filepath.Join(workspaceRoot, "repo")
	mustMkdirAll(t, repoRoot)
	validator, err := workspacepkg.NewPathValidator(workspaceRoot)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	// Tests exercise the runner path with the explicit test sandbox
	// (process-group isolation, cross-platform): git tool behavior is what
	// is under test, not the seatbelt profile.
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.ExplicitTestSandbox{},
	})
	if err != nil {
		t.Fatalf("NewRunner() error = %v", err)
	}
	return validator, runner, workspaceRoot, repoRoot
}

func configureGitRepo(t *testing.T, repoRoot string) {
	t.Helper()
	gitRun(t, repoRoot, "init")
	gitRun(t, repoRoot, "config", "user.email", "loom@example.com")
	gitRun(t, repoRoot, "config", "user.name", "Loom Test")
}

func currentBranchName(t *testing.T, repoRoot string) string {
	t.Helper()
	output := gitOutput(t, repoRoot, "branch", "--show-current")
	return strings.TrimSpace(output)
}

func gitRun(t *testing.T, repoRoot string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", repoRoot}, args...)...)
	cmd.Env = append([]string{"LANG=C", "LC_ALL=C"}, os.Environ()...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v failed: %v\n%s", args, err, output)
	}
}

func gitOutput(t *testing.T, repoRoot string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", repoRoot}, args...)...)
	cmd.Env = append([]string{"LANG=C", "LC_ALL=C"}, os.Environ()...)
	output, err := cmd.Output()
	if err != nil {
		t.Fatalf("git %v failed: %v", args, err)
	}
	return string(output)
}

func newToolCall[T any](t *testing.T, name string, args T) domain.ToolCall {
	t.Helper()
	return domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      name,
		Arguments: mustMarshalRaw(t, args),
	}
}

func mustMarshalRaw[T any](t *testing.T, value T) json.RawMessage {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return data
}

func decodeToolResult(t *testing.T, result domain.ToolResult, out any) {
	t.Helper()
	if len(result.Content) != 1 {
		t.Fatalf("len(result.Content) = %d, want 1", len(result.Content))
	}
	if result.Content[0].Kind != domain.PartText {
		t.Fatalf("result.Content[0].Kind = %s, want text", result.Content[0].Kind)
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), out); err != nil {
		t.Fatalf("json.Unmarshal(tool result) error = %v, payload=%s", err, result.Content[0].Text)
	}
}

func assertToolResultError(t *testing.T, result domain.ToolResult, wantStatus domain.ToolStatus, wantCode domain.ErrorCode) {
	t.Helper()
	if result.Status != wantStatus {
		t.Fatalf("result.Status = %s, want %s", result.Status, wantStatus)
	}
	if result.Error == nil {
		t.Fatal("expected structured tool error")
	}
	if result.Error.Code != string(wantCode) {
		t.Fatalf("result.Error.Code = %q, want %q", result.Error.Code, wantCode)
	}
}

func assertAgentErrorCode(t *testing.T, err error, want domain.ErrorCode) {
	t.Helper()
	if err == nil {
		t.Fatal("expected error")
	}
	var agentErr *domain.AgentError
	if !errors.As(err, &agentErr) {
		t.Fatalf("expected AgentError, got %T: %v", err, err)
	}
	if agentErr.Code != want {
		t.Fatalf("agentErr.Code = %s, want %s", agentErr.Code, want)
	}
}

func assertStringSliceEqual(t *testing.T, got, want []string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("len(got) = %d, want %d; got=%v want=%v", len(got), len(want), got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got[%d] = %q, want %q; got=%v want=%v", i, got[i], want[i], got, want)
		}
	}
}

func mustWriteFile(t *testing.T, path string, data []byte) {
	t.Helper()
	mustMkdirAll(t, filepath.Dir(path))
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("os.WriteFile(%q) error = %v", path, err)
	}
}

func mustMkdirAll(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(path, 0o755); err != nil {
		t.Fatalf("os.MkdirAll(%q) error = %v", path, err)
	}
}

func TestGitMergeBaseToolReturnsAncestralCommit(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "base commit")

	initialSHA := strings.TrimSpace(gitOutput(t, repoRoot, "rev-parse", "HEAD"))
	defaultBranch := currentBranchName(t, repoRoot)

	// Create a feature branch and add a commit.
	gitRun(t, repoRoot, "checkout", "-b", "feature")
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\nfeature change\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "feature commit")

	// Go back to default branch and add an independent commit.
	gitRun(t, repoRoot, "checkout", defaultBranch)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\nmain change\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "main commit")

	tool, err := NewGitMergeBaseTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitMergeBaseTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_merge_base", gitMergeBaseArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Branch:   "feature",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output gitMergeBaseOutput
	decodeToolResult(t, result, &output)
	if output.MergeBase != initialSHA {
		t.Fatalf("merge_base = %q, want initial commit %q", output.MergeBase, initialSHA)
	}
	if output.Branch != "feature" {
		t.Fatalf("branch = %q, want feature", output.Branch)
	}
	if output.BaseRef != "feature" {
		t.Fatalf("base_ref = %q, want feature (no upstream)", output.BaseRef)
	}
}

func TestGitDiffWithBaseRef(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("original\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "initial")

	// Modify working tree — default diff shows working-tree vs index.
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("original\nmodified\n"))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}

	// Diff against HEAD should show the same change as working-tree vs HEAD.
	headSHA := strings.TrimSpace(gitOutput(t, repoRoot, "rev-parse", "HEAD"))
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Base:     headSHA,
	}))
	if err != nil {
		t.Fatalf("Prepare() with base error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if !strings.Contains(output.Diff, "modified") {
		t.Fatalf("diff with base=HEAD should contain 'modified', got:\n%s", output.Diff)
	}
}

func TestGitDiffWithMergeBasePrefix(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "base")

	defaultBranch2 := currentBranchName(t, repoRoot)
	gitRun(t, repoRoot, "checkout", "-b", "feature")
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\nfeature\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "feature")

	gitRun(t, repoRoot, "checkout", defaultBranch2)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\nmain\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "main")

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Base:     "merge-base:feature",
	}))
	if err != nil {
		t.Fatalf("Prepare() with merge-base: error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if output.Base != "merge-base:feature" {
		t.Fatalf("output.Base = %q, want merge-base:feature", output.Base)
	}
}

func TestGitDiffBaseAndStagedMutuallyExclusive(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "init")

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Staged:   true,
		Base:     "HEAD",
	}))
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
}

func TestValidateGitRefRejectsDangerousInput(t *testing.T) {
	tests := []struct {
		name string
		ref  string
		ok   bool
	}{
		{"normal branch", "feature/foo", true},
		{"remote ref", "origin/main", true},
		{"leading dash", "-e", false},
		{"double dot range", "main..feature", false},
		{"shell metachar", "foo;rm", false},
		{"empty", "", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validateGitRef(tt.ref)
			if tt.ok && err != nil {
				t.Fatalf("validateGitRef(%q) unexpected error: %v", tt.ref, err)
			}
			if !tt.ok && err == nil {
				t.Fatalf("validateGitRef(%q) expected error, got nil", tt.ref)
			}
		})
	}
}

func TestValidateGitRevisionAcceptsAncestryOperators(t *testing.T) {
	tests := []struct {
		name string
		rev  string
		ok   bool
	}{
		{"plain branch", "feature/foo", true},
		{"remote ref", "origin/main", true},
		{"full sha", "afd9cebedeceab9f4492b48a41002400f140101e", true},
		{"head tilde", "HEAD~3", true},
		{"short sha tilde", "3f9a8d4f9~1", true},
		{"bare tilde", "HEAD~", true},
		{"head caret", "HEAD^", true},
		{"caret with digits", "abc123^2", true},
		{"chained ancestry", "feature/foo~2^1", true},
		{"tag peel", "v1.0^0", true},
		{"leading dash", "-e", false},
		{"double dot range", "main..feature", false},
		{"triple dot range", "main...feature", false},
		{"shell metachar", "foo;rm", false},
		{"leading tilde", "~1", false},
		{"upstream brace syntax", "@{upstream}", false},
		{"rev-parse peel syntax", "HEAD^{commit}", false},
		{"reflog syntax", "HEAD@{2}", false},
		{"empty", "", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validateGitRevision(tt.rev)
			if tt.ok && err != nil {
				t.Fatalf("validateGitRevision(%q) unexpected error: %v", tt.rev, err)
			}
			if !tt.ok && err == nil {
				t.Fatalf("validateGitRevision(%q) expected error, got nil", tt.rev)
			}
		})
	}
}

func TestGitDiffWithAncestryBaseRef(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("original\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "initial")

	initialSHA := strings.TrimSpace(gitOutput(t, repoRoot, "rev-parse", "HEAD"))

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("original\nsecond\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "second")

	// Uncommitted working-tree change on top of the second commit.
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("original\nsecond\nmodified\n"))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}

	// HEAD~1 diffs the working tree against the initial commit: both the
	// committed "second" line and the uncommitted "modified" line show up.
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Base:     "HEAD~1",
	}))
	if err != nil {
		t.Fatalf("Prepare() with ancestry base error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if output.Base != "HEAD~1" {
		t.Fatalf("output.Base = %q, want HEAD~1", output.Base)
	}
	if !strings.Contains(output.Diff, "+second") || !strings.Contains(output.Diff, "+modified") {
		t.Fatalf("diff with base=HEAD~1 should contain '+second' and '+modified', got:\n%s", output.Diff)
	}

	// A short SHA with an ancestry suffix — the exact shape that failed
	// before revision syntax was accepted — resolves to the same base.
	shortBase := initialSHA[:9] + "~0"
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Base:     shortBase,
	}))
	if err != nil {
		t.Fatalf("Prepare() with short-sha ancestry base error = %v", err)
	}
	result = tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	decodeToolResult(t, result, &output)
	if !strings.Contains(output.Diff, "+second") || !strings.Contains(output.Diff, "+modified") {
		t.Fatalf("diff with base=%q should contain '+second' and '+modified', got:\n%s", shortBase, output.Diff)
	}
}

func TestGitStatusReportsRemoteURL(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "init")
	// Add a fake remote — local-only path so no network is needed.
	fakeRemote := t.TempDir()
	gitRun(t, repoRoot, "remote", "add", "origin", fakeRemote)

	tool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitStatusOutput
	decodeToolResult(t, result, &output)
	if output.RemoteURL != fakeRemote {
		t.Fatalf("RemoteURL = %q, want %q", output.RemoteURL, fakeRemote)
	}
}

// setupRepoWithUpstream builds a repo whose branch tracks a local bare
// remote, so upstream-dependent behavior can be tested without a network.
func setupRepoWithUpstream(t *testing.T) (*workspacepkg.PathValidator, *process.Runner, string, string) {
	t.Helper()
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	gitRun(t, repoRoot, "checkout", "-b", "main")

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "init")

	bare := filepath.Join(t.TempDir(), "origin.git")
	gitRun(t, repoRoot, "init", "--bare", bare)
	gitRun(t, repoRoot, "remote", "add", "origin", bare)
	gitRun(t, repoRoot, "push", "-u", "origin", "main")
	// origin/HEAD -> main, so default-branch resolution exercises the
	// symbolic-ref path.
	gitRun(t, repoRoot, "remote", "set-head", "origin", "main")
	return validator, runner, workspaceRoot, repoRoot
}

func TestGitStatusReportsUpstreamAndDefaultBranch(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, _ := setupRepoWithUpstream(t)

	tool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitStatusOutput
	decodeToolResult(t, result, &output)
	if output.Upstream != "origin/main" {
		t.Fatalf("Upstream = %q, want origin/main", output.Upstream)
	}
	if output.DefaultBranch != "main" {
		t.Fatalf("DefaultBranch = %q, want main", output.DefaultBranch)
	}
}

func TestGitDiffToolUpstreamBase(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := setupRepoWithUpstream(t)

	// A pushed commit (invisible to the upstream diff) plus an unpushed
	// working-tree change (visible).
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\npushed\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "pushed commit")
	gitRun(t, repoRoot, "push")
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("base\npushed\nunpushed\n"))

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Base:     "upstream",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if output.BaseRef != "origin/main" {
		t.Fatalf("BaseRef = %q, want origin/main", output.BaseRef)
	}
	if !strings.Contains(output.Diff, "+unpushed") {
		t.Fatalf("diff missing unpushed change: %q", output.Diff)
	}
	if strings.Contains(output.Diff, "+pushed") {
		t.Fatalf("diff should not contain the pushed change: %q", output.Diff)
	}
}

func TestGitDiffToolIncludeUntracked(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "init")
	mustWriteFile(t, filepath.Join(repoRoot, "new.txt"), []byte("hello\nworld\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "bin.dat"), []byte{'a', 0, 'b'})

	tool, err := NewGitDiffTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitDiffTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_diff", gitDiffArgs{
		RepoRoot:         filepath.Join(workspaceRoot, "repo"),
		IncludeUntracked: true,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitDiffOutput
	decodeToolResult(t, result, &output)
	if len(output.UntrackedFiles) != 1 || output.UntrackedFiles[0] != "repo/new.txt" {
		t.Fatalf("UntrackedFiles = %v, want [repo/new.txt]", output.UntrackedFiles)
	}
	if !strings.Contains(output.Diff, "new file mode 100644") || !strings.Contains(output.Diff, "+hello") {
		t.Fatalf("diff missing synthesized untracked entry: %q", output.Diff)
	}
	if len(output.UntrackedSkipped) != 1 || !strings.Contains(output.UntrackedSkipped[0], "bin.dat") {
		t.Fatalf("UntrackedSkipped = %v, want bin.dat (binary)", output.UntrackedSkipped)
	}
}

func TestGitDiffToolSynthesizedUntrackedNoEOL(t *testing.T) {
	entry := synthesizeNewFileDiff("f.txt", "one\ntwo")
	want := "diff --git a/f.txt b/f.txt\nnew file mode 100644\nindex 0000000..0000000\n--- /dev/null\n+++ b/f.txt\n@@ -0,0 +1,2 @@\n+one\n+two\n\\ No newline at end of file\n"
	if entry != want {
		t.Fatalf("synthesized diff =\n%q\nwant\n%q", entry, want)
	}
}

func TestGitBlameToolAttributesLines(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one\ntwo\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "first")
	firstSHA := strings.TrimSpace(gitOutput(t, repoRoot, "rev-parse", "HEAD"))

	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one\ntwo changed\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "second")
	secondSHA := strings.TrimSpace(gitOutput(t, repoRoot, "rev-parse", "HEAD"))

	// An uncommitted working-tree edit to line 1.
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one dirty\ntwo changed\n"))

	tool, err := NewGitBlameTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitBlameTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_blame", gitBlameArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/a.txt",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output gitBlameOutput
	decodeToolResult(t, result, &output)
	if len(output.Entries) != 2 {
		t.Fatalf("len(Entries) = %d, want 2: %+v", len(output.Entries), output.Entries)
	}
	if !output.Entries[0].Uncommitted {
		t.Fatalf("Entries[0] = %+v, want uncommitted working-tree line", output.Entries[0])
	}
	if output.Entries[1].Commit != secondSHA[:8] {
		t.Fatalf("Entries[1].Commit = %q, want %q", output.Entries[1].Commit, secondSHA[:8])
	}
	if output.Entries[1].Author != "Loom Test" || output.Entries[1].Date == "" {
		t.Fatalf("Entries[1] = %+v, want author and date filled", output.Entries[1])
	}

	// Historical rev blame: line 1 still belongs to the first commit.
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "git_blame", gitBlameArgs{
		RepoRoot: filepath.Join(workspaceRoot, "repo"),
		Path:     "repo/a.txt",
		Rev:      secondSHA,
	}))
	if err != nil {
		t.Fatalf("Prepare(rev) error = %v", err)
	}
	result = tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute(rev) status = %s, want success: %+v", result.Status, result.Error)
	}
	// Fresh variable: decoding into the reused struct would keep
	// omitempty fields absent from this payload at their previous values.
	var revOutput gitBlameOutput
	decodeToolResult(t, result, &revOutput)
	if revOutput.Entries[0].Commit != firstSHA[:8] || revOutput.Entries[0].Uncommitted {
		t.Fatalf("rev blame Entries[0] = %+v, want first commit %q", revOutput.Entries[0], firstSHA[:8])
	}
}

func TestGitBlameToolValidatesWindow(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "a.txt"), []byte("one\n"))
	gitRun(t, repoRoot, "add", ".")
	gitRun(t, repoRoot, "commit", "-m", "init")

	tool, err := NewGitBlameTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitBlameTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), newToolCall(t, "git_blame", gitBlameArgs{
		RepoRoot:  filepath.Join(workspaceRoot, "repo"),
		Path:      "repo/a.txt",
		StartLine: 10,
		EndLine:   5,
	}))
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	_, err = tool.Prepare(context.Background(), newToolCall(t, "git_blame", gitBlameArgs{
		RepoRoot:  filepath.Join(workspaceRoot, "repo"),
		Path:      "repo/a.txt",
		StartLine: 1,
		EndLine:   maxBlameLines + 1,
	}))
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
}

func TestParseBlamePorcelainFillsRepeatedCommitFromCache(t *testing.T) {
	sha := strings.Repeat("a", 40)
	data := sha + " 1 1 2\n" +
		"author Alice\n" +
		"author-time 1700000000\n" +
		"\tone\n" +
		sha + " 2 2\n" +
		"\ttwo\n"
	entries, err := parseBlamePorcelain([]byte(data))
	if err != nil {
		t.Fatalf("parseBlamePorcelain() error = %v", err)
	}
	if len(entries) != 2 {
		t.Fatalf("len(entries) = %d, want 2", len(entries))
	}
	if entries[1].Author != "Alice" || entries[1].Date == "" {
		t.Fatalf("entries[1] = %+v, want attribution filled from cache", entries[1])
	}
	if entries[0].Line != 1 || entries[1].Line != 2 {
		t.Fatalf("lines = %d,%d, want 1,2", entries[0].Line, entries[1].Line)
	}
	if entries[0].Commit != "aaaaaaaa" {
		t.Fatalf("entries[0].Commit = %q, want aaaaaaaa", entries[0].Commit)
	}
}

// TestGitToolsDisableRepoFsmonitor arms a repo-local core.fsmonitor that
// would execute on git status and asserts the git tools still refuse to
// run it (gitBaseArgs passes -c core.fsmonitor=false; REVIEW M26). This
// also exercises the post-A6 path: the git process now runs through the
// process runner, so the protection must survive that migration.
func TestGitToolsDisableRepoFsmonitor(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	marker := filepath.Join(t.TempDir(), "fsmonitor-ran")
	script := filepath.Join(t.TempDir(), "evil-fsmonitor.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ntouch "+marker+"\n"), 0o755); err != nil {
		t.Fatalf("os.WriteFile(script) error = %v", err)
	}
	gitRun(t, repoRoot, "config", "core.fsmonitor", script)

	tool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{RepoRoot: filepath.Join(workspaceRoot, "repo")}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatalf("repo fsmonitor executed despite -c core.fsmonitor=false (marker %s exists)", marker)
	}
}

// TestGitToolsIgnoreGlobalConfig plants a malicious $HOME/.gitconfig and
// asserts git tools are immune: the runner's env allowlist keeps
// GIT_CONFIG_GLOBAL=/dev/null and GIT_CONFIG_NOSYSTEM=1 (REVIEW A6), so a
// crafted global config can neither arm hooks nor alias commands.
func TestGitToolsIgnoreGlobalConfig(t *testing.T) {
	ensureGitAvailable(t)
	validator, runner, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	home := t.TempDir()
	marker := filepath.Join(t.TempDir(), "global-config-ran")
	script := filepath.Join(t.TempDir(), "evil-global.sh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\ntouch "+marker+"\n"), 0o755); err != nil {
		t.Fatalf("os.WriteFile(script) error = %v", err)
	}
	globalConfig := "[core]\n\tfsmonitor = " + script + "\n[alias]\n\tstatus = !touch " + marker + "\n"
	if err := os.WriteFile(filepath.Join(home, ".gitconfig"), []byte(globalConfig), 0o644); err != nil {
		t.Fatalf("os.WriteFile(.gitconfig) error = %v", err)
	}
	t.Setenv("HOME", home)

	tool, err := NewGitStatusTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "git_status", gitStatusArgs{RepoRoot: filepath.Join(workspaceRoot, "repo")}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if _, err := os.Stat(marker); !os.IsNotExist(err) {
		t.Fatalf("global gitconfig fsmonitor/alias executed despite GIT_CONFIG_GLOBAL=/dev/null (marker %s exists)", marker)
	}
}
