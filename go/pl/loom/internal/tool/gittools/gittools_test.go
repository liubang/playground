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
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestGitStatusToolClassifiesChanges(t *testing.T) {
	ensureGitAvailable(t)
	validator, workspaceRoot, repoRoot := newGitValidator(t)
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

	tool, err := NewGitStatusTool(validator)
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
	validator, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("one\ntwo\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other\n"))
	gitRun(t, repoRoot, "add", "tracked.txt", "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("one\nTWO\nthree\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other changed\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")

	tool, err := NewGitDiffTool(validator)
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
	validator, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)
	mustWriteFile(t, filepath.Join(repoRoot, "tracked.txt"), []byte("base\n"))
	gitRun(t, repoRoot, "add", "tracked.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	statusTool, err := NewGitStatusTool(validator)
	if err != nil {
		t.Fatalf("NewGitStatusTool() error = %v", err)
	}
	diffTool, err := NewGitDiffTool(validator)
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
	validator, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	literalMagic := ":(glob)*.txt"
	mustWriteFile(t, filepath.Join(repoRoot, literalMagic), []byte("one\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("two\n"))
	gitRun(t, repoRoot, "add", literalMagic, "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, literalMagic), []byte("ONE\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("TWO\n"))

	tool, err := NewGitDiffTool(validator)
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
	validator, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	literalName := "star[1]?.txt"
	mustWriteFile(t, filepath.Join(repoRoot, literalName), []byte("before\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other\n"))
	gitRun(t, repoRoot, "add", literalName, "other.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")

	mustWriteFile(t, filepath.Join(repoRoot, literalName), []byte("after\n"))
	mustWriteFile(t, filepath.Join(repoRoot, "other.txt"), []byte("other changed\n"))

	tool, err := NewGitDiffTool(validator)
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
	validator, workspaceRoot, repoRoot := newGitValidator(t)
	configureGitRepo(t, repoRoot)

	base := strings.Repeat("base line for large diff\n", 5000)
	changed := strings.Repeat("changed line for large diff\n", 5000)
	mustWriteFile(t, filepath.Join(repoRoot, "large.txt"), []byte(base))
	gitRun(t, repoRoot, "add", "large.txt")
	gitRun(t, repoRoot, "commit", "-m", "init")
	mustWriteFile(t, filepath.Join(repoRoot, "large.txt"), []byte(changed))

	tool, err := NewGitDiffTool(validator)
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

func newGitValidator(t *testing.T) (*workspacepkg.PathValidator, string, string) {
	t.Helper()
	workspaceRoot := filepath.Join(t.TempDir(), "workspace")
	repoRoot := filepath.Join(workspaceRoot, "repo")
	mustMkdirAll(t, repoRoot)
	validator, err := workspacepkg.NewPathValidator(workspaceRoot)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	return validator, workspaceRoot, repoRoot
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
	if !domain.As(err, &agentErr) {
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
