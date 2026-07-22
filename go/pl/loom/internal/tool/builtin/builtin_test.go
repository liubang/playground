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

package builtin

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestReadFileToolPrepareAndExecute(t *testing.T) {
	validator, root := newValidator(t)
	content := "zero\none\ntwo\nthree\n"
	mustWriteFile(t, filepath.Join(root, "dir", "sample.txt"), []byte(content))

	tool, err := NewReadFileTool(validator)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{
		Path:   filepath.Join(root, "dir", "sample.txt"),
		Offset: 2,
		Limit:  2,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if got, want := prepared.ReadPaths, []string{filepath.Join(validator.Root(), "dir", "sample.txt")}; !reflect.DeepEqual(got, want) {
		t.Fatalf("prepared.ReadPaths = %v, want %v", got, want)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output readFileOutput
	decodeToolResult(t, result, &output)
	if output.Path != "dir/sample.txt" {
		t.Fatalf("output.Path = %q, want %q", output.Path, "dir/sample.txt")
	}
	if output.Offset != 2 || output.Limit != 2 {
		t.Fatalf("unexpected window: offset=%d limit=%d", output.Offset, output.Limit)
	}
	if output.TotalLines != 4 {
		t.Fatalf("output.TotalLines = %d, want 4", output.TotalLines)
	}
	if !output.Truncated {
		t.Fatal("expected truncated output")
	}
	wantLines := []readFileLine{{Number: 2, Text: "one"}, {Number: 3, Text: "two"}}
	if !reflect.DeepEqual(output.Lines, wantLines) {
		t.Fatalf("output.Lines = %#v, want %#v", output.Lines, wantLines)
	}
	if output.SizeBytes != int64(len(content)) {
		t.Fatalf("output.SizeBytes = %d, want %d", output.SizeBytes, len(content))
	}
	if output.ContentHash != hexSHA256([]byte(content)) {
		t.Fatalf("output.ContentHash = %q, want %q", output.ContentHash, hexSHA256([]byte(content)))
	}
}

func TestReadFileToolRejectsSensitiveComponent(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "safe", ".git", "config"), []byte("secret"))

	tool, err := NewReadFileTool(validator)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}

	_, err = tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{
		Path: filepath.Join(root, "safe", ".git", "config"),
	}))
	assertAgentErrorCode(t, err, domain.ErrSecurity)
}

