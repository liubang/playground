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

package gittools

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
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultGitCommandTimeout        = 5 * time.Second
	defaultGitDiffUnified           = 3
	maxGitDiffUnified               = 20
	maxGitPathBytes                 = 4096
	maxGitStatusStdoutBytes   int64 = 1 << 20
	maxGitDiffStdoutBytes     int64 = 64 << 10
	maxGitRevParseStdoutBytes int64 = 4096
	maxGitStderrBytes         int64 = 16 << 10
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
	gitPath   string
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

type repoRootResolution struct {
	Absolute string
	Display  string
}

type repoPathResolution struct {
	Absolute     string
	Display      string
	RepoRelative string
}

type gitRunResult struct {
	stdout    []byte
	stderr    []byte
	truncated bool
}

type boundedBuffer struct {
	mu        sync.Mutex
	limit     int64
	buf       bytes.Buffer
	truncated bool
}

func newBaseTool(def domain.ToolDefinition, validator *workspacepkg.PathValidator) (baseTool, error) {
	if validator == nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	if err := def.Validate(); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInvalidInput, "invalid tool definition", domain.WithCause(err))
	}

	gitPath, err := exec.LookPath("git")
	if err != nil {
		return baseTool{}, domain.NewError(domain.ErrUnavailable, "git executable not found", domain.WithCause(err))
	}
	gitPath, err = filepath.Abs(gitPath)
	if err != nil {
		return baseTool{}, domain.NewError(domain.ErrInternal, "failed to normalize git executable path", domain.WithCause(err))
	}
	if resolved, err := filepath.EvalSymlinks(gitPath); err == nil {
		gitPath = resolved
	}

	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return baseTool{}, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}

	return baseTool{def: def, validator: validator, gitPath: gitPath, key: key}, nil
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

func sortedUniqueStrings(values map[string]struct{}) []string {
	if len(values) == 0 {
		return []string{}
	}
	out := make([]string, 0, len(values))
	for value := range values {
		out = append(out, value)
	}
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

func sameSortedStrings(left, right []string) bool {
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

func resolveRepoRoot(validator *workspacepkg.PathValidator, input string) (repoRootResolution, error) {
	if strings.TrimSpace(input) == "" {
		return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, "repo_root is required")
	}
	if len(input) > maxGitPathBytes {
		return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("repo_root exceeds %d bytes", maxGitPathBytes))
	}
	if rel, ok := lexicalWorkspaceRelativePath(validator, input); ok && containsSensitiveComponent(rel) {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root contains a sensitive component")
	}

	resolved, err := validator.Validate(input)
	if err != nil {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(resolved)
	if err != nil {
		if os.IsNotExist(err) {
			return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, "repo_root does not exist", domain.WithCause(err))
		}
		return repoRootResolution{}, domain.NewError(domain.ErrUnavailable, "failed to stat repo_root", domain.WithCause(err))
	}
	if !info.IsDir() {
		return repoRootResolution{}, domain.NewError(domain.ErrInvalidInput, "repo_root must refer to a directory")
	}

	rel, err := filepath.Rel(validator.Root(), resolved)
	if err != nil {
		return repoRootResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize repo_root", domain.WithCause(err))
	}
	if containsSensitiveComponent(rel) {
		return repoRootResolution{}, domain.NewError(domain.ErrSecurity, "repo_root contains a sensitive component")
	}
	return repoRootResolution{Absolute: resolved, Display: displayPath(rel)}, nil
}

func resolveRepoPath(
	validator *workspacepkg.PathValidator,
	repoRoot repoRootResolution,
	input string,
) (repoPathResolution, error) {
	if strings.TrimSpace(input) == "" {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, "path is required")
	}
	if filepath.IsAbs(input) {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, "path must be workspace-relative")
	}
	if len(input) > maxGitPathBytes {
		return repoPathResolution{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("path exceeds %d bytes", maxGitPathBytes))
	}
	if containsSensitiveComponent(input) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}

	resolved, err := validator.ResolveLexical(input)
	if err != nil {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes workspace or is invalid", domain.WithCause(err))
	}
	if !isUnderRoot(resolved.Absolute, repoRoot.Absolute) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path escapes repository root")
	}

	repoRelative, err := filepath.Rel(repoRoot.Absolute, resolved.Absolute)
	if err != nil {
		return repoPathResolution{}, domain.NewError(domain.ErrInternal, "failed to normalize repository path", domain.WithCause(err))
	}
	repoRelative = filepath.ToSlash(filepath.Clean(repoRelative))
	if containsSensitiveComponent(repoRelative) {
		return repoPathResolution{}, domain.NewError(domain.ErrSecurity, "path contains a sensitive component")
	}
	return repoPathResolution{
		Absolute:     resolved.Absolute,
		Display:      resolved.Display,
		RepoRelative: repoRelative,
	}, nil
}

func confirmRepoRoot(ctx context.Context, gitPath string, repoRoot repoRootResolution) error {
	result, err := runGit(ctx, gitPath, buildRevParseArgs(repoRoot.Absolute), maxGitRevParseStdoutBytes, maxGitStderrBytes)
	if err != nil {
		return classifyGitError(err, result.stderr, "failed to resolve git repository root")
	}
	if result.truncated {
		return domain.NewError(domain.ErrUnavailable, "git repository root output exceeded limit")
	}

	topLevel := strings.TrimSpace(sanitizeUTF8(result.stdout))
	if topLevel == "" {
		return domain.NewError(domain.ErrUnavailable, "git repository root output was empty")
	}
	if resolved, err := filepath.EvalSymlinks(topLevel); err == nil {
		topLevel = resolved
	}
	if filepath.Clean(topLevel) != filepath.Clean(repoRoot.Absolute) {
		return domain.NewError(domain.ErrInvalidInput, "repo_root must be the repository root")
	}
	return nil
}

