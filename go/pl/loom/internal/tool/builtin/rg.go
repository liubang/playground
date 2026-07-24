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
// Created: 2026/07/24

package builtin

import (
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
	"strconv"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// searchEngine identifies which backend produced a search result.
type searchEngine string

const (
	engineRipgrep    searchEngine = "ripgrep"
	engineGoFallback searchEngine = "go_fallback"
)

const (
	rgTimeout      = 30 * time.Second
	rgOutputLimit  = 1 << 20
	rgMaxCountHint = 500
)

// rgLocator probes for the ripgrep binary once per process.
var rgLocator = sync.OnceValues(func() (string, bool) {
	path, err := exec.LookPath("rg")
	return path, err == nil
})

// rgRunner abstracts process execution so tests can substitute fakes.
type rgRunner interface {
	Run(ctx context.Context, spec process.CommandSpec) (process.Result, error)
}

// rgAvailable reports whether ripgrep can be used through the given runner.
func rgAvailable(runner rgRunner) bool {
	if runner == nil {
		return false
	}
	_, ok := rgLocator()
	return ok
}

// rgEvent is one line of `rg --json` output (match/context/begin/end/summary).
type rgEvent struct {
	Type string `json:"type"`
	Data struct {
		Path struct {
			Text string `json:"text"`
		} `json:"path"`
		LineNumber int `json:"line_number"`
		Lines      struct {
			Text string `json:"text"`
		} `json:"lines"`
	} `json:"data"`
}

// runRipgrep executes rg with the given pre-built argument list through the
// sandboxed process runner and returns raw stdout. rg exit codes: 0 = matches
// found, 1 = no matches, 2 = error. A missing-match run is not an error.
func runRipgrep(ctx context.Context, runner rgRunner, cwd string, args []string) ([]byte, error) {
	result, err := runner.Run(ctx, process.CommandSpec{
		Program:     "rg",
		Args:        args,
		Cwd:         cwd,
		Env:         map[string]string{},
		Timeout:     rgTimeout,
		OutputLimit: rgOutputLimit,
	})
	if err != nil {
		return nil, err
	}
	switch result.ExitCode {
	case 0, 1:
		return result.Stdout, nil
	default:
		stderr := string(result.Stderr)
		if len(stderr) > 512 {
			stderr = stderr[:512] + "..."
		}
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("ripgrep failed (exit %d): %s", result.ExitCode, stderr))
	}
}

// decodeRgEvents parses `rg --json` JSONL output.
func decodeRgEvents(stdout []byte) ([]rgEvent, error) {
	lines, err := splitLines(stdout, maxSearchFileBytes)
	if err != nil {
		return nil, err
	}
	var events []rgEvent
	for _, line := range lines {
		if line == "" {
			continue
		}
		var evt rgEvent
		if err := json.Unmarshal([]byte(line), &evt); err != nil {
			return nil, domain.NewError(domain.ErrInternal, "failed to parse ripgrep JSON output", domain.WithCause(err))
		}
		events = append(events, evt)
	}
	return events, nil
}

// rgCommonArgs builds the shared rg argument prefix (search behavior flags).
// Pattern and path are appended by the caller after "--".
func rgCommonArgs(contextLines int, caseSensitive, fixedStrings, noIgnore bool, globs []string, fileType string, maxCount int) []string {
	args := []string{"--json", "--max-count", strconv.Itoa(maxCount)}
	if contextLines > 0 {
		args = append(args, "-C", strconv.Itoa(contextLines))
	}
	if !caseSensitive {
		args = append(args, "-i")
	}
	if fixedStrings {
		args = append(args, "-F")
	}
	if noIgnore {
		args = append(args, "--no-ignore")
	}
	for _, g := range globs {
		args = append(args, "--glob", g)
	}
	if fileType != "" {
		args = append(args, "--type", fileType)
	}
	return args
}
