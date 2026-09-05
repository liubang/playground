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
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// This file hosts the Go fallback engine of the grep tool (see search.go for
// the tool definition and ripgrep engine). The engine deliberately reuses the
// tool's own types — searchArgs/searchMatch/searchOutput — instead of a
// parallel set: it scans into the caller-provided output skeleton, and the
// tool-level concerns (engine label, glob filtering, head-limit truncation,
// the unapplied-filter note) stay in search.go.

// searchDirectory walks root.Absolute and fills out.Matches with every hit,
// sorted by path then line. Binary and oversized files are counted in the
// skip counters; sensitive locations are skipped as in the rg engine.
func searchDirectory(ctx context.Context, validator *workspacepkg.PathValidator, root pathResolution, args searchArgs, out *searchOutput) error {
	needle := args.Pattern
	if !args.CaseSensitive {
		needle = strings.ToLower(args.Pattern)
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
		// The component check covers names sensitive anywhere (.git, .env);
		// IsSensitiveAbsolute additionally covers home-rooted locations
		// (~/.kube, ~/.aws) when the walk root lies outside the workspace.
		if containsSensitiveComponent(rel) || workspacepkg.IsSensitiveAbsolute(path) {
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
			out.SkippedBinary++
			return nil
		case fileSearchTooLarge:
			out.SkippedTooLarge++
			return nil
		}
		out.ScannedFiles++
		if len(matches) == 0 {
			return nil
		}
		remaining := maxSearchMatches - len(out.Matches)
		if remaining <= 0 {
			out.Truncated = true
			return io.EOF
		}
		if len(matches) > remaining {
			matches = matches[:remaining]
			out.Truncated = true
			out.Matches = append(out.Matches, matches...)
			return io.EOF
		}
		out.Matches = append(out.Matches, matches...)
		return nil
	})
	if walkErr != nil && walkErr != io.EOF {
		return walkErr
	}

	sort.Slice(out.Matches, func(i, j int) bool {
		if out.Matches[i].Path != out.Matches[j].Path {
			return out.Matches[i].Path < out.Matches[j].Path
		}
		return out.Matches[i].Line < out.Matches[j].Line
	})
	out.MatchCount = len(out.Matches)
	return nil
}

// searchSingleFile runs the fallback matcher over one regular file (the
// search tool accepts single-file targets, matching rg semantics). Binary
// and oversized files are reported through the skip counters, exactly as in
// the directory walk; hard I/O failures propagate as errors.
func searchSingleFile(ctx context.Context, file pathResolution, args searchArgs, out *searchOutput) error {
	needle := args.Pattern
	if !args.CaseSensitive {
		needle = strings.ToLower(args.Pattern)
	}
	status, matches, err := searchFile(ctx, file, needle, args)
	if err != nil {
		return err
	}
	switch status {
	case fileSearchBinary:
		out.SkippedBinary = 1
	case fileSearchTooLarge:
		out.SkippedTooLarge = 1
	default:
		out.ScannedFiles = 1
		if len(matches) > maxSearchMatches {
			matches = matches[:maxSearchMatches]
			out.Truncated = true
		}
		out.Matches = append(out.Matches, matches...)
	}
	out.MatchCount = len(out.Matches)
	return nil
}

func searchFile(ctx context.Context, file pathResolution, needle string, args searchArgs) (fileSearchStatus, []searchMatch, error) {
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

	sample := make([]byte, toolkit.BinarySampleBytes)
	n, sampleErr := opened.Read(sample)
	if sampleErr != nil && sampleErr != io.EOF {
		return fileSearchScanned, nil, domain.NewError(domain.ErrUnavailable, "failed to inspect file", domain.WithCause(sampleErr))
	}
	if toolkit.IsBinaryContent(sample[:n]) {
		return fileSearchBinary, nil, nil
	}
	if _, err := opened.Seek(0, io.SeekStart); err != nil {
		return fileSearchScanned, nil, domain.NewError(domain.ErrUnavailable, "failed to reset file reader", domain.WithCause(err))
	}

	lines, err := readSearchLines(ctx, opened)
	if err != nil {
		return fileSearchScanned, nil, err
	}

	matches := make([]searchMatch, 0)
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
		match := searchMatch{
			Path: file.Display,
			Line: idx + 1,
			Text: line,
		}
		if args.Context > 0 {
			start := max(0, idx-args.Context)
			match.Before = make([]contextLine, 0, idx-start)
			for i := start; i < idx; i++ {
				match.Before = append(match.Before, contextLine{Line: i + 1, Text: lines[i]})
			}
			end := min(len(lines), idx+1+args.Context)
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
