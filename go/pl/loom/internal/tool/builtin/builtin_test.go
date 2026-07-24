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
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func TestReadFileToolPrepareAndExecute(t *testing.T) {
	validator, root := newValidator(t)
	content := "zero\none\ntwo\nthree\n"
	mustWriteFile(t, filepath.Join(root, "dir", "sample.txt"), []byte(content))

	tool, err := NewReadFileTool(validator, nil)
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

	tool, err := NewReadFileTool(validator, nil)
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

	tool, err := NewReadFileTool(validator, nil)
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

	tool, err := NewReadFileTool(validator, nil)
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

func TestListDirToolExecuteAndTruncate(t *testing.T) {
	validator, root := newValidator(t)
	mustMkdirAll(t, filepath.Join(root, "subdir"))
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, "alpha.txt"), []byte("a"))
	mustWriteFile(t, filepath.Join(root, "beta.txt"), []byte("b"))
	mustWriteFile(t, filepath.Join(root, ".gitignore"), []byte("ignored? no"))
	mustWriteFile(t, filepath.Join(root, ".git", "config"), []byte("secret"))
	mustSymlink(t, "alpha.txt", filepath.Join(root, "link.txt"))

	tool, err := NewListDirTool(validator)
	if err != nil {
		t.Fatalf("NewListDirTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "list_dir", listDirArgs{Path: "."}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output listDirOutput
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
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "list_dir", listDirArgs{Path: "many"}))
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

// The Go fallback engine keeps its own accounting (scanned/skipped files),
// binary and symlink filtering, and before/after context lines.
func TestSearchGoFallbackSkipsBinaryAndSymlink(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "src", "a.txt"), []byte("Hello world\nsecond line\nHELLO again\n"))
	mustWriteFile(t, filepath.Join(root, "src", "b.txt"), []byte("no match here\n"))
	mustWriteFile(t, filepath.Join(root, "src", "bin.dat"), []byte{'x', 0, 'y'})
	mustWriteFile(t, filepath.Join(root, "src", "big.txt"), []byte(strings.Repeat("a", maxSearchFileBytes+1)))
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, ".git", "secret.txt"), []byte("hello from sensitive path\n"))
	mustSymlink(t, filepath.Join("src", "a.txt"), filepath.Join(root, "src", "link.txt"))

	// A nil runner forces the Go fallback engine.
	tool, err := NewSearchTool(validator, nil)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "hello",
		Context: 1,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output searchOutput
	decodeToolResult(t, result, &output)
	if output.Engine != string(engineGoFallback) {
		t.Fatalf("output.Engine = %q, want go_fallback", output.Engine)
	}
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
	wantMatches := []searchMatch{
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

func TestSearchGoFallbackTruncateStrictJSONAndCancelled(t *testing.T) {
	validator, root := newValidator(t)
	var builder strings.Builder
	for i := 0; i < maxSearchMatches+5; i++ {
		builder.WriteString("needle\n")
	}
	mustWriteFile(t, filepath.Join(root, "many.txt"), []byte(builder.String()))

	tool, err := NewSearchTool(validator, nil)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}

	_, err = tool.Prepare(context.Background(), domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "search",
		Arguments: json.RawMessage(`{"path":".","pattern":"needle","extra":true}`),
	})
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{Pattern: "needle"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output searchOutput
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

// fakeRgRunner stubs the sandboxed process runner so ripgrep-engine tests do
// not depend on the host platform, sandbox, or an installed rg binary.
type fakeRgRunner struct {
	result   process.Result
	err      error
	lastSpec process.CommandSpec
}

func (f *fakeRgRunner) Run(_ context.Context, spec process.CommandSpec) (process.Result, error) {
	f.lastSpec = spec
	return f.result, f.err
}

// stubRgLocator makes rgAvailable succeed without a real rg binary.
func stubRgLocator(t *testing.T) {
	t.Helper()
	old := rgLocator
	rgLocator = func() (string, bool) { return "/fake/rg", true }
	t.Cleanup(func() { rgLocator = old })
}

func rgJSON(events ...string) []byte {
	return []byte(strings.Join(events, "\n") + "\n")
}

func TestSearchRipgrepEngineAggregatesMatchesAndContext(t *testing.T) {
	stubRgLocator(t)
	validator, root := newValidator(t)
	mustMkdirAll(t, filepath.Join(root, "src"))

	runner := &fakeRgRunner{result: process.Result{
		ExitCode: 0,
		Stdout: rgJSON(
			`{"type":"context","data":{"path":{"text":"`+filepath.Join(validator.Root(), "src", "a.go")+`"},"line_number":1,"lines":{"text":"package src\n"}}}`,
			`{"type":"match","data":{"path":{"text":"`+filepath.Join(validator.Root(), "src", "a.go")+`"},"line_number":2,"lines":{"text":"func hello() {}\n"}}}`,
			`{"type":"context","data":{"path":{"text":"`+filepath.Join(validator.Root(), "src", "a.go")+`"},"line_number":3,"lines":{"text":"// after\n"}}}`,
		),
	}}

	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "func \\w+\\(",
		Path:    "src",
		Context: 1,
		Glob:    []string{"*.go"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}

	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	// The argv passed to rg must be allowlist-assembled with the pattern and
	// search root after a "--" separator.
	argv := runner.lastSpec.Args
	n := len(argv)
	if n < 3 || argv[n-3] != "--" || argv[n-2] != `func \w+\(` || argv[n-1] != filepath.Join(validator.Root(), "src") {
		t.Fatalf("rg argv tail = %v", argv[max(0, n-4):])
	}
	joined := strings.Join(argv, " ")
	for _, want := range []string{"--json", "--max-count", "-C 1", "-i", "--glob *.go"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("rg argv missing %q: %v", want, argv)
		}
	}

	var output searchOutput
	decodeToolResult(t, result, &output)
	if output.Engine != string(engineRipgrep) {
		t.Fatalf("output.Engine = %q, want ripgrep", output.Engine)
	}
	if output.MatchCount != 1 {
		t.Fatalf("output.MatchCount = %d, want 1", output.MatchCount)
	}
	match := output.Matches[0]
	if match.Path != "src/a.go" || match.Line != 2 || match.Text != "func hello() {}" {
		t.Fatalf("unexpected match: %+v", match)
	}
	if len(match.Before) != 1 || match.Before[0].Text != "package src" {
		t.Fatalf("unexpected before context: %+v", match.Before)
	}
	if len(match.After) != 1 || match.After[0].Text != "// after" {
		t.Fatalf("unexpected after context: %+v", match.After)
	}
}

