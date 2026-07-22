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
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultReadFileOffset = 1
	defaultReadFileLimit  = 200
	maxReadFileLimit      = 500
	maxReadFileBytes      = 1 << 20

	maxDirectoryEntries = 200

	defaultSearchContextLines = 0
	maxSearchContextLines     = 5
	maxSearchMatches          = 200
	maxSearchFileBytes        = 1 << 20
	maxSearchQueryBytes       = 4096

	chunkSize         = 32 << 10
	binarySampleBytes = 8 << 10
)

var sensitiveComponents = map[string]struct{}{
	".git":                 {},
	".ssh":                 {},
	".gnupg":               {},
	".env":                 {},
	".credentials":         {},
	"credentials.json":     {},
	"service-account.json": {},
}

type baseTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	key       [32]byte
}

type preparedFingerprint struct {
	CallID     string           `json:"call_id"`
	ToolName   string           `json:"tool_name"`
	Arguments  json.RawMessage  `json:"arguments"`
	ReadPaths  []string         `json:"read_paths,omitempty"`
	WritePaths []string         `json:"write_paths,omitempty"`
	Risk       domain.RiskLevel `json:"risk"`
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
	if err := def.Validate(); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}

	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}

	return baseTool{def: def, validator: validator, key: key}, nil
}

func (b *baseTool) prepareCall(
	ctx context.Context,
	call domain.ToolCall,
	canonicalArgs json.RawMessage,
	readPaths []string,
	approvalDesc string,
) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != b.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", b.def.Name))
	}

	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      b.def.Name,
			Arguments: cloneRawMessage(canonicalArgs),
		},
		Definition:   b.def,
		Risk:         b.def.Risk(),
		ApprovalDesc: approvalDesc,
		ReadPaths:    sortedStrings(readPaths),
	}
	prepared.ArgsHash = b.signPrepared(prepared)
	return prepared, nil
}

func (b *baseTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != b.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Name != b.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	if prepared.Definition.Source != b.def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	if prepared.Risk != b.def.Risk() {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	if !sameCapabilities(prepared.Definition.Capabilities, b.def.Capabilities) {
		return domain.NewError(domain.ErrSecurity, "prepared call capabilities mismatch")
	}

	expected := b.signPrepared(prepared)
	if !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func (b *baseTool) signPrepared(prepared domain.PreparedCall) string {
	fingerprint := preparedFingerprint{
		CallID:     prepared.Call.ID.String(),
		ToolName:   prepared.Call.Name,
		Arguments:  cloneRawMessage(prepared.Call.Arguments),
		ReadPaths:  append([]string(nil), prepared.ReadPaths...),
		WritePaths: append([]string(nil), prepared.WritePaths...),
		Risk:       prepared.Risk,
	}
	payload, _ := json.Marshal(fingerprint)

	h := hmac.New(sha256.New, b.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

func decodeStrict[T any](raw json.RawMessage) (T, error) {
	var out T
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&out); err != nil {
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must be valid JSON matching the tool schema", domain.WithCause(err))
	}

	var trailing struct{}
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value")
		}
		return out, domain.NewError(domain.ErrInvalidInput, "arguments must contain exactly one JSON value", domain.WithCause(err))
	}
	return out, nil
}

func cloneRawMessage(raw json.RawMessage) json.RawMessage {
	if raw == nil {
		return nil
	}
	return append(json.RawMessage(nil), raw...)
}

func sortedStrings(values []string) []string {
	if len(values) == 0 {
		return nil
	}
	out := append([]string(nil), values...)
	sort.Strings(out)
	return out
}

func sameCapabilities(left, right []domain.Capability) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
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
			return pathResolution{}, domain.NewError(domain.ErrInvalidInput, "path does not exist")
		}
		return pathResolution{}, domain.NewError(domain.ErrUnavailable, "failed to stat path", domain.WithCause(err))
	}

	return pathResolution{
		Absolute: resolved,
		Display:  displayPath(rel),
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

func containsSensitiveComponent(path string) bool {
	clean := filepath.Clean(path)
	if clean == "." || clean == string(filepath.Separator) {
		return false
	}
	for _, part := range strings.Split(clean, string(filepath.Separator)) {
		if _, ok := sensitiveComponents[part]; ok {
			return true
		}
	}
	return false
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

func isBinaryContent(data []byte) bool {
	sample := data
	if len(sample) > binarySampleBytes {
		sample = sample[:binarySampleBytes]
	}
	return bytes.IndexByte(sample, 0) >= 0 || !utf8.Valid(sample)
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

func successResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	content, err := json.Marshal(payload)
	if err != nil {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	finishedAt := time.Now()
	return domain.ToolResult{
		CallID:     callID,
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(content)}},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	status := domain.ToolStatusError
	code := string(domain.ErrInternal)
	message := "internal tool error"
	retryable := false

	switch {
	case errors.Is(err, context.Canceled):
		status = domain.ToolStatusCancelled
		code = string(domain.ErrCancelled)
		message = "operation cancelled"
	case errors.Is(err, context.DeadlineExceeded):
		status = domain.ToolStatusTimeout
		code = string(domain.ErrTimeout)
		message = "operation timed out"
	default:
		var agentErr *domain.AgentError
		if domain.As(err, &agentErr) {
			code = string(agentErr.Code)
			message = agentErr.Message
			retryable = agentErr.Retryable
			switch agentErr.Code {
			case domain.ErrCancelled:
				status = domain.ToolStatusCancelled
			case domain.ErrTimeout:
				status = domain.ToolStatusTimeout
			}
		}
	}

	finishedAt := time.Now()
	return domain.ToolResult{
		CallID: callID,
		Status: status,
		Error: &domain.ToolError{
			Code:      code,
			Message:   message,
			Retryable: retryable,
		},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}
