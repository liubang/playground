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

package lint

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3) plus the path validator
// the lint tool needs for target resolution.
type baseTool struct {
	toolkit.BaseTool
	validator *workspacepkg.PathValidator
}

type pathResolution struct {
	Absolute string
	Display  string
	Info     os.FileInfo
}

func newBaseTool(def domain.ToolDefinition, validator *workspacepkg.PathValidator) (baseTool, error) {
	if validator == nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	bt, err := toolkit.NewBaseTool(def)
	if err != nil {
		return baseTool{}, err
	}
	return baseTool{BaseTool: bt, validator: validator}, nil
}

func resolveExistingPath(validator *workspacepkg.PathValidator, input string) (pathResolution, error) {
	if strings.TrimSpace(input) == "" {
		return pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	if rel, ok := lexicalWorkspaceRelativePath(validator, input); ok && containsSensitiveComponent(rel) {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	resolved, err := validator.Validate(input)
	if err != nil {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid", domain.WithCause(err))
	}

	rel, err := filepath.Rel(validator.Root(), resolved)
	if err != nil {
		return pathResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize path", domain.WithCause(err))
	}
	if containsSensitiveComponent(rel) {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	info, err := os.Stat(resolved)
	if err != nil {
		if os.IsNotExist(err) {
			return pathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path does not exist: %q", domain.TruncateForErrorEcho(input)))
		}
		return pathResolution{}, domain.NewError(domain.ErrUnavailable, "failed to stat path", domain.WithCause(err))
	}

	return pathResolution{
		Absolute: resolved,
		Display:  workspacepkg.DisplayPath(rel),
		Info:     info,
	}, nil
}

func lexicalWorkspaceRelativePath(validator *workspacepkg.PathValidator, input string) (string, bool) {
	clean := filepath.Clean(input)
	if !filepath.IsAbs(clean) {
		return clean, true
	}

	root := filepath.Clean(validator.Root())
	candidate := filepath.Clean(clean)
	if candidate != root && !strings.HasPrefix(candidate, root+string(filepath.Separator)) {
		return "", false
	}
	rel, err := filepath.Rel(root, candidate)
	if err != nil {
		return "", false
	}
	return rel, true
}

// containsSensitiveComponent delegates to the single canonical list in the
// workspace package (REVIEW R4).
func containsSensitiveComponent(path string) bool {
	return workspacepkg.ContainsSensitiveComponent(path)
}
