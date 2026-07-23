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

package edit

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestReplaceTextToolSuccessAndPermissionPreserved(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "note.txt")
	original := []byte("hello world\n")
	mustWriteFile(t, path, original, 0o600)

	tool, err := NewReplaceTextTool(validator)
	if err != nil {
		t.Fatalf("NewReplaceTextTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "replace_text", replaceTextArgs{
		Path:         "note.txt",
		OldText:      "world",
		NewText:      "loom",
		ExpectedHash: hexSHA256(original),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if got, want := prepared.WritePaths, []string{filepath.Join(validator.Root(), "note.txt")}; len(got) != len(want) || got[0] != want[0] {
		t.Fatalf("prepared.WritePaths = %v, want %v", got, want)
	}
	if prepared.Recovery == nil || prepared.Recovery.ExpectedHash != hexSHA256(original) ||
		prepared.Recovery.ResultHash != hexSHA256([]byte("hello loom\n")) {
		t.Fatalf("unexpected recovery evidence: %+v", prepared.Recovery)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output editOutput
	decodeToolResult(t, result, &output)
	if output.Path != "note.txt" {
		t.Fatalf("output.Path = %q, want note.txt", output.Path)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "hello loom\n" {
		t.Fatalf("content = %q, want replacement applied", string(content))
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("os.Stat() error = %v", err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("mode = %o, want 600", info.Mode().Perm())
	}
}

func TestReplaceTextToolConflictsAndTampering(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "multi.txt")
	original := []byte("dup\ndup\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewReplaceTextTool(validator)
	if err != nil {
		t.Fatalf("NewReplaceTextTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "replace_text",
		Arguments: json.RawMessage(`{"path":"multi.txt","old_text":"dup","new_text":"x","expected_hash":"` + workspacepkg.EmptyFileSHA256 + `","extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "replace_text", replaceTextArgs{
		Path:         "multi.txt",
		OldText:      "dup",
		NewText:      "x",
		ExpectedHash: hexSHA256(original),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	conflict := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, conflict, domain.ToolStatusError, domain.ErrConflict)

	prepared.Call.Arguments = mustMarshalRaw(t, replaceTextArgs{
		Path:         "multi.txt",
		OldText:      "dup",
		NewText:      "x",
		ExpectedHash: hexSHA256(original),
		ReplaceAll:   true,
	})
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)
}

func TestReplaceTextToolReplaceAllAndHashConflict(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "doc.txt")
	original := []byte("foo foo\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewReplaceTextTool(validator)
	if err != nil {
		t.Fatalf("NewReplaceTextTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "replace_text", replaceTextArgs{
		Path:         "doc.txt",
		OldText:      "foo",
		NewText:      "bar",
		ExpectedHash: hexSHA256(original),
		ReplaceAll:   true,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if err := os.WriteFile(path, []byte("changed\n"), 0o644); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrConflict)
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "changed\n" {
		t.Fatalf("content = %q, want concurrent change preserved", string(content))
	}
}

func TestReplaceTextToolRejectsSymlinkAndSensitivePath(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "target.txt"), []byte("hello"), 0o644)
	mustSymlink(t, "target.txt", filepath.Join(root, "link.txt"))
	tool, err := NewReplaceTextTool(validator)
	if err != nil {
		t.Fatalf("NewReplaceTextTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), newToolCall(t, "replace_text", replaceTextArgs{
		Path:         "link.txt",
		OldText:      "hello",
		NewText:      "bye",
		ExpectedHash: hexSHA256([]byte("hello")),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)
	_, err = tool.Prepare(context.Background(), newToolCall(t, "replace_text", replaceTextArgs{
		Path:         filepath.Join(root, ".git", "config"),
		OldText:      "x",
		NewText:      "y",
		ExpectedHash: workspacepkg.EmptyFileSHA256,
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)
}

func TestApplyPatchToolSuccess(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "file.txt")
	original := []byte("one\ntwo\nthree\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewApplyPatchTool(validator)
	if err != nil {
		t.Fatalf("NewApplyPatchTool() error = %v", err)
	}
	patch := strings.Join([]string{
		"--- a/file.txt",
		"+++ b/file.txt",
		"@@ -1,3 +1,3 @@",
		" one",
		"-two",
		"+TWO",
		" three",
	}, "\n")
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "file.txt",
		Patch:        patch,
		ExpectedHash: hexSHA256(original),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if prepared.Recovery == nil || prepared.Recovery.ExpectedHash != hexSHA256(original) ||
		prepared.Recovery.ResultHash != hexSHA256([]byte("one\nTWO\nthree\n")) {
		t.Fatalf("unexpected recovery evidence: %+v", prepared.Recovery)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "one\nTWO\nthree\n" {
		t.Fatalf("content = %q, want patched output", string(content))
	}
}

func TestApplyPatchToolRejectsContextConflictAndTampering(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "file.txt")
	original := []byte("one\ntwo\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewApplyPatchTool(validator)
	if err != nil {
		t.Fatalf("NewApplyPatchTool() error = %v", err)
	}
	patch := strings.Join([]string{
		"--- a/file.txt",
		"+++ b/file.txt",
		"@@ -1,2 +1,2 @@",
		" zero",
		"-two",
		"+TWO",
	}, "\n")
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "file.txt",
		Patch:        patch,
		ExpectedHash: hexSHA256(original),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrConflict)
	prepared.WritePaths = []string{filepath.Join(root, "other.txt")}
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)
}

func TestApplyPatchToolRejectsEscapeAndMultipleFiles(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "file.txt"), []byte("one\n"), 0o644)
	tool, err := NewApplyPatchTool(validator)
	if err != nil {
		t.Fatalf("NewApplyPatchTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "file.txt",
		Patch:        "--- a/../escape.txt\n+++ b/../escape.txt\n@@ -0,0 +1 @@\n+x\n",
		ExpectedHash: hexSHA256([]byte("one\n")),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	multi := strings.Join([]string{
		"--- a/file.txt",
		"+++ b/file.txt",
		"@@ -1 +1 @@",
		"-one",
		"+ONE",
		"--- a/other.txt",
		"+++ b/other.txt",
		"@@ -0,0 +1 @@",
		"+two",
	}, "\n")
	_, err = tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "file.txt",
		Patch:        multi,
		ExpectedHash: hexSHA256([]byte("one\n")),
	}))
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
}

func TestApplyPatchToolRejectsSymlinkAndHashConflict(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "target.txt"), []byte("one\n"), 0o644)
	mustSymlink(t, "target.txt", filepath.Join(root, "link.txt"))
	tool, err := NewApplyPatchTool(validator)
	if err != nil {
		t.Fatalf("NewApplyPatchTool() error = %v", err)
	}
	patch := strings.Join([]string{
		"--- a/link.txt",
		"+++ b/link.txt",
		"@@ -1 +1 @@",
		"-one",
		"+ONE",
	}, "\n")
	_, err = tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "link.txt",
		Patch:        patch,
		ExpectedHash: hexSHA256([]byte("one\n")),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)

	mustWriteFile(t, filepath.Join(root, "file.txt"), []byte("one\n"), 0o644)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "apply_patch", applyPatchArgs{
		Path:         "file.txt",
		Patch:        strings.Join([]string{"--- a/file.txt", "+++ b/file.txt", "@@ -1 +1 @@", "-one", "+ONE"}, "\n"),
		ExpectedHash: hexSHA256([]byte("one\n")),
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(root, "file.txt"), []byte("changed\n"), 0o644); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrConflict)
}

func newValidator(t *testing.T) (*workspacepkg.PathValidator, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "workspace")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatalf("os.MkdirAll() error = %v", err)
	}
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator() error = %v", err)
	}
	return validator, root
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

func mustWriteFile(t *testing.T, path string, data []byte, mode os.FileMode) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("os.MkdirAll() error = %v", err)
	}
	if err := os.WriteFile(path, data, mode); err != nil {
		t.Fatalf("os.WriteFile() error = %v", err)
	}
}

func mustSymlink(t *testing.T, target, link string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(link), 0o755); err != nil {
		t.Fatalf("os.MkdirAll() error = %v", err)
	}
	if err := os.Symlink(target, link); err != nil {
		t.Fatalf("os.Symlink() error = %v", err)
	}
}

func hexSHA256(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
