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

package edit

import (
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
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxTextFileBytes    int64 = 1 << 20
	maxPatchBytes             = 1 << 20
	maxReplacementBytes       = 256 << 10
)

type baseTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	key       [32]byte
}

type preparedFingerprint struct {
	CallID     string           `json:"call_id"`
	ToolName   string           `json:"tool_name"`
	Arguments  json.RawMessage  `json:"arguments"`
	WritePaths []string         `json:"write_paths,omitempty"`
	Risk       domain.RiskLevel `json:"risk"`
}

type editOutput struct {
	Path    string `json:"path"`
	OldHash string `json:"old_hash"`
	NewHash string `json:"new_hash"`
	Size    int64  `json:"size"`
}

type fileLine struct {
	Text       string
	HasNewline bool
}

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
	writePaths []string,
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
		WritePaths:   sortedStrings(writePaths),
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

func resolveWritePath(validator *workspacepkg.PathValidator, input string) (workspacepkg.ResolvedPath, error) {
	if strings.TrimSpace(input) == "" {
		return workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	resolved, err := validator.ResolveLexical(input)
	if err != nil {
		return workspacepkg.ResolvedPath{}, domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid", domain.WithCause(err))
	}
	return resolved, nil
}

func ensureExistingTextFile(validator *workspacepkg.PathValidator, input string) (workspacepkg.ResolvedPath, workspacepkg.Snapshot, []byte, error) {
	resolved, err := resolveWritePath(validator, input)
	if err != nil {
		return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, err
	}
	snapshot, err := validator.Snapshot(resolved.Absolute)
	if err != nil {
		if os.IsNotExist(err) {
			return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, "path does not exist", domain.WithCause(err))
		}
		return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrSecurity, "path is not a writable regular file", domain.WithCause(err))
	}

	data, err := os.ReadFile(resolved.Absolute)
	if err != nil {
		return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrUnavailable, "failed to read file", domain.WithCause(err))
	}
	if int64(len(data)) > maxTextFileBytes {
		return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("file exceeds size limit of %d bytes", maxTextFileBytes))
	}
	if bytes.IndexByte(data, 0) >= 0 || !utf8.Valid(data) {
		return workspacepkg.ResolvedPath{}, workspacepkg.Snapshot{}, nil, domain.NewError(domain.ErrInvalidInput, "file appears to be binary or not valid UTF-8")
	}
	return resolved, snapshot, data, nil
}

func canonicalizeHash(value string) (string, error) {
	if value == "" {
		return "", domain.NewError(domain.ErrInvalidInput, "expected_hash is required")
	}
	if len(value) != 64 {
		return "", domain.NewError(domain.ErrInvalidInput, "expected_hash must be a lowercase SHA256 hex string")
	}
	for _, r := range value {
		if (r < '0' || r > '9') && (r < 'a' || r > 'f') {
			return "", domain.NewError(domain.ErrInvalidInput, "expected_hash must be a lowercase SHA256 hex string")
		}
	}
	return value, nil
}

func normalizeAtomicWriteError(err error) error {
	if err == nil {
		return nil
	}
	if strings.Contains(err.Error(), "expected hash mismatch") {
		return domain.NewError(domain.ErrConflict, "file changed since expected_hash was computed", domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "failed to write file atomically", domain.WithCause(err))
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

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func splitFileLines(data []byte) []fileLine {
	if len(data) == 0 {
		return nil
	}
	text := string(data)
	lines := make([]fileLine, 0, strings.Count(text, "\n")+1)
	start := 0
	for i, r := range text {
		if r != '\n' {
			continue
		}
		lines = append(lines, fileLine{Text: text[start:i], HasNewline: true})
		start = i + 1
	}
	if start < len(text) {
		lines = append(lines, fileLine{Text: text[start:], HasNewline: false})
	}
	return lines
}

func joinFileLines(lines []fileLine) ([]byte, error) {
	if len(lines) == 0 {
		return []byte{}, nil
	}
	var buf bytes.Buffer
	for i, line := range lines {
		if !line.HasNewline && i != len(lines)-1 {
			return nil, domain.NewError(domain.ErrInvalidInput, "only the last line may omit a trailing newline")
		}
		buf.WriteString(line.Text)
		if line.HasNewline {
			buf.WriteByte('\n')
		}
	}
	return buf.Bytes(), nil
}
