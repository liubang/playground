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

package command

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
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxProgramBytes                   = 4096
	maxArgsCount                      = 256
	maxArgBytes                       = 8192
	maxWorkingDirBytes                = 4096
	maxEnvVars                        = 64
	maxEnvKeyBytes                    = 256
	maxEnvValueBytes                  = 8192
	maxApprovalDescBytes              = 512
	approvalDescHashPrefixBytes       = 12
	minTimeoutMs                int64 = 1
	maxTimeoutMs                int64 = 10 * 60 * 1000
	maxOutputBytes              int64 = 1 << 20
)

type rawRunCommandArgs struct {
	Program        *string            `json:"program"`
	Args           *[]string          `json:"args"`
	WorkingDir     *string            `json:"working_dir"`
	Env            *map[string]string `json:"env"`
	TimeoutMs      *int64             `json:"timeout_ms"`
	MaxOutputBytes *int64             `json:"max_output_bytes"`
}

type runCommandArgs struct {
	Program        string            `json:"program"`
	Args           []string          `json:"args"`
	WorkingDir     string            `json:"working_dir"`
	Env            map[string]string `json:"env"`
	TimeoutMs      int64             `json:"timeout_ms"`
	MaxOutputBytes int64             `json:"max_output_bytes"`
}

type runCommandOutput struct {
	Stdout         string `json:"stdout"`
	Stderr         string `json:"stderr"`
	ExitCode       int    `json:"exit_code"`
	Signal         string `json:"signal"`
	DurationMs     int64  `json:"duration_ms"`
	TimedOut       bool   `json:"timed_out"`
	Cancelled      bool   `json:"cancelled"`
	Truncated      bool   `json:"truncated"`
	Isolation      string `json:"isolation"`
	ExecutablePath string `json:"executable_path"`
	Hash           string `json:"hash"`
}

type preparedFingerprint struct {
	CallID     string                `json:"call_id"`
	Arguments  json.RawMessage       `json:"arguments"`
	ReadPaths  []string              `json:"read_paths"`
	WritePaths []string              `json:"write_paths"`
	Risk       domain.RiskLevel      `json:"risk"`
	Definition domain.ToolDefinition `json:"definition"`
}

type resolvedWorkingDir struct {
	Absolute string
	Display  string
}

// RunCommandTool adapts process.Runner as the builtin run_command domain tool.
type RunCommandTool struct {
	def       domain.ToolDefinition
	validator *workspacepkg.PathValidator
	runner    *process.Runner
	key       [32]byte
}

// NewRunCommandTool creates a run_command tool bound to a workspace validator and process runner.
func NewRunCommandTool(
	validator *workspacepkg.PathValidator,
	runner *process.Runner,
) (*RunCommandTool, error) {
	if validator == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if runner == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "process runner is required")
	}
	def := domain.ToolDefinition{
		Name:         "run_command",
		Description:  "Execute a non-shell command using the configured process runner. Network access is not configurable here and defaults to denied by the runner sandbox.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"program":{"type":"string","minLength":1,"maxLength":4096},"args":{"type":"array","maxItems":256,"items":{"type":"string","maxLength":8192}},"working_dir":{"type":"string","minLength":1,"maxLength":4096},"env":{"type":"object","maxProperties":64,"additionalProperties":{"type":"string","maxLength":8192}},"timeout_ms":{"type":"integer","minimum":1,"maximum":600000},"max_output_bytes":{"type":"integer","minimum":1,"maximum":1048576}},"required":["program","args","working_dir","env","timeout_ms","max_output_bytes"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"stdout":{"type":"string"},"stderr":{"type":"string"},"exit_code":{"type":"integer"},"signal":{"type":"string"},"duration_ms":{"type":"integer"},"timed_out":{"type":"boolean"},"cancelled":{"type":"boolean"},"truncated":{"type":"boolean"},"isolation":{"type":"string"},"executable_path":{"type":"string"},"hash":{"type":"string"}},"required":["stdout","stderr","exit_code","signal","duration_ms","timed_out","cancelled","truncated","isolation","executable_path","hash"]}`),
		Capabilities: []domain.Capability{domain.CapProcessExec},
		Source:       domain.ToolSourceBuiltin,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}

	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return &RunCommandTool{
		def:       def,
		validator: validator,
		runner:    runner,
		key:       key,
	}, nil
}

func (t *RunCommandTool) Definition() domain.ToolDefinition {
	return t.def
}

