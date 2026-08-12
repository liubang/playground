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
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/png"
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

	output := toolResultText(t, result)
	wantHeader := fmt.Sprintf("path: dir/sample.txt · lines 2-3 of 4 · %s · sha256:%s\n",
		formatByteSize(int64(len(content))), hexSHA256([]byte(content)))
	if !strings.HasPrefix(output, wantHeader) {
		t.Fatalf("output header = %q, want prefix %q", output, wantHeader)
	}
	if !strings.Contains(output, "     2→one\n     3→two\n") {
		t.Fatalf("output missing numbered lines: %q", output)
	}
	if !strings.Contains(output, "[truncated: 1 more line; call read_file with offset=4 to continue]") {
		t.Fatalf("output missing truncation marker: %q", output)
	}
}

func TestReadFileToolClampsOversizedLimit(t *testing.T) {
	validator, root := newValidator(t)
	var sb strings.Builder
	for i := 1; i <= 600; i++ {
		fmt.Fprintf(&sb, "line %d\n", i)
	}
	mustWriteFile(t, filepath.Join(root, "big.txt"), []byte(sb.String()))

	tool, err := NewReadFileTool(validator, nil)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}
	// Models asking for "the rest of the file" can exceed maxReadFileLimit;
	// clamp instead of failing the call (the header reports the real range).
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{
		Path: filepath.Join(root, "big.txt"), Offset: 1, Limit: 683,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s: %+v", result.Status, result.Error)
	}
	output := toolResultText(t, result)
	if !strings.Contains(output, "lines 1-500 of 600") {
		t.Fatalf("output header = %q, want clamped range", strings.Split(output, "\n")[0])
	}
	if !strings.Contains(output, "offset=501") {
		t.Fatalf("output missing continuation hint: %q", output)
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

// Read alignment: absolute paths outside the workspace are readable
// through the builtin read tools, matching the sandbox's broad read
// allowance (workspace.PathValidator.ValidateRead).
func TestReadFileToolReadsExternalPath(t *testing.T) {
	validator, _ := newValidator(t)
	content := "external\ncontent\n"
	external := filepath.Join(t.TempDir(), "external.txt")
	mustWriteFile(t, external, []byte(content))

	tool, err := NewReadFileTool(validator, nil)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{Path: external}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	output := toolResultText(t, result)
	// External paths display as their canonical absolute path.
	wantHeader := "path: " + filepath.ToSlash(workspacepkg.Canonicalize(external))
	if !strings.HasPrefix(output, wantHeader) {
		t.Fatalf("output header = %q, want prefix %q", strings.Split(output, "\n")[0], wantHeader)
	}
	if !strings.Contains(output, "     1→external\n     2→content\n") {
		t.Fatalf("output missing external content: %q", output)
	}
}

// Read alignment never opens credential locations: home-rooted secrets and
// sensitive components stay denied wherever they live.
func TestReadFileToolDeniesExternalSensitivePath(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	validator, _ := newValidator(t)

	tool, err := NewReadFileTool(validator, nil)
	if err != nil {
		t.Fatalf("NewReadFileTool() error = %v", err)
	}
	targets := []string{
		filepath.Join(home, ".ssh", "id_rsa"),     // home-rooted sensitive directory
		filepath.Join(home, ".netrc"),             // home-rooted sensitive file
		filepath.Join(t.TempDir(), "app", ".env"), // sensitive component anywhere
	}
	for _, target := range targets {
		mustWriteFile(t, target, []byte("secret"))
		if _, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", readFileArgs{Path: target})); err == nil {
			t.Errorf("Prepare(%q) must be denied", target)
		} else {
			assertAgentErrorCode(t, err, domain.ErrSecurity)
		}
	}
}