func TestReadFileToolStrictJSONBinaryAndPreparedBinding(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "text.txt"), []byte("alpha\nbeta\n"))
	mustWriteFile(t, filepath.Join(root, "data.bin"), []byte{'a', 0, 'b'})

	tool, err := NewReadFileTool(validator)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}

	_, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "read_file",
		Arguments: json.RawMessage(`{"path":"text.txt","unknown":1}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	binaryPrepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{Path: "data.bin"}))
	if err != nil {
		t.Fatalf("Prepare(binary) error = %v", err)
	}
	binaryResult := tool.Execute(context.Background(), binaryPrepared)
	assertToolResultError(t, binaryResult, domain.ToolStatusError, domain.ErrInvalidInput)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{Path: "text.txt"}))
	if err != nil {
		t.Fatalf("Prepare(text) error = %v", err)
	}
	prepared.Call.Arguments = mustMarshalRaw(t, readFileArgs{Path: "text.txt", Offset: 2, Limit: 1})
	tampered := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, tampered, domain.ToolStatusError, domain.ErrSecurity)
}

func TestReadFileToolCancelled(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "text.txt"), []byte("hello\nworld\n"))

	tool, err := NewReadFileTool(validator)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{Path: "text.txt"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	result := tool.Execute(ctx, prepared)
	assertToolResultError(t, result, domain.ToolStatusCancelled, domain.ErrCancelled)
}

func TestListDirectoryToolExecuteAndTruncate(t *testing.T) {
	validator, root := newValidator(t)
	mustMkdirAll(t, filepath.Join(root, "subdir"))
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, "alpha.txt"), []byte("a"))
	mustWriteFile(t, filepath.Join(root, "beta.txt"), []byte("b"))
	mustWriteFile(t, filepath.Join(root, ".gitignore"), []byte("ignored? no"))
	mustWriteFile(t, filepath.Join(root, ".git", "config"), []byte("secret"))
	mustSymlink(t, "alpha.txt", filepath.Join(root, "link.txt"))

	tool, err := NewListDirectoryTool(validator)
	if err != nil {
		t.Fatalf("NewListDirectoryTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "list_directory", listDirectoryArgs{Path: "."}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output listDirectoryOutput
	decodeToolResult(t, result, &output)
	gotPaths := make([]string, 0, len(output.Entries))
	for _, entry := range output.Entries {
		gotPaths = append(gotPaths, entry.Path)
	}
	wantPaths := []string{"subdir", ".gitignore", "alpha.txt", "beta.txt"}
	if !reflect.DeepEqual(gotPaths, wantPaths) {
		t.Fatalf("directory entries = %v, want %v", gotPaths, wantPaths)
	}
	if output.Truncated {
		t.Fatal("did not expect truncated root listing")
	}

	manyDir := filepath.Join(root, "many")
	mustMkdirAll(t, manyDir)
	for i := 0; i < maxDirectoryEntries+5; i++ {
		mustWriteFile(t, filepath.Join(manyDir, fmt.Sprintf("file-%03d.txt", i)), []byte("x"))
	}
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "list_directory", listDirectoryArgs{Path: "many"}))
	if err != nil {
		t.Fatalf("Prepare(many) error = %v", err)
	}
	result = tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute(many) status = %s, want success: %+v", result.Status, result.Error)
	}
	decodeToolResult(t, result, &output)
	if !output.Truncated {
		t.Fatal("expected truncated directory listing")
	}
	if output.EntryCount != maxDirectoryEntries {
		t.Fatalf("output.EntryCount = %d, want %d", output.EntryCount, maxDirectoryEntries)
	}
	if output.Entries[0].Path != "many/file-000.txt" || output.Entries[len(output.Entries)-1].Path != fmt.Sprintf("many/file-%03d.txt", maxDirectoryEntries-1) {
		t.Fatalf("unexpected deterministic ordering in truncated output: first=%q last=%q", output.Entries[0].Path, output.Entries[len(output.Entries)-1].Path)
	}
}

func TestSearchTextToolExecuteSkipsBinaryAndSymlink(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "src", "a.txt"), []byte("Hello world\nsecond line\nHELLO again\n"))
	mustWriteFile(t, filepath.Join(root, "src", "b.txt"), []byte("no match here\n"))
	mustWriteFile(t, filepath.Join(root, "src", "bin.dat"), []byte{'x', 0, 'y'})
	mustWriteFile(t, filepath.Join(root, "src", "big.txt"), []byte(strings.Repeat("a", maxSearchFileBytes+1)))
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, ".git", "secret.txt"), []byte("hello from sensitive path\n"))
	mustSymlink(t, filepath.Join("src", "a.txt"), filepath.Join(root, "src", "link.txt"))

	tool, err := NewSearchTextTool(validator)
	if err != nil {
		t.Fatalf("NewSearchTextTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search_text", searchTextArgs{
		Path:   ".",
		Query:  "hello",
		Before: 1,
		After:  1,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output searchTextOutput
	decodeToolResult(t, result, &output)
	if output.MatchCount != 2 {
		t.Fatalf("output.MatchCount = %d, want 2", output.MatchCount)
	}
	if output.ScannedFiles != 2 {
		t.Fatalf("output.ScannedFiles = %d, want 2", output.ScannedFiles)
	}
	if output.SkippedBinary != 1 {
		t.Fatalf("output.SkippedBinary = %d, want 1", output.SkippedBinary)
	}
	if output.SkippedTooLarge != 1 {
		t.Fatalf("output.SkippedTooLarge = %d, want 1", output.SkippedTooLarge)
	}
	if output.Truncated {
		t.Fatal("did not expect truncated search output")
	}
	wantMatches := []searchTextMatch{
		{
			Path:   "src/a.txt",
			Line:   1,
			Text:   "Hello world",
			After:  []contextLine{{Line: 2, Text: "second line"}},
			Before: nil,
		},
		{
			Path:   "src/a.txt",
			Line:   3,
			Text:   "HELLO again",
			Before: []contextLine{{Line: 2, Text: "second line"}},
			After:  nil,
		},
	}
	if !reflect.DeepEqual(output.Matches, wantMatches) {
		t.Fatalf("output.Matches = %#v, want %#v", output.Matches, wantMatches)
	}
}

func TestSearchTextToolTruncateStrictJSONAndCancelled(t *testing.T) {
	validator, root := newValidator(t)
	var builder strings.Builder
	for i := 0; i < maxSearchMatches+5; i++ {
		builder.WriteString("needle\n")
	}
	mustWriteFile(t, filepath.Join(root, "many.txt"), []byte(builder.String()))

	tool, err := NewSearchTextTool(validator)
	if err != nil {
		t.Fatalf("NewSearchTextTool() error = %v", err)
	}

	_, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "search_text",
		Arguments: json.RawMessage(`{"path":".","query":"needle","extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search_text", searchTextArgs{Path: ".", Query: "needle"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output searchTextOutput
	decodeToolResult(t, result, &output)
	if !output.Truncated {
		t.Fatal("expected truncated search output")
	}
	if output.MatchCount != maxSearchMatches {
		t.Fatalf("output.MatchCount = %d, want %d", output.MatchCount, maxSearchMatches)
	}
	if output.Matches[0].Line != 1 || output.Matches[len(output.Matches)-1].Line != maxSearchMatches {
		t.Fatalf("unexpected match boundaries: first=%d last=%d", output.Matches[0].Line, output.Matches[len(output.Matches)-1].Line)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	cancelled := tool.Execute(ctx, prepared)
	assertToolResultError(t, cancelled, domain.ToolStatusCancelled, domain.ErrCancelled)
}

func newValidator(t *testing.T) (*workspacepkg.PathValidator, string) {
	t.Helper()
	root := filepath.Join(t.TempDir(), "workspace")
	mustMkdirAll(t, root)
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

func mustWriteFile(t *testing.T, path string, data []byte) {
	t.Helper()
	mustMkdirAll(t, filepath.Dir(path))
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatalf("os.WriteFile(%q) error = %v", path, err)
	}
}

func mustMkdirAll(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(path, 0755); err != nil {
		t.Fatalf("os.MkdirAll(%q) error = %v", path, err)
	}
}

func mustSymlink(t *testing.T, target, link string) {
	t.Helper()
	mustMkdirAll(t, filepath.Dir(link))
	if err := os.Symlink(target, link); err != nil {
		t.Fatalf("os.Symlink(%q, %q) error = %v", target, link, err)
	}
}

func hexSHA256(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