func (t *RunCommandTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}

	rawArgs, err := decodeStrict[rawRunCommandArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, _, err := validateArgs(t.validator, rawArgs)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}

	root := t.validator.Root()
	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: cloneRawMessage(canonical),
		},
		Definition: t.def,
		Risk:       t.def.Risk(),
		ReadPaths:  []string{root},
		WritePaths: []string{root},
	}
	prepared.ApprovalDesc = buildApprovalDesc(args, prepared)
	prepared.ArgsHash = t.signPrepared(prepared)
	return prepared, nil
}

func (t *RunCommandTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if !hasOnlyWorkspaceRoot(prepared.ReadPaths, t.validator.Root()) || !hasOnlyWorkspaceRoot(prepared.WritePaths, t.validator.Root()) {
		return errorResult(prepared.Call.ID, startedAt, domain.NewError(domain.ErrSecurity, "prepared call workspace bindings are invalid"))
	}

	args, err := decodeStrict[runCommandArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	_, resolvedDir, err := validateCanonicalArgs(t.validator, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	runnerResult, err := t.runner.Run(ctx, process.CommandSpec{
		Program:     args.Program,
		Args:        append([]string(nil), args.Args...),
		Cwd:         resolvedDir.Absolute,
		Env:         cloneStringMap(args.Env),
		Timeout:     time.Duration(args.TimeoutMs) * time.Millisecond,
		OutputLimit: args.MaxOutputBytes,
	})
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, classifyRunError(err))
	}

	payload := runCommandOutput{
		Stdout:         sanitizeUTF8(runnerResult.Stdout),
		Stderr:         sanitizeUTF8(runnerResult.Stderr),
		ExitCode:       runnerResult.ExitCode,
		Signal:         runnerResult.Signal,
		DurationMs:     durationMilliseconds(runnerResult.Duration),
		TimedOut:       runnerResult.TimedOut,
		Cancelled:      runnerResult.Cancelled,
		Truncated:      runnerResult.Truncated,
		Isolation:      runnerResult.Isolation,
		ExecutablePath: runnerResult.ExecutablePath,
		Hash:           runnerResult.ExecutableHash,
	}
	switch {
	case runnerResult.TimedOut:
		return contentResult(prepared.Call.ID, domain.ToolStatusTimeout, startedAt, payload)
	case runnerResult.Cancelled:
		return contentResult(prepared.Call.ID, domain.ToolStatusCancelled, startedAt, payload)
	default:
		return contentResult(prepared.Call.ID, domain.ToolStatusSuccess, startedAt, payload)
	}
}

func (t *RunCommandTool) verifyPreparedCall(prepared domain.PreparedCall) error {
	if prepared.Call.Name != t.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if !sameDefinition(prepared.Definition, t.def) {
		return domain.NewError(domain.ErrSecurity, "prepared call definition mismatch")
	}
	if prepared.Risk != t.def.Risk() {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	if expected := t.signPrepared(prepared); !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func (t *RunCommandTool) signPrepared(prepared domain.PreparedCall) string {
	fingerprint := preparedFingerprint{
		CallID:     prepared.Call.ID.String(),
		Arguments:  cloneRawMessage(prepared.Call.Arguments),
		ReadPaths:  append([]string(nil), prepared.ReadPaths...),
		WritePaths: append([]string(nil), prepared.WritePaths...),
		Risk:       prepared.Risk,
		Definition: prepared.Definition,
	}
	payload, _ := json.Marshal(fingerprint)
	h := hmac.New(sha256.New, t.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

func validateArgs(
	validator *workspacepkg.PathValidator,
	raw rawRunCommandArgs,
) (runCommandArgs, resolvedWorkingDir, error) {
	if raw.Program == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program is required")
	}
	if raw.Args == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "args is required")
	}
	if raw.WorkingDir == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir is required")
	}
	if raw.Env == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "env is required")
	}
	if raw.TimeoutMs == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "timeout_ms is required")
	}
	if raw.MaxOutputBytes == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "max_output_bytes is required")
	}

	args := runCommandArgs{
		Program:        strings.TrimSpace(*raw.Program),
		Args:           append([]string(nil), (*raw.Args)...),
		Env:            cloneStringMap(*raw.Env),
		TimeoutMs:      *raw.TimeoutMs,
		MaxOutputBytes: *raw.MaxOutputBytes,
	}
	resolvedDir, err := resolveWorkingDir(validator, *raw.WorkingDir)
	if err != nil {
		return runCommandArgs{}, resolvedWorkingDir{}, err
	}
	args.WorkingDir = resolvedDir.Display
	return validateCanonicalArgs(validator, args)
}

