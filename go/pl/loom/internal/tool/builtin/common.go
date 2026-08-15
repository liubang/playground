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
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultReadFileOffset = 1
	defaultReadFileLimit  = 200
	maxReadFileLimit      = 500
	maxReadFileBytes      = 1 << 20

	maxDirectoryEntries = 200

	maxSearchContextLines = 5
	maxSearchMatches      = 200
	maxSearchFileBytes    = 1 << 20
	maxSearchQueryBytes   = 4096

	chunkSize = 32 << 10
)

// baseTool embeds the shared toolkit.BaseTool skeleton (definition +
// signer + prepare/verify protocol, REVIEW R3) plus the path validator
// every builtin file tool needs.
type baseTool struct {
	toolkit.BaseTool
	validator *workspacepkg.PathValidator
}

type contextLine struct {
	Line int    `json:"line"`
	Text string `json:"text"`
}

type pathResolution struct {
	Absolute string
	Display  string
	Info     os.FileInfo
}

type fileSearchStatus int

const (
	fileSearchScanned fileSearchStatus = iota
	fileSearchBinary
	fileSearchTooLarge
)

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

// malformedArgumentsKey marks the placeholder the agent loop substitutes
// when a provider's streamed tool-call arguments fail to reassemble into
// JSON (see agent.malformedArgumentsPlaceholder).
const malformedArgumentsKey = "__malformed_arguments"

// malformedArgumentsHint reports the placeholder's self-describing error
// when raw carries one. Routing the placeholder through the strict
// decoder instead would reject it on an unknown field whose name leaks
// an internal marker to the model; the placeholder already carries the
// actionable hint.
func malformedArgumentsHint(raw json.RawMessage) (string, bool) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		return "", false
	}
	payload, ok := fields[malformedArgumentsKey]
	if !ok {
		return "", false
	}
	hint := "model emitted invalid arguments JSON; re-issue the tool call with valid arguments"
	if head := strings.TrimSpace(string(payload)); len(head) > 200 {
		head = head[:200] + "…"
		hint += "; arguments began with " + head
	}
	return hint, true
}

// decodeStrict is the builtin variant of toolkit.DecodeStrict: it first
// routes the provider's malformed-arguments placeholder to a
// self-describing error instead of rejecting it as an unknown field.
func decodeStrict[T any](raw json.RawMessage) (T, error) {
	if hint, ok := malformedArgumentsHint(raw); ok {
		var zero T
		return zero, domain.NewError(domain.ErrInvalidInput, hint)
	}
	return toolkit.DecodeStrict[T](raw)
}

// resolveExistingPath resolves an existing filesystem path for the builtin
// READ tools. Paths inside the workspace keep their workspace-relative
// display form; absolute paths outside the workspace are readable too
// (PathValidator.ValidateRead mirrors the sandbox's broad read allowance)
// and display as their absolute path. Sensitive locations are denied
// everywhere by the validator.
func resolveExistingPath(validator *workspacepkg.PathValidator, input string) (pathResolution, error) {
	if strings.TrimSpace(input) == "" {
		return pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}

	resolved, err := validator.ValidateRead(input)
	if err != nil {
		return pathResolution{}, domain.NewError(domain.ErrSecurity, "path is not readable", domain.WithCause(err))
	}

	display := filepath.ToSlash(resolved)
	if rel, err := filepath.Rel(validator.Root(), resolved); err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		display = displayPath(rel)
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
		Display:  display,
		Info:     info,
	}, nil
}

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

// containsSensitiveComponent delegates to the single canonical list in the
// workspace package (REVIEW R4).
func containsSensitiveComponent(path string) bool {
	return workspacepkg.ContainsSensitiveComponent(path)
}

func splitLines(data []byte, maxToken int) ([]string, error) {
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(make([]byte, 0, 4096), maxToken)

	lines := make([]string, 0)
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	if err := scanner.Err(); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to read text content", domain.WithCause(err))
	}
	return lines, nil
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func readSmallFile(ctx context.Context, path string, maxBytes int64) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	file, err := os.Open(path)
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to open file", domain.WithCause(err))
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to stat file", domain.WithCause(err))
	}
	if !info.Mode().IsRegular() {
		return nil, domain.NewError(domain.ErrInvalidInput, "path must refer to a regular file")
	}
	if info.Size() > maxBytes {
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("file exceeds size limit of %d bytes", maxBytes))
	}

	var buffer bytes.Buffer
	if info.Size() > 0 {
		buffer.Grow(int(info.Size()))
	}

	tmp := make([]byte, chunkSize)
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		n, readErr := file.Read(tmp)
		if n > 0 {
			if int64(buffer.Len()+n) > maxBytes {
				return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("file exceeds size limit of %d bytes", maxBytes))
			}
			_, _ = buffer.Write(tmp[:n])
		}
		if errors.Is(readErr, io.EOF) {
			break
		}
		if readErr != nil {
			return nil, domain.NewError(domain.ErrUnavailable, "failed to read file", domain.WithCause(readErr))
		}
	}

	return buffer.Bytes(), nil
}