func buildRevParseArgs(repoRoot string) []string {
	return []string{
		"--no-pager",
		"-c", "color.ui=false",
		"-c", "core.pager=cat",
		"-C", repoRoot,
		"rev-parse",
		"--show-toplevel",
	}
}

func buildStatusArgs(repoRoot string) []string {
	return []string{
		"--no-pager",
		"-c", "color.ui=false",
		"-c", "core.pager=cat",
		"-C", repoRoot,
		"status",
		"--porcelain=v2",
		"-z",
		"--branch",
	}
}

func buildDiffArgs(repoRoot string, staged bool, unified int, repoRelativePath string) []string {
	args := []string{
		"--no-pager",
		"-c", "color.ui=false",
		"-c", "core.pager=cat",
		"-C", repoRoot,
		"diff",
		"--no-ext-diff",
		"--no-textconv",
		fmt.Sprintf("--unified=%d", unified),
	}
	if staged {
		args = append(args, "--cached")
	}
	if repoRelativePath != "" {
		args = append(args, "--", literalGitPathspec(repoRelativePath))
	}
	return args
}

func literalGitPathspec(path string) string {
	return ":(literal)" + path
}

func runGit(ctx context.Context, gitPath string, args []string, stdoutLimit, stderrLimit int64) (gitRunResult, error) {
	commandCtx, cancel := context.WithTimeout(ctx, defaultGitCommandTimeout)
	defer cancel()

	cmd := exec.CommandContext(commandCtx, gitPath, args...)
	cmd.Env = []string{
		"LANG=C",
		"LC_ALL=C",
		"GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=/dev/null",
		"GIT_TERMINAL_PROMPT=0",
	}
	cmd.Stdin = bytes.NewReader(nil)

	stdout := newBoundedBuffer(stdoutLimit)
	stderr := newBoundedBuffer(stderrLimit)
	cmd.Stdout = stdout
	cmd.Stderr = stderr

	err := cmd.Run()
	result := gitRunResult{
		stdout:    stdout.Bytes(),
		stderr:    stderr.Bytes(),
		truncated: stdout.Truncated() || stderr.Truncated(),
	}
	if err != nil {
		if errors.Is(commandCtx.Err(), context.Canceled) {
			return result, commandCtx.Err()
		}
		if errors.Is(commandCtx.Err(), context.DeadlineExceeded) {
			return result, commandCtx.Err()
		}
		return result, err
	}
	return result, nil
}

func classifyGitError(err error, stderr []byte, fallback string) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
		return err
	}

	stderrText := strings.TrimSpace(sanitizeUTF8(stderr))
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		if strings.Contains(stderrText, "not a git repository") {
			return domain.NewError(domain.ErrInvalidInput, "repo_root is not a git repository root", domain.WithCause(err))
		}
		if stderrText == "" {
			stderrText = fallback
		}
		return domain.NewError(domain.ErrUnavailable, stderrText, domain.WithCause(err))
	}
	if stderrText == "" {
		stderrText = fallback
	}
	return domain.NewError(domain.ErrUnavailable, stderrText, domain.WithCause(err))
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

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func repoPathDisplay(repoRootDisplay, repoRelative string) string {
	clean := filepath.Clean(filepath.FromSlash(repoRelative))
	if clean == "." || clean == string(filepath.Separator) {
		return repoRootDisplay
	}
	if repoRootDisplay == "." {
		return filepath.ToSlash(clean)
	}
	return filepath.ToSlash(filepath.Join(repoRootDisplay, clean))
}

func isUnderRoot(path, root string) bool {
	normalized := filepath.Clean(path)
	rootNorm := filepath.Clean(root)
	if normalized == rootNorm {
		return true
	}
	return strings.HasPrefix(normalized, rootNorm+string(filepath.Separator))
}

func sanitizeUTF8(data []byte) string {
	return string(bytes.ToValidUTF8(data, []byte("?")))
}

func newBoundedBuffer(limit int64) *boundedBuffer {
	return &boundedBuffer{limit: limit}
}

func (b *boundedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()

	n := len(p)
	remaining := b.limit - int64(b.buf.Len())
	if remaining <= 0 {
		b.truncated = true
		return n, nil
	}
	if int64(len(p)) > remaining {
		_, _ = b.buf.Write(p[:remaining])
		b.truncated = true
		return n, nil
	}
	_, _ = b.buf.Write(p)
	return n, nil
}

func (b *boundedBuffer) Bytes() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]byte(nil), b.buf.Bytes()...)
}

func (b *boundedBuffer) Truncated() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.truncated
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
		} else if err != nil {
			message = err.Error()
		}
	}

	finishedAt := time.Now()
	return domain.ToolResult{
		CallID:     callID,
		Status:     status,
		Error:      &domain.ToolError{Code: code, Message: message, Retryable: retryable},
		StartedAt:  startedAt,
		FinishedAt: finishedAt,
	}
}
