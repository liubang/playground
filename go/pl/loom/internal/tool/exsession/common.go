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

package exsession

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxProgramBytes    = 4096
	maxArgsCount       = 256
	maxArgBytes        = 8192
	maxWorkingDirBytes = 4096
	maxEnvVars         = 64
	maxEnvKeyBytes     = 256
	maxEnvValueBytes   = 8192

	maxYieldMs          int64 = 300000
	defaultStartYieldMs int64 = 1000
	defaultWriteYieldMs int64 = 250
	defaultPollYieldMs  int64 = 5000

	defaultMaxOutputBytes int64 = 16384
	maxMaxOutputBytes     int64 = 65536

	maxJustificationBytes = 240
)

// sandbox_permissions values, aligned with run_cmd: sandboxed execution by
// default, or an escalated session outside the sandbox after approval.
const (
	sandboxUseDefault       = "use_default"
	sandboxRequireEscalated = "require_escalated"
)

// signer is the exsession-local HMAC signer, wrapping the toolkit Signer
// with a variadic-parts interface that exec_session and write_stdin share.
type signer struct {
	tk toolkit.Signer
}

func newSigner() (signer, error) {
	tk, err := toolkit.NewSigner()
	if err != nil {
		return signer{}, err
	}
	return signer{tk: tk}, nil
}

func (s *signer) sign(parts ...any) string {
	payload, _ := json.Marshal(parts)
	return s.tk.SignRaw(payload)
}

func (s *signer) verify(expected, actual string) bool {
	return s.tk.VerifyRaw(expected, actual)
}

func decodeStrict[T any](raw json.RawMessage) (T, error) { return toolkit.DecodeStrict[T](raw) }

// commandArgs is the shared command-line shape of exec_session.
type commandArgs struct {
	Program            string            `json:"program"`
	Args               []string          `json:"args,omitempty"`
	WorkingDir         string            `json:"working_dir,omitempty"`
	Env                map[string]string `json:"env,omitempty"`
	YieldTimeMs        int64             `json:"yield_time_ms,omitempty"`
	MaxOutputBytes     int64             `json:"max_output_bytes,omitempty"`
	SandboxPermissions string            `json:"sandbox_permissions,omitempty"`
	NeedsGUIOpen       bool              `json:"needs_gui_open,omitempty"`
	Justification      string            `json:"justification,omitempty"`
}

// validateCommandArgs normalizes and bounds the command-line fields,
// resolving working_dir against the workspace. It is the session
// counterpart of run_cmd's validation, minus the timeout field.
func validateCommandArgs(validator *workspacepkg.PathValidator, args *commandArgs) (absoluteDir string, err error) {
	args.Program = strings.TrimSpace(args.Program)
	if args.Program == "" {
		return "", domain.NewError(domain.ErrInvalidInput, "program is required")
	}
	if len(args.Program) > maxProgramBytes {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("program exceeds %d bytes", maxProgramBytes))
	}
	if strings.ContainsRune(args.Program, 0) {
		return "", domain.NewError(domain.ErrInvalidInput, "program contains null byte")
	}
	if len(args.Args) > maxArgsCount {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args exceeds %d items", maxArgsCount))
	}
	for i, arg := range args.Args {
		if len(arg) > maxArgBytes {
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] exceeds %d bytes", i, maxArgBytes))
		}
		if strings.ContainsRune(arg, 0) {
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("args[%d] contains null byte", i))
		}
	}
	if len(args.Env) > maxEnvVars {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env exceeds %d entries", maxEnvVars))
	}
	for key, value := range args.Env {
		if key == "" || len(key) > maxEnvKeyBytes || strings.ContainsAny(key, "=\x00") {
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q is invalid", key))
		}
		if len(value) > maxEnvValueBytes || strings.ContainsRune(value, 0) {
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q is invalid", key))
		}
	}

	workingDir := args.WorkingDir
	if strings.TrimSpace(workingDir) == "" {
		workingDir = "."
	}
	if len(workingDir) > maxWorkingDirBytes {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir exceeds %d bytes", maxWorkingDirBytes))
	}
	absolute, err := validator.Validate(workingDir)
	if err != nil {
		return "", domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(absolute)
	if err != nil {
		if os.IsNotExist(err) {
			// Echo the offending path so the model can correct course
			// without guessing which working_dir was rejected.
			return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir does not exist: %q", domain.TruncateForErrorEcho(workingDir)), domain.WithCause(err))
		}
		return "", domain.NewError(domain.ErrUnavailable, "failed to stat working_dir", domain.WithCause(err))
	}
	if !info.IsDir() {
		return "", domain.NewError(domain.ErrInvalidInput, "working_dir must be a directory")
	}
	rel, err := filepath.Rel(validator.Root(), absolute)
	if err != nil {
		return "", domain.NewError(domain.ErrInternal, "failed to normalize working_dir", domain.WithCause(err))
	}
	args.WorkingDir = displayPath(rel)

	switch args.SandboxPermissions {
	case "":
		args.SandboxPermissions = sandboxUseDefault
	case sandboxUseDefault:
	case sandboxRequireEscalated:
		if args.NeedsGUIOpen {
			return "", domain.NewError(domain.ErrInvalidInput, "needs_gui_open cannot be combined with sandbox_permissions=require_escalated (escalated runs already have GUI access; use needs_gui_open for the sandboxed path)")
		}
		if strings.TrimSpace(args.Justification) == "" {
			return "", domain.NewError(domain.ErrInvalidInput, "justification is required with sandbox_permissions=require_escalated (ask the user a short yes/no question)")
		}
	default:
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("sandbox_permissions must be %q or %q", sandboxUseDefault, sandboxRequireEscalated))
	}
	if len(args.Justification) > maxJustificationBytes {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("justification exceeds %d bytes", maxJustificationBytes))
	}
	if args.YieldTimeMs < 0 || args.YieldTimeMs > maxYieldMs {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("yield_time_ms must be between 0 and %d", maxYieldMs))
	}
	if args.MaxOutputBytes < 0 || args.MaxOutputBytes > maxMaxOutputBytes {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_output_bytes must be between 0 and %d", maxMaxOutputBytes))
	}
	return absolute, nil
}

