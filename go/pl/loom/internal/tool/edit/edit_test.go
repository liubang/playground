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
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestEditToolSuccessAndPermissionPreserved(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "note.txt")
	original := []byte("hello world\n")
	mustWriteFile(t, path, original, 0o600)

	tool, err := NewEditTool(validator, nil)
	if err != nil {
		t.Fatalf("NewEditTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      "note.txt",
		OldString: "world",
		NewString: "loom",
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

func TestEditToolConflictsAndTampering(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "multi.txt")
	original := []byte("dup\ndup\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewEditTool(validator, nil)
	if err != nil {
		t.Fatalf("NewEditTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "edit",
		Arguments: json.RawMessage(`{"path":"multi.txt","old_string":"dup","new_string":"x","extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	// Without replace_all, multiple matches conflict.
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      "multi.txt",
		OldString: "dup",
		NewString: "x",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	conflict := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, conflict, domain.ToolStatusError, domain.ErrConflict)

	// A tampered prepared call fails HMAC verification.
	prepared.Call.Arguments = mustMarshalRaw(t, editArgs{
		Path:       "multi.txt",
		OldString:  "dup",
		NewString:  "x",
		ReplaceAll: true,
	})
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)
}

func TestEditToolExpectedHashConflictAndReplaceAll(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "doc.txt")
	original := []byte("foo foo\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewEditTool(validator, nil)
	if err != nil {
		t.Fatalf("NewEditTool() error = %v", err)
	}

	// The optional expected_hash guard catches drift authoritatively.
	stale, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:         "doc.txt",
		OldString:    "foo",
		NewString:    "bar",
		ReplaceAll:   true,
		ExpectedHash: workspacepkg.EmptyFileSHA256,
	}))
	if err != nil {
		t.Fatalf("Prepare(stale) error = %v", err)
	}
	conflict := tool.Execute(context.Background(), stale)
	assertToolResultError(t, conflict, domain.ToolStatusError, domain.ErrConflict)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:       "doc.txt",
		OldString:  "foo",
		NewString:  "bar",
		ReplaceAll: true,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "bar bar\n" {
		t.Fatalf("content = %q, want replace_all applied", string(content))
	}
}

func TestEditToolRejectsSymlinkAndSensitivePath(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "target.txt"), []byte("hello"), 0o644)
	mustSymlink(t, "target.txt", filepath.Join(root, "link.txt"))
	tool, err := NewEditTool(validator, nil)
	if err != nil {
		t.Fatalf("NewEditTool() error = %v", err)
	}
	_, err = tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      "link.txt",
		OldString: "hello",
		NewString: "bye",
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)
	_, err = tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      filepath.Join(root, ".git", "config"),
		OldString: "x",
		NewString: "y",
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)
}

// The shared file-state book detects external modification after the agent's
// last read without any model-carried hash; a fresh read re-arms the edit.
func TestEditToolDetectsExternalModificationViaBook(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "note.txt")
	mustWriteFile(t, path, []byte("hello world\n"), 0o644)

	book := workspacepkg.NewFileStateBook()
	tool, err := NewEditTool(validator, book)
	if err != nil {
		t.Fatalf("NewEditTool() error = %v", err)
	}

	// Simulate a prior read_file: the book now knows the file's hash.
	abs := filepath.Join(validator.Root(), "note.txt")
	snapshot, err := validator.Snapshot(abs)
	if err != nil {
		t.Fatalf("Snapshot() error = %v", err)
	}
	book.Record(abs, snapshot.SHA256)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      "note.txt",
		OldString: "world",
		NewString: "loom",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}

	// External modification after the read must fail closed.
	mustWriteFile(t, path, []byte("external change\n"), 0o644)
	conflict := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, conflict, domain.ToolStatusError, domain.ErrConflict)
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "external change\n" {
		t.Fatalf("externally modified file must not be overwritten: %q", string(content))
	}

	// A fresh read records the new state and the edit goes through.
	snapshot2, err := validator.Snapshot(abs)
	if err != nil {
		t.Fatalf("Snapshot() error = %v", err)
	}
	book.Record(abs, snapshot2.SHA256)
	prepared2, err := tool.Prepare(context.Background(), newToolCall(t, "edit", editArgs{
		Path:      "note.txt",
		OldString: "external",
		NewString: "reviewed",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared2)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success after re-read: %+v", result.Status, result.Error)
	}
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

func TestWriteToolCreatesFileWithParentDirectories(t *testing.T) {
	validator, _ := newValidator(t)
	tool, err := NewWriteTool(validator)
	if err != nil {
		t.Fatalf("NewWriteTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "write", writeArgs{
		Path:    "nested/dir/new.go",
		Content: "package dir\n",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if prepared.Recovery != nil {
		t.Fatalf("create must not carry recovery evidence: %+v", prepared.Recovery)
	}
	if want := "Write nested/dir/new.go (12 bytes, create)"; prepared.ApprovalDesc != want {
		t.Fatalf("ApprovalDesc = %q, want %q", prepared.ApprovalDesc, want)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output writeOutput
	decodeToolResult(t, result, &output)
	if !output.Created || output.Path != "nested/dir/new.go" || output.Size != 12 {
		t.Fatalf("unexpected output: %+v", output)
	}
	if output.OldHash != "" {
		t.Fatalf("create must not report old_hash: %+v", output)
	}
	content, err := os.ReadFile(filepath.Join(validator.Root(), "nested", "dir", "new.go"))
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "package dir\n" {
		t.Fatalf("content = %q", string(content))
	}
}

func TestWriteToolOverwriteBindsStateAndDetectsDrift(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "note.txt")
	original := []byte("before\n")
	mustWriteFile(t, path, original, 0o644)

	tool, err := NewWriteTool(validator)
	if err != nil {
		t.Fatalf("NewWriteTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "write", writeArgs{
		Path:    "note.txt",
		Content: "after\n",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if want := "Write note.txt (6 bytes, overwrite)"; prepared.ApprovalDesc != want {
		t.Fatalf("ApprovalDesc = %q, want %q", prepared.ApprovalDesc, want)
	}
	if prepared.Recovery == nil || prepared.Recovery.ExpectedHash != hexSHA256(original) ||
		prepared.Recovery.ResultHash != hexSHA256([]byte("after\n")) {
		t.Fatalf("unexpected recovery evidence: %+v", prepared.Recovery)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output writeOutput
	decodeToolResult(t, result, &output)
	if output.Created || output.OldHash != hexSHA256(original) {
		t.Fatalf("unexpected output: %+v", output)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "after\n" {
		t.Fatalf("content = %q", string(content))
	}
}

func TestWriteToolFailsClosedWhenFileDriftsAfterApproval(t *testing.T) {
	validator, root := newValidator(t)
	path := filepath.Join(root, "note.txt")
	mustWriteFile(t, path, []byte("before\n"), 0o644)

	tool, err := NewWriteTool(validator)
	if err != nil {
		t.Fatalf("NewWriteTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "write", writeArgs{
		Path:    "note.txt",
		Content: "after\n",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}

	// External modification between approval and execution must fail closed.
	mustWriteFile(t, path, []byte("external\n"), 0o644)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("Execute() status = %s, want error after drift", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrConflict) {
		t.Fatalf("error code = %v, want conflict", result.Error)
	}
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("os.ReadFile() error = %v", err)
	}
	if string(content) != "external\n" {
		t.Fatalf("drifted file must not be overwritten, content = %q", string(content))
	}
}

func TestWriteToolFailsClosedWhenTargetAppearsAfterApproval(t *testing.T) {
	validator, root := newValidator(t)
	tool, err := NewWriteTool(validator)
	if err != nil {
		t.Fatalf("NewWriteTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "write", writeArgs{
		Path:    "fresh.txt",
		Content: "new\n",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}

	// The target is created externally between approval and execution.
	mustWriteFile(t, filepath.Join(root, "fresh.txt"), []byte("external\n"), 0o644)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError {
		t.Fatalf("Execute() status = %s, want error after external create", result.Status)
	}
	if result.Error == nil || result.Error.Code != string(domain.ErrConflict) {
		t.Fatalf("error code = %v, want conflict", result.Error)
	}
}

func hexSHA256(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