func TestViewImageToolReadsExternalPath(t *testing.T) {
	validator, _ := newValidator(t)
	external := filepath.Join(t.TempDir(), "pixel.png")
	mustWriteFile(t, external, mustPNG(t, 2, 2))

	tool, err := NewViewImageTool(validator)
	if err != nil {
		t.Fatalf("NewViewImageTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "view_image", viewImageArgs{Path: external}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if len(result.Content) != 2 || result.Content[0].Kind != domain.PartText || result.Content[1].Kind != domain.PartImage {
		t.Fatalf("result.Content = %+v, want text+image parts", result.Content)
	}
	output := result.Content[0].Text
	wantHeader := "image: " + filepath.ToSlash(workspacepkg.Canonicalize(external)) + " · image/png"
	if !strings.HasPrefix(output, wantHeader) {
		t.Fatalf("output header = %q, want prefix %q", strings.Split(output, "\n")[0], wantHeader)
	}
}

// Regression: a missing path must be named in the error — with parallel
// tool calls a bare "path does not exist" leaves the model guessing which
// call failed (observed in a real session: search on a hallucinated
// internal/config/example.go).
func TestResolveExistingPathErrorNamesPath(t *testing.T) {
	validator, _ := newValidator(t)

	_, err := resolveExistingPath(validator, "internal/config/example.go")
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	if !strings.Contains(err.Error(), `path does not exist: "internal/config/example.go"`) {
		t.Fatalf("error = %v, want the offending path named", err)
	}

	// Multi-component so every segment stays under the filename limit and
	// os.Stat reports ENOENT rather than ENAMETOOLONG.
	long := strings.Repeat("aaaa/", 60) + "tail.go"
	_, err = resolveExistingPath(validator, long)
	assertAgentErrorCode(t, err, domain.ErrInvalidInput)
	if strings.Contains(err.Error(), long) || !strings.Contains(err.Error(), "...") {
		t.Fatalf("error must truncate pathological paths: %v", err)
	}
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

// The search path may point to a single file (models routinely scope a
// search to one file; rg accepts file targets natively).
func TestSearchAcceptsSingleFilePath(t *testing.T) {
	validator, root := newValidator(t)
	target := filepath.Join(root, "docs", "guide.md")
	mustWriteFile(t, target, []byte("nothing here\nbind to host\nlast line\n"))
	mustWriteFile(t, filepath.Join(root, "docs", "other.md"), []byte("bind elsewhere\n"))

	tool, err := NewSearchTool(validator, nil) // nil runner → go fallback
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "bind",
		Path:    target,
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
	if output.MatchCount != 1 || len(output.Matches) != 1 {
		t.Fatalf("matches = %+v, want exactly the single-file hit", output.Matches)
	}
	if output.Matches[0].Path != "docs/guide.md" || output.Matches[0].Line != 2 {
		t.Fatalf("match = %+v, want docs/guide.md:2", output.Matches[0])
	}
}

// Regression (REVIEW M16): the go fallback used to silently ignore glob
// filters, returning matches from files the model explicitly excluded.
func TestSearchGoFallbackAppliesGlobFilters(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "a.go"), []byte("hello go\n"))
	mustWriteFile(t, filepath.Join(root, "b.txt"), []byte("hello txt\n"))
	mustWriteFile(t, filepath.Join(root, "sub", "c.go"), []byte("hello sub\n"))

	tool, err := NewSearchTool(validator, nil) // nil runner → go fallback
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "hello",
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
	if output.MatchCount != 2 {
		t.Fatalf("MatchCount = %d, want 2 (only .go files): %+v", output.MatchCount, output.Matches)
	}
	for _, m := range output.Matches {
		if !strings.HasSuffix(m.Path, ".go") {
			t.Fatalf("glob filter leaked non-go match: %+v", m)
		}
	}

	// Negation excludes; exclusion wins over inclusion.
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "hello",
		Glob:    []string{"*.go", "!sub/**"},
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result = tool.Execute(context.Background(), prepared)
	decodeToolResult(t, result, &output)
	if output.MatchCount != 1 || output.Matches[0].Path != "a.go" {
		t.Fatalf("negation glob failed: %+v", output.Matches)
	}
}

// Regression (REVIEW M16): filters the fallback cannot honor (type,
// ignore rules) must be disclosed in the output note.
func TestSearchGoFallbackNotesUnappliedFilters(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "a.go"), []byte("hello\n"))

	tool, err := NewSearchTool(validator, nil)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "hello",
		Type:    "go",
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	var output searchOutput
	decodeToolResult(t, result, &output)
	if !strings.Contains(output.Note, "type filter") || !strings.Contains(output.Note, "not applied") {
		t.Fatalf("note must disclose the unapplied type filter: %q", output.Note)
	}
	if !strings.Contains(output.Note, "gitignore") {
		t.Fatalf("note must disclose unapplied ignore rules: %q", output.Note)
	}
}