// riskForCommand mirrors run_cmd's risk tiers: only an escalated
// session (outside the sandbox) is R3. Shell invocations stay at the
// base risk — the sandbox confines them and the permission layer's
// danger screen (shell AST analysis) catches the dangerous shapes.
func riskForCommand(args commandArgs, base domain.RiskLevel) domain.RiskLevel {
	if args.SandboxPermissions == sandboxRequireEscalated {
		return domain.R3
	}
	return base
}

// sessionOutput is the shared result payload of exec_session/write_stdin.
type sessionOutput struct {
	SessionID          string `json:"session_id"`
	Command            string `json:"command"`
	Status             string `json:"status"`
	ExitCode           int    `json:"exit_code"`
	Signal             string `json:"signal,omitempty"`
	Output             string `json:"output"`
	OutputDroppedBytes int64  `json:"output_dropped_bytes,omitempty"`
	StdoutBytes        int64  `json:"stdout_bytes"`
	StderrBytes        int64  `json:"stderr_bytes"`
	DurationMs         int64  `json:"duration_ms"`
	Isolation          string `json:"isolation"`
	StdoutArtifactPath string `json:"stdout_artifact_path,omitempty"`
	StderrArtifactPath string `json:"stderr_artifact_path,omitempty"`
	Note               string `json:"note,omitempty"`
}

// awaitYield blocks until the yield budget expires, the session exits, or
// the caller cancels — whichever comes first.
func awaitYield(ctx context.Context, entry *sessionEntry, yieldMs int64) {
	if yieldMs <= 0 {
		return
	}
	timer := time.NewTimer(time.Duration(yieldMs) * time.Millisecond)
	defer timer.Stop()
	select {
	case <-entry.session.Done():
	case <-timer.C:
	case <-ctx.Done():
	}
}

// drainSession reads the incremental output window, commits artifacts once
// the session has exited, and renders the shared result payload. A commit
// failure is surfaced in the note instead of failing the drain: the process
// output is still readable, and the staging area is kept for a later retry
// (REVIEW H13).
func drainSession(ctx context.Context, m *Manager, entry *sessionEntry, maxBytes int64) sessionOutput {
	if maxBytes <= 0 {
		maxBytes = defaultMaxOutputBytes
	}
	read := entry.session.Read(int(maxBytes))
	commitErr := entry.commitArtifacts(ctx)

	status := "running"
	switch {
	case !read.Running && read.Killed:
		status = "killed"
	case !read.Running:
		status = "exited"
	}

	out := sessionOutput{
		SessionID:          entry.id,
		Command:            entry.argv,
		Status:             status,
		ExitCode:           read.ExitCode,
		Signal:             read.Signal,
		Output:             sanitizeUTF8(read.Data),
		OutputDroppedBytes: read.DroppedBytes,
		StdoutBytes:        read.StdoutBytes,
		StderrBytes:        read.StderrBytes,
		DurationMs:         read.Duration.Milliseconds(),
		Isolation:          entry.session.Isolation,
	}
	if read.Running {
		out.ExitCode = -1
	}
	if read.DroppedBytes > 0 {
		out.Note = fmt.Sprintf("output shows only the most recent bytes; %d earlier bytes were dropped from this view (full output is tracked in the session artifacts)", read.DroppedBytes)
	}
	if commitErr != nil {
		msg := fmt.Sprintf("session output artifact commit failed (%v); the staged output is kept and will be retried on the next read", commitErr)
		if out.Note != "" {
			out.Note += " " + msg
		} else {
			out.Note = msg
		}
	}
	if resolver, ok := m.artifacts.(artifactPathResolver); ok {
		if entry.stdoutRef != nil {
			if path, found := resolver.PathForRef(*entry.stdoutRef); found {
				out.StdoutArtifactPath = path
			}
		}
		if entry.stderrRef != nil {
			if path, found := resolver.PathForRef(*entry.stderrRef); found {
				out.StderrArtifactPath = path
			}
		}
	}
	return out
}

// artifactPathResolver matches the concrete artifact store, giving the
// model a directly readable path instead of an opaque blob ID.
type artifactPathResolver interface {
	PathForRef(ref domain.ArtifactRef) (string, bool)
}

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func sanitizeUTF8(data string) string {
	if utf8.ValidString(data) {
		return data
	}
	return strings.ToValidUTF8(data, "?")
}

func displayArgv(program string, args []string) string {
	parts := append([]string{program}, args...)
	quoted := make([]string, 0, len(parts))
	for _, item := range parts {
		if strings.ContainsAny(item, " \t'\"\\") {
			item = "'" + strings.ReplaceAll(item, "'", `'"'"'`) + "'"
		}
		quoted = append(quoted, item)
	}
	return strings.Join(quoted, " ")
}

func successResult(callID domain.ToolCallID, startedAt time.Time, payload any) domain.ToolResult {
	return toolkit.SuccessResult(callID, startedAt, payload)
}

func errorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	return toolkit.ErrorResult(callID, startedAt, err)
}