func TestSearchRipgrepErrorSurfacing(t *testing.T) {
	stubRgLocator(t)
	validator, _ := newValidator(t)
	runner := &fakeRgRunner{result: process.Result{ExitCode: 2, Stderr: []byte("regex parse error")}}

	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{Pattern: "("}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	assertToolResultError(t, result, domain.ToolStatusError, domain.ErrInvalidInput)
}

func TestGlobRipgrepEngineListsFiles(t *testing.T) {
	stubRgLocator(t)
	validator, root := newValidator(t)
	mustMkdirAll(t, filepath.Join(root, "src"))

	runner := &fakeRgRunner{result: process.Result{
		ExitCode: 0,
		Stdout:   []byte("a.go\nsub/b.go\nREADME.md\n"),
	}}

	tool, err := NewGlobTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGlobTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "glob", globArgs{Pattern: "**/*.go", Path: "src"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output globOutput
	decodeToolResult(t, result, &output)
	if output.Engine != string(engineRipgrep) {
		t.Fatalf("output.Engine = %q, want ripgrep", output.Engine)
	}
	want := []string{"src/README.md", "src/a.go", "src/sub/b.go"}
	if !reflect.DeepEqual(output.Files, want) {
		t.Fatalf("output.Files = %v, want %v", output.Files, want)
	}
	if runner.lastSpec.Args[0] != "--files" {
		t.Fatalf("rg argv = %v, want --files first", runner.lastSpec.Args)
	}
}

func TestGlobGoFallbackMatchesDoubleStar(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "a.go"), []byte("package a\n"))
	mustWriteFile(t, filepath.Join(root, "sub", "b.go"), []byte("package b\n"))
	mustWriteFile(t, filepath.Join(root, "sub", "c.txt"), []byte("text\n"))
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, ".git", "ignored.go"), []byte("package git\n"))

	tool, err := NewGlobTool(validator, nil)
	if err != nil {
		t.Fatalf("NewGlobTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "glob", globArgs{Pattern: "**/*.go"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output globOutput
	decodeToolResult(t, result, &output)
	if output.Engine != string(engineGoFallback) {
		t.Fatalf("output.Engine = %q, want go_fallback", output.Engine)
	}
	want := []string{"a.go", "sub/b.go"}
	if !reflect.DeepEqual(output.Files, want) {
		t.Fatalf("output.Files = %v, want %v (.git must be excluded)", output.Files, want)
	}
}

// TestSearchRipgrepRealEndToEnd exercises the true ripgrep binary through the
// platform sandbox. It skips when rg or a usable sandbox is unavailable.
func TestSearchRipgrepRealEndToEnd(t *testing.T) {
	if _, ok := rgLocator(); !ok {
		t.Skip("ripgrep not installed")
	}
	validator, root := newValidator(t)
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		t.Skipf("platform sandbox unavailable: %v", err)
	}
	// Nested sandboxes (e.g. under Bazel's own test sandbox) reject
	// sandbox_apply; skip there instead of failing.
	probe, probeErr := runner.Run(context.Background(), process.CommandSpec{
		Program: "/usr/bin/true",
		Args:    []string{},
		Cwd:     ".",
		Env:     map[string]string{},
		Timeout: 5 * time.Second,
	})
	if probeErr != nil || probe.ExitCode != 0 {
		t.Skipf("nested sandbox unavailable: err=%v exit=%d stderr=%s", probeErr, probe.ExitCode, probe.Stderr)
	}

	// ripgrep only honors .gitignore inside a git repository.
	mustMkdirAll(t, filepath.Join(root, ".git"))
	mustWriteFile(t, filepath.Join(root, "src", "a.go"), []byte("package src\nfunc hello() {}\n"))
	mustWriteFile(t, filepath.Join(root, "src", "b.txt"), []byte("func ignored\n"))
	mustMkdirAll(t, filepath.Join(root, "bazel-out"))
	mustWriteFile(t, filepath.Join(root, "bazel-out", "gen.go"), []byte("func generated() {}\n"))
	mustWriteFile(t, filepath.Join(root, ".gitignore"), []byte("bazel-out/\n"))

	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: `func \w+\(`,
		Glob:    []string{"*.go"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}

	var output searchOutput
	decodeToolResult(t, result, &output)
	if output.Engine != string(engineRipgrep) {
		t.Fatalf("output.Engine = %q, want ripgrep", output.Engine)
	}
	if output.MatchCount != 1 || output.Matches[0].Path != "src/a.go" {
		t.Fatalf("matches = %+v, want exactly src/a.go (gitignore must exclude bazel-out, glob must exclude b.txt)", output.Matches)
	}
}

func TestMatchGlobPath(t *testing.T) {
	cases := []struct {
		pattern, name string
		want          bool
	}{
		{"*.go", "a.go", true},
		{"*.go", "a.txt", false},
		{"**/*.go", "a.go", true},
		{"**/*.go", "sub/deep/a.go", true},
		{"src/**", "src/a/b.go", true},
		{"src/**", "other/a.go", false},
		{"sub/*/c.go", "sub/b/c.go", true},
		{"sub/*/c.go", "sub/b/d/c.go", false},
		{"test_*.go", "test_a.go", true},
	}
	for _, tc := range cases {
		if got := matchGlobPath(tc.pattern, tc.name); got != tc.want {
			t.Errorf("matchGlobPath(%q, %q) = %v, want %v", tc.pattern, tc.name, got, tc.want)
		}
	}
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