// Regression: models serialize unset fields as "null"/"none" or double-encode
// quotes ("\"go\""); the type filter must normalize these instead of feeding
// the garbage to rg (observed: rg exit 2 "unrecognized file type: null").
func TestSearchTypeFilterNormalization(t *testing.T) {
	validator, _ := newValidator(t)
	cases := map[string]string{
		"null":   "",
		"NULL":   "",
		"none":   "",
		`"null"`: "",
		`"go"`:   "go",
		"'rust'": "rust",
		"  go  ": "go",
	}
	for in, want := range cases {
		args, _, err := validateSearchArgs(validator, searchArgs{Pattern: "x", Type: in})
		if err != nil {
			t.Fatalf("validateSearchArgs(type=%q) error = %v", in, err)
		}
		if args.Type != want {
			t.Fatalf("validateSearchArgs(type=%q) = %q, want %q", in, args.Type, want)
		}
	}
}

// Regression: a syntax-valid but unknown type name must be rejected at
// prepare time with recovery guidance, not handed to rg to die with exit 2.
func TestSearchTypeFilterUnknownRejected(t *testing.T) {
	if rgTypeSet() == nil {
		t.Skip("ripgrep unavailable; type-list validation disabled")
	}
	validator, _ := newValidator(t)
	_, _, err := validateSearchArgs(validator, searchArgs{Pattern: "x", Type: "notarealtype"})
	if err == nil || !strings.Contains(err.Error(), "unknown ripgrep type") {
		t.Fatalf("validateSearchArgs() error = %v, want unknown ripgrep type", err)
	}
	if !strings.Contains(err.Error(), "glob") {
		t.Fatalf("error must suggest the glob alternative: %v", err)
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

// Regression: searching an absolute path OUTSIDE the workspace must pass
// that path through as the rg working directory. The process runner accepts
// such a cwd under the read boundary (ValidateRead); an earlier write-boundary
// (Validate) check rejected it and surfaced as an opaque "internal tool
// error" for every out-of-workspace search.
func TestSearchRipgrepOutsideWorkspaceRoot(t *testing.T) {
	stubRgLocator(t)
	validator, _ := newValidator(t)
	// Canonicalize: macOS /var symlinks into /private/var, and the path
	// validator resolves to the canonical form the runner will see.
	outside := workspacepkg.Canonicalize(t.TempDir())
	mustWriteFile(t, filepath.Join(outside, "notes.txt"), []byte("hello outside\n"))

	match := `{"type":"match","data":{"path":{"text":"` + filepath.Join(outside, "notes.txt") + `"},"line_number":1,"lines":{"text":"hello outside\n"}}}`
	runner := &fakeRgRunner{result: process.Result{ExitCode: 0, Stdout: []byte(match + "\n")}}

	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{
		Pattern: "hello",
		Path:    outside,
	}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if runner.lastSpec.Cwd != outside {
		t.Fatalf("rg cwd = %q, want outside-workspace root %q", runner.lastSpec.Cwd, outside)
	}
	argv := runner.lastSpec.Args
	if n := len(argv); n < 2 || argv[n-1] != outside {
		t.Fatalf("rg argv must end with the outside-workspace root %q: %v", outside, argv)
	}

	var output searchOutput
	decodeToolResult(t, result, &output)
	if output.MatchCount != 1 || output.Matches[0].Text != "hello outside" {
		t.Fatalf("unexpected output: %+v", output)
	}
}

// Regression (REVIEW M8): rg must cap emitted columns so multi-megabyte
// single-line files cannot blow the JSONL scanner's token limit.
func TestRgCommonArgsIncludesMaxColumns(t *testing.T) {
	args := rgCommonArgs(0, true, false, false, nil, "", 100)
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "--max-columns") {
		t.Fatalf("rg argv missing --max-columns: %v", args)
	}
}

// Regression (REVIEW M7): a truncated rg run used to surface as a generic
// "failed to parse ripgrep JSON output" error — the head/tail seam cuts a
// JSON line. Unparseable seam lines must be skipped and the result marked
// truncated instead.
func TestSearchRipgrepToleratesSeamCorruption(t *testing.T) {
	stubRgLocator(t)
	validator, _ := newValidator(t)
	match := `{"type":"match","data":{"path":{"text":"a.go"},"line_number":2,"lines":{"text":"func hello() {}\n"}}}`
	runner := &fakeRgRunner{result: process.Result{
		ExitCode:        0,
		Stdout:          []byte(match + "\n" + `{"type":"match","data":{"path":{"text":"cut-mid-json`),
		StdoutTruncated: true,
	}}

	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{Pattern: "hello"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output searchOutput
	decodeToolResult(t, result, &output)
	if output.MatchCount != 1 {
		t.Fatalf("output.MatchCount = %d, want 1 (seam line skipped)", output.MatchCount)
	}
	if !output.Truncated {
		t.Fatal("output.Truncated must be true when the runner truncated rg output")
	}
}

// Regression (REVIEW M7): glob must propagate the runner's truncation flag
// instead of claiming a complete listing, and drop the spliced seam line.
func TestGlobRipgrepPropagatesTruncation(t *testing.T) {
	stubRgLocator(t)
	validator, _ := newValidator(t)
	runner := &fakeRgRunner{result: process.Result{
		ExitCode:        0,
		Stdout:          []byte("a.go\nb.go\n"),
		StdoutTruncated: true,
	}}

	tool, err := NewGlobTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGlobTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "glob", globArgs{Pattern: "*.go"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	var output globOutput
	decodeToolResult(t, result, &output)
	if !output.Truncated {
		t.Fatal("output.Truncated must be true when the runner truncated rg output")
	}
}

func TestTrimPreviewSeamDropsPartialLines(t *testing.T) {
	// seam=14 splits "partial.go": head keeps complete lines only, the
	// tail's first line (the continuation) is dropped too.
	preview := []byte("a.go\nb.go\npartial.go\nc.go\n")
	out := trimPreviewSeam(preview, 14)
	if got, want := string(out), "a.go\nb.go\nc.go\n"; got != want {
		t.Fatalf("trimPreviewSeam = %q, want %q", got, want)
	}
	// Previews shorter than the seam pass through untouched.
	short := []byte("x.go\n")
	if got := trimPreviewSeam(short, 1<<20); string(got) != "x.go\n" {
		t.Fatalf("short preview changed: %q", got)
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

// Regression (REVIEW H5): on platforms where the sandbox fails closed (e.g.
// Linux), runner.Run always fails with ErrSandboxUnavailable. search must
// degrade to the Go engine instead of erroring out.
func TestSearchFallsBackWhenSandboxUnavailable(t *testing.T) {
	stubRgLocator(t)
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "src", "a.go"), []byte("package src\nfunc hello() {}\n"))

	runner := &fakeRgRunner{err: fmt.Errorf("prepare sandbox: %w", process.ErrSandboxUnavailable)}
	tool, err := NewSearchTool(validator, runner)
	if err != nil {
		t.Fatalf("NewSearchTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "search", searchArgs{Pattern: "hello", Path: "src"}))
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
	if output.MatchCount != 1 {
		t.Fatalf("output.MatchCount = %d, want 1", output.MatchCount)
	}
}

// Regression (REVIEW H5): same sandbox-unavailable fallback for glob.
func TestGlobFallsBackWhenSandboxUnavailable(t *testing.T) {
	stubRgLocator(t)
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "src", "a.go"), []byte("package src\n"))

	runner := &fakeRgRunner{err: fmt.Errorf("%w: linux sandbox is not implemented", process.ErrSandboxUnavailable)}
	tool, err := NewGlobTool(validator, runner)
	if err != nil {
		t.Fatalf("NewGlobTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "glob", globArgs{Pattern: "*.go", Path: "src"}))
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
	if output.Count != 1 || output.Files[0] != "src/a.go" {
		t.Fatalf("unexpected glob output: %+v", output)
	}
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

// Regression: the go fallback used to require pattern segments to align with
// the full relative path, so a bare '*.go' matched only root-level files
// while the ripgrep engine matched the basename at any depth — the same
// call silently returned different results depending on the engine. The
// fallback now shares the ripgrep semantics (matchSearchGlob).
func TestGlobGoFallbackMatchesBasenameAtAnyDepth(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "root.go"), []byte("package a\n"))
	mustWriteFile(t, filepath.Join(root, "sub", "deep", "b.go"), []byte("package b\n"))
	mustWriteFile(t, filepath.Join(root, "sub", "c.txt"), []byte("text\n"))

	tool, err := NewGlobTool(validator, nil)
	if err != nil {
		t.Fatalf("NewGlobTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "glob", globArgs{Pattern: "*.go"}))
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
	want := []string{"root.go", "sub/deep/b.go"}
	if !reflect.DeepEqual(output.Files, want) {
		t.Fatalf("output.Files = %v, want %v (basename pattern must match at any depth)", output.Files, want)
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

// toolResultText extracts the single plain-text part of a tool result.
func toolResultText(t *testing.T, result domain.ToolResult) string {
	t.Helper()
	if len(result.Content) != 1 {
		t.Fatalf("len(result.Content) = %d, want 1", len(result.Content))
	}
	if result.Content[0].Kind != domain.PartText {
		t.Fatalf("result.Content[0].Kind = %s, want text", result.Content[0].Kind)
	}
	return result.Content[0].Text
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

// --- view_image ---

func mustPNG(t *testing.T, width, height int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("png.Encode() error = %v", err)
	}
	return buf.Bytes()
}

func TestViewImageToolReturnsImagePart(t *testing.T) {
	validator, root := newValidator(t)
	pngData := mustPNG(t, 4, 2)
	mustWriteFile(t, filepath.Join(root, "shot.png"), pngData)

	tool, err := NewViewImageTool(validator)
	if err != nil {
		t.Fatalf("NewViewImageTool() error = %v", err)
	}
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "view_image", viewImageArgs{Path: "shot.png"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if prepared.Risk != domain.R1 {
		t.Fatalf("Risk = %v, want R1", prepared.Risk)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, want success: %+v", result.Status, result.Error)
	}
	if len(result.Content) != 2 {
		t.Fatalf("len(Content) = %d, want 2 (text header + image)", len(result.Content))
	}
	header := result.Content[0]
	if header.Kind != domain.PartText || !strings.Contains(header.Text, "image/png") || !strings.Contains(header.Text, "4x2") {
		t.Fatalf("header part = %+v", header)
	}
	// 与 browser 截图一致的契约：告知模型图片已展示给用户，禁止再贴 markdown 链接。
	if !strings.Contains(header.Text, "already displayed to the user") || !strings.Contains(header.Text, "do not embed it as a markdown link") {
		t.Fatalf("header missing display contract note: %q", header.Text)
	}
	imagePart := result.Content[1]
	if imagePart.Kind != domain.PartImage || imagePart.Image == nil {
		t.Fatalf("image part = %+v", imagePart)
	}
	if imagePart.Image.MediaType != "image/png" {
		t.Fatalf("MediaType = %q, want image/png", imagePart.Image.MediaType)
	}
	decoded, err := base64.StdEncoding.DecodeString(imagePart.Image.Data)
	if err != nil || !bytes.Equal(decoded, pngData) {
		t.Fatalf("base64 round-trip mismatch (err=%v)", err)
	}
}

func TestViewImageToolRejectsNonImageAndOversize(t *testing.T) {
	validator, root := newValidator(t)
	mustWriteFile(t, filepath.Join(root, "text.txt"), []byte("not an image"))
	tool, err := NewViewImageTool(validator)
	if err != nil {
		t.Fatalf("NewViewImageTool() error = %v", err)
	}

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "view_image", viewImageArgs{Path: "text.txt"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(domain.ErrInvalidInput) {
		t.Fatalf("result = %+v, want invalid input for non-image", result)
	}

	// A file with valid magic but beyond the byte budget.
	big := append(mustPNG(t, 1, 1), make([]byte, maxImageBytes)...)
	mustWriteFile(t, filepath.Join(root, "big.png"), big)
	prepared, err = tool.Prepare(context.Background(), newToolCall(t, "view_image", viewImageArgs{Path: "big.png"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	result = tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(domain.ErrInvalidInput) {
		t.Fatalf("result = %+v, want invalid input for oversize image", result)
	}
}

func TestDetectImageMediaType(t *testing.T) {
	tests := []struct {
		name string
		data []byte
		want string
	}{
		{"png", mustPNG(t, 1, 1), "image/png"},
		{"jpeg", []byte{0xFF, 0xD8, 0xFF, 0xE0, 0, 0}, "image/jpeg"},
		{"gif87", []byte("GIF87a...."), "image/gif"},
		{"gif89", []byte("GIF89a...."), "image/gif"},
		{"webp", []byte("RIFF\x00\x00\x00\x00WEBP"), "image/webp"},
		{"text", []byte("hello world"), ""},
		{"short", []byte{0x89}, ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := detectImageMediaType(tt.data); got != tt.want {
				t.Fatalf("detectImageMediaType() = %q, want %q", got, tt.want)
			}
		})
	}
}