func validateCanonicalArgs(
	validator *workspacepkg.PathValidator,
	args runCommandArgs,
) (runCommandArgs, resolvedWorkingDir, error) {
	if validator == nil {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if args.Program == "" {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program is required")
	}
	if len(args.Program) > maxProgramBytes {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("program exceeds %d bytes", maxProgramBytes))
	}
	if strings.ContainsRune(args.Program, 0) {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "program contains null byte")
	}
	if len(args.Args) > maxArgsCount {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args exceeds %d items", maxArgsCount))
	}
	for i, arg := range args.Args {
		if len(arg) > maxArgBytes {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] exceeds %d bytes", i, maxArgBytes))
		}
		if strings.ContainsRune(arg, 0) {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] contains null byte", i))
		}
	}
	resolvedDir, err := resolveWorkingDir(validator, args.WorkingDir)
	if err != nil {
		return runCommandArgs{}, resolvedWorkingDir{}, err
	}
	args.WorkingDir = resolvedDir.Display
	if args.TimeoutMs < minTimeoutMs || args.TimeoutMs > maxTimeoutMs {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("timeout_ms must be between %d and %d", minTimeoutMs, maxTimeoutMs))
	}
	if args.MaxOutputBytes < 1 || args.MaxOutputBytes > maxOutputBytes {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 1 and %d", maxOutputBytes))
	}
	if len(args.Env) > maxEnvVars {
		return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env exceeds %d entries", maxEnvVars))
	}
	canonicalEnv := make(map[string]string, len(args.Env))
	for key, value := range args.Env {
		if len(key) == 0 {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "env contains an empty key")
		}
		if len(key) > maxEnvKeyBytes {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q exceeds %d bytes", key, maxEnvKeyBytes))
		}
		if strings.ContainsAny(key, "=\x00") {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q is invalid", key))
		}
		if len(value) > maxEnvValueBytes {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q exceeds %d bytes", key, maxEnvValueBytes))
		}
		if strings.ContainsRune(value, 0) {
			return runCommandArgs{}, resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q contains null byte", key))
		}
		canonicalEnv[key] = value
	}
	args.Env = canonicalEnv
	return args, resolvedDir, nil
}

func resolveWorkingDir(
	validator *workspacepkg.PathValidator,
	input string,
) (resolvedWorkingDir, error) {
	if validator == nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if strings.TrimSpace(input) == "" {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir is required")
	}
	if len(input) > maxWorkingDirBytes {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir exceeds %d bytes", maxWorkingDirBytes))
	}
	absolute, err := validator.Validate(input)
	if err != nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(absolute)
	if err != nil {
		if os.IsNotExist(err) {
			return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir does not exist", domain.WithCause(err))
		}
		return resolvedWorkingDir{}, domain.NewError(domain.ErrUnavailable, "failed to stat working_dir", domain.WithCause(err))
	}
	if !info.IsDir() {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInvalidInput, "working_dir must be a directory")
	}
	rel, err := filepath.Rel(validator.Root(), absolute)
	if err != nil {
		return resolvedWorkingDir{}, domain.NewError(domain.ErrInternal, "failed to normalize working_dir", domain.WithCause(err))
	}
	return resolvedWorkingDir{Absolute: absolute, Display: displayPath(rel)}, nil
}

