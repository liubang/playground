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
	"io"
	"io/fs"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const maxGlobResults = 200

type globArgs struct {
	Pattern string `json:"pattern"`
	Path    string `json:"path,omitempty"`
}

type globOutput struct {
	Path      string   `json:"path"`
	Pattern   string   `json:"pattern"`
	Engine    string   `json:"engine"`
	Files     []string `json:"files"`
	Count     int      `json:"count"`
	Truncated bool     `json:"truncated"`
}

// GlobTool implements pattern-based file discovery with a ripgrep engine and
// a Go fallback.
type GlobTool struct {
	base   baseTool
	runner rgRunner
}

// NewGlobTool creates a glob tool. A nil runner forces the Go fallback.
func NewGlobTool(validator *workspacepkg.PathValidator, runner rgRunner) (*GlobTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "glob",
		Description: "Find files by name pattern within the workspace (e.g. '**/*.go', 'src/**/test_*.ts'). " +
			"Returns workspace-relative paths in deterministic order, capped at 200. " +
			"Files matched by .gitignore are skipped when the ripgrep engine is available.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"pattern":{"type":"string","minLength":1,"maxLength":512},"path":{"type":"string","minLength":1}},"required":["pattern"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"},"pattern":{"type":"string"},"engine":{"type":"string"},"files":{"type":"array","items":{"type":"string"}},"count":{"type":"integer"},"truncated":{"type":"boolean"}},"required":["path","pattern","engine","files","count","truncated"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}, validator)
	if err != nil {
		return nil, err
	}
	return &GlobTool{base: base, runner: runner}, nil
}

func (t *GlobTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *GlobTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[globArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if strings.TrimSpace(args.Pattern) == "" || len(args.Pattern) > 512 {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "pattern must be 1..512 bytes")
	}
	if args.Path == "" {
		args.Path = "."
	}
	pathInfo, err := resolveExistingPath(t.base.validator, args.Path)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	if !pathInfo.Info.IsDir() {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "path must refer to a directory")
	}
	args.Path = pathInfo.Display

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Find files matching %q under %s", args.Pattern, args.Path)
	return t.base.prepareCall(ctx, call, canonical, []string{pathInfo.Absolute}, approvalDesc)
}

func (t *GlobTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if len(prepared.ReadPaths) != 1 {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call read paths are invalid"))
	}

	args, err := decodeStrict[globArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	pathInfo, err := resolveExistingPath(t.base.validator, prepared.ReadPaths[0])
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if pathInfo.Display != args.Path {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call path binding mismatch"))
	}

	if rgAvailable(t.runner) {
		return t.executeRipgrep(ctx, prepared, pathInfo, args, startedAt)
	}
	return t.executeGoFallback(ctx, prepared, pathInfo, args, startedAt)
}

// --- ripgrep engine ---

func (t *GlobTool) executeRipgrep(ctx context.Context, prepared domain.PreparedCall, root pathResolution, args globArgs, startedAt time.Time) domain.ToolResult {
	// rg --files lists candidate files relative to the working directory; the
	// glob pattern filters them. Fetch one extra entry to detect truncation.
	argv := []string{"--files", "--glob", args.Pattern, "--", "."}
	stdout, err := runRipgrep(ctx, t.runner, root.Absolute, argv)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	lines, err := splitLines(stdout, maxSearchFileBytes)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	files := []string{}
	for _, line := range lines {
		if line == "" {
			continue
		}
		files = append(files, displayUnderRoot(root, line))
	}
	sort.Strings(files)
	truncated := false
	if len(files) > maxGlobResults {
		files = files[:maxGlobResults]
		truncated = true
	}
	return successResult(prepared.Call.ID, startedAt, globOutput{
		Path: args.Path, Pattern: args.Pattern, Engine: string(engineRipgrep),
		Files: files, Count: len(files), Truncated: truncated,
	})
}

// --- Go fallback engine ---

func (t *GlobTool) executeGoFallback(ctx context.Context, prepared domain.PreparedCall, root pathResolution, args globArgs, startedAt time.Time) domain.ToolResult {
	if _, err := path.Match(args.Pattern, ""); err != nil && !strings.Contains(args.Pattern, "**") {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrInvalidInput, "invalid glob pattern", domain.WithCause(err)))
	}

	files := []string{}
	truncated := false
	walkErr := filepath.WalkDir(root.Absolute, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if p == root.Absolute {
			return nil
		}
		rel, err := filepath.Rel(root.Absolute, p)
		if err != nil {
			return err
		}
		if containsSensitiveComponent(rel) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() || d.Type()&fs.ModeSymlink != 0 {
			return nil
		}
		if !matchGlobPath(args.Pattern, filepath.ToSlash(rel)) {
			return nil
		}
		if len(files) >= maxGlobResults {
			truncated = true
			return io.EOF
		}
		files = append(files, displayUnderRoot(root, p))
		return nil
	})
	if walkErr != nil && walkErr != io.EOF {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrUnavailable, "failed to walk directory", domain.WithCause(walkErr)))
	}

	sort.Strings(files)
	return successResult(prepared.Call.ID, startedAt, globOutput{
		Path: args.Path, Pattern: args.Pattern, Engine: string(engineGoFallback),
		Files: files, Count: len(files), Truncated: truncated,
	})
}

// matchGlobPath matches a slash-separated path against a glob pattern that
// may contain '**' segments crossing directory boundaries.
func matchGlobPath(pattern, name string) bool {
	return matchGlobSegments(strings.Split(pattern, "/"), strings.Split(name, "/"))
}

func matchGlobSegments(patSegs, nameSegs []string) bool {
	if len(patSegs) == 0 {
		return len(nameSegs) == 0
	}
	if patSegs[0] == "**" {
		for i := 0; i <= len(nameSegs); i++ {
			if matchGlobSegments(patSegs[1:], nameSegs[i:]) {
				return true
			}
		}
		return false
	}
	if len(nameSegs) == 0 {
		return false
	}
	ok, err := path.Match(patSegs[0], nameSegs[0])
	if err != nil || !ok {
		return false
	}
	return matchGlobSegments(patSegs[1:], nameSegs[1:])
}
