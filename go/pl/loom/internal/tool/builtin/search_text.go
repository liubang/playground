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
	"bufio"
	"context"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

type searchTextArgs struct {
	Path          string `json:"path"`
	Query         string `json:"query"`
	CaseSensitive bool   `json:"case_sensitive,omitempty"`
	Before        int    `json:"before,omitempty"`
	After         int    `json:"after,omitempty"`
}

type searchTextMatch struct {
	Path   string        `json:"path"`
	Line   int           `json:"line"`
	Text   string        `json:"text"`
	Before []contextLine `json:"before,omitempty"`
	After  []contextLine `json:"after,omitempty"`
}

type searchTextOutput struct {
	Path            string            `json:"path"`
	Query           string            `json:"query"`
	CaseSensitive   bool              `json:"case_sensitive"`
	Before          int               `json:"before"`
	After           int               `json:"after"`
	MatchCount      int               `json:"match_count"`
	Truncated       bool              `json:"truncated"`
	ScannedFiles    int               `json:"scanned_files"`
	SkippedBinary   int               `json:"skipped_binary"`
	SkippedTooLarge int               `json:"skipped_too_large"`
	Matches         []searchTextMatch `json:"matches"`
}

// This file now hosts only the Go fallback search engine used by the search
// tool when ripgrep is unavailable (see search.go for the tool definition).

func searchDirectory(ctx context.Context, validator *workspacepkg.PathValidator, root pathResolution, args searchTextArgs) (searchTextOutput, error) {
	output := searchTextOutput{
		Path:          args.Path,
		Query:         args.Query,
		CaseSensitive: args.CaseSensitive,
		Before:        args.Before,
		After:         args.After,
		Matches:       []searchTextMatch{},
	}

	needle := args.Query
	if !args.CaseSensitive {
		needle = strings.ToLower(args.Query)
	}

	walkErr := filepath.WalkDir(root.Absolute, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return domain.NewError(domain.ErrUnavailable, "failed to walk directory", domain.WithCause(err))
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if path == root.Absolute {
			return nil
		}
		if d.Type()&os.ModeSymlink != 0 {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		rel, err := filepath.Rel(root.Absolute, path)
		if err != nil {
			return domain.NewError(domain.ErrInternal, "failed to normalize walked path", domain.WithCause(err))
		}
		if containsSensitiveComponent(rel) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}

		resolved, err := resolveExistingPath(validator, path)
		if err != nil {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}

		status, matches, err := searchFile(ctx, resolved, needle, args)
		if err != nil {
			return err
		}
		switch status {
		case fileSearchBinary:
			output.SkippedBinary++
			return nil
		case fileSearchTooLarge:
			output.SkippedTooLarge++
			return nil
		}
		output.ScannedFiles++
		if len(matches) == 0 {
			return nil
		}
		remaining := maxSearchMatches - len(output.Matches)
		if remaining <= 0 {
			output.Truncated = true
			return io.EOF
		}
		if len(matches) > remaining {
			matches = matches[:remaining]
			output.Truncated = true
			output.Matches = append(output.Matches, matches...)
			return io.EOF
		}
		output.Matches = append(output.Matches, matches...)
		return nil
	})
	if walkErr != nil && walkErr != io.EOF {
		return searchTextOutput{}, walkErr
	}

	sort.Slice(output.Matches, func(i, j int) bool {
		if output.Matches[i].Path != output.Matches[j].Path {
			return output.Matches[i].Path < output.Matches[j].Path
		}
		return output.Matches[i].Line < output.Matches[j].Line
	})
	output.MatchCount = len(output.Matches)
	return output, nil
}

func searchFile(ctx context.Context, file pathResolution, needle string, args searchTextArgs) (fileSearchStatus, []searchTextMatch, error) {
	if !file.Info.Mode().IsRegular() {
		return fileSearchScanned, nil, nil
	}
	if file.Info.Size() > maxSearchFileBytes {
		return fileSearchTooLarge, nil, nil
	}

	opened, err := os.Open(file.Absolute)
	if err != nil {
		return fileSearchScanned, nil, domain.NewError(domain.ErrUnavailable, "failed to open file", domain.WithCause(err))
	}
	defer opened.Close()

	sample := make([]byte, binarySampleBytes)
	n, sampleErr := opened.Read(sample)
	if sampleErr != nil && sampleErr != io.EOF {
		return fileSearchScanned, nil, domain.NewError(domain.ErrUnavailable, "failed to inspect file", domain.WithCause(sampleErr))
	}
	if isBinaryContent(sample[:n]) {
		return fileSearchBinary, nil, nil
	}
	if _, err := opened.Seek(0, io.SeekStart); err != nil {
		return fileSearchScanned, nil, domain.NewError(domain.ErrUnavailable, "failed to reset file reader", domain.WithCause(err))
	}

	lines, err := readSearchLines(ctx, opened)
	if err != nil {
		return fileSearchScanned, nil, err
	}

	matches := make([]searchTextMatch, 0)
	for idx, line := range lines {
		if err := ctx.Err(); err != nil {
			return fileSearchScanned, nil, err
		}
		haystack := line
		if !args.CaseSensitive {
			haystack = strings.ToLower(line)
		}
		if !strings.Contains(haystack, needle) {
			continue
		}
		match := searchTextMatch{
			Path: file.Display,
			Line: idx + 1,
			Text: line,
		}
		if args.Before > 0 {
			start := maxInt(0, idx-args.Before)
			match.Before = make([]contextLine, 0, idx-start)
			for i := start; i < idx; i++ {
				match.Before = append(match.Before, contextLine{Line: i + 1, Text: lines[i]})
			}
		}
		if args.After > 0 {
			end := minInt(len(lines), idx+1+args.After)
			match.After = make([]contextLine, 0, end-(idx+1))
			for i := idx + 1; i < end; i++ {
				match.After = append(match.After, contextLine{Line: i + 1, Text: lines[i]})
			}
		}
		matches = append(matches, match)
	}
	return fileSearchScanned, matches, nil
}

func readSearchLines(ctx context.Context, reader io.Reader) ([]string, error) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 0, 4096), maxSearchFileBytes)
	lines := make([]string, 0)
	for scanner.Scan() {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		lines = append(lines, scanner.Text())
	}
	if err := scanner.Err(); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to scan file", domain.WithCause(err))
	}
	return lines, nil
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