func classifyRunError(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, process.ErrSandboxRequired), errors.Is(err, process.ErrSandboxUnavailable):
		return domain.NewError(domain.ErrUnavailable, "process sandbox is unavailable", domain.WithCause(err))
	case errors.Is(err, process.ErrExecutableHashChanged):
		return domain.NewError(domain.ErrSecurity, "resolved executable changed before start", domain.WithCause(err))
	case errors.Is(err, process.ErrShellNotAllowed):
		return domain.NewError(domain.ErrInvalidInput, "shell execution is not allowed", domain.WithCause(err))
	case errors.Is(err, context.Canceled):
		return domain.NewError(domain.ErrCancelled, "operation cancelled", domain.WithCause(err))
	case errors.Is(err, context.DeadlineExceeded):
		return domain.NewError(domain.ErrTimeout, "operation timed out", domain.WithCause(err))
	}

	var agentErr *domain.AgentError
	if domain.As(err, &agentErr) {
		return err
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
	}
	if errors.Is(err, exec.ErrNotFound) || strings.Contains(err.Error(), "executable file not found") {
		return domain.NewError(domain.ErrInvalidInput, "program could not be resolved on PATH", domain.WithCause(err))
	}
	if strings.Contains(err.Error(), "validate cwd") {
		return domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	if strings.Contains(err.Error(), "stdout pipe") || strings.Contains(err.Error(), "stderr pipe") || strings.Contains(err.Error(), "start command") || strings.Contains(err.Error(), "wait command") {
		return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "command execution failed", domain.WithCause(err))
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

func contentResult(callID domain.ToolCallID, status domain.ToolStatus, startedAt time.Time, payload any) domain.ToolResult {
	content, err := json.Marshal(payload)
	if err != nil {
		return errorResult(callID, startedAt, domain.NewError(domain.ErrInternal, "failed to encode tool output", domain.WithCause(err)))
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     status,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: string(content)}},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
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

	return domain.ToolResult{
		CallID: callID,
		Status: status,
		Error: &domain.ToolError{
			Code:      code,
			Message:   message,
			Retryable: retryable,
		},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

func sameDefinition(left, right domain.ToolDefinition) bool {
	if left.Name != right.Name || left.Description != right.Description || left.Source != right.Source {
		return false
	}
	if string(left.InputSchema) != string(right.InputSchema) || string(left.OutputSchema) != string(right.OutputSchema) {
		return false
	}
	if len(left.Capabilities) != len(right.Capabilities) {
		return false
	}
	for i := range left.Capabilities {
		if left.Capabilities[i] != right.Capabilities[i] {
			return false
		}
	}
	return true
}

func cloneRawMessage(raw json.RawMessage) json.RawMessage {
	if raw == nil {
		return nil
	}
	return append(json.RawMessage(nil), raw...)
}

func cloneStringMap(values map[string]string) map[string]string {
	if len(values) == 0 {
		return map[string]string{}
	}
	out := make(map[string]string, len(values))
	for key, value := range values {
		out[key] = value
	}
	return out
}

func hasOnlyWorkspaceRoot(paths []string, root string) bool {
	if len(paths) != 1 {
		return false
	}
	return filepath.Clean(paths[0]) == filepath.Clean(root)
}

func sanitizeUTF8(data []byte) string {
	return string(bytes.ToValidUTF8(data, []byte("?")))
}

func durationMilliseconds(d time.Duration) int64 {
	if d <= 0 {
		return 0
	}
	return d.Milliseconds()
}

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func buildApprovalDesc(args runCommandArgs, prepared domain.PreparedCall) string {
	parts := []string{"Run"}
	command := append([]string{args.Program}, args.Args...)
	quoted := make([]string, 0, len(command))
	for _, item := range command {
		quoted = append(quoted, shellQuote(item))
	}
	parts = append(parts, strings.Join(quoted, " "))

	envKeys := sortedEnvKeys(args.Env)
	if len(envKeys) > 0 {
		parts = append(parts, "env["+strings.Join(envKeys, ", ")+"]")
	} else {
		parts = append(parts, "env[none]")
	}
	parts = append(parts, "cwd="+shellQuote(args.WorkingDir))
	parts = append(parts, fmt.Sprintf("timeout=%dms", args.TimeoutMs))
	parts = append(parts, "network=deny")

	base := strings.Join(parts, "; ")
	argsHash := prepared.ArgsHash
	if argsHash == "" {
		argsHash = shortArgsHash(prepared.Call.Arguments)
	}
	suffix := fmt.Sprintf("; args_hash=%s", argsHash)
	truncated := truncateWithMarker(base, maxApprovalDescBytes-len(suffix))
	return truncated + suffix
}

func sortedEnvKeys(env map[string]string) []string {
	keys := make([]string, 0, len(env))
	for key := range env {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func shellQuote(value string) string {
	if value == "" {
		return "''"
	}
	var b strings.Builder
	b.Grow(len(value) + 2)
	b.WriteByte('\'')
	for _, r := range value {
		if r == '\'' {
			b.WriteString(`'"'"'`)
			continue
		}
		b.WriteRune(r)
	}
	b.WriteByte('\'')
	return b.String()
}

func truncateWithMarker(value string, maxBytes int) string {
	if maxBytes <= 0 {
		return "[truncated]"
	}
	if len(value) <= maxBytes {
		return value
	}
	const marker = "...[truncated]"
	if maxBytes <= len(marker) {
		return marker[:maxBytes]
	}
	trimmed := value[:maxBytes-len(marker)]
	for !utf8.ValidString(trimmed) && len(trimmed) > 0 {
		trimmed = trimmed[:len(trimmed)-1]
	}
	return trimmed + marker
}

func shortArgsHash(payload []byte) string {
	sum := sha256.Sum256(payload)
	return hex.EncodeToString(sum[:])[:approvalDescHashPrefixBytes]
}
