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
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/toolkit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	maxYieldMs          int64 = 300000
	defaultStartYieldMs int64 = 1000
	defaultWriteYieldMs int64 = 250
	defaultPollYieldMs  int64 = 5000

	defaultMaxOutputBytes int64 = 16384
	maxMaxOutputBytes     int64 = 65536

	maxJustificationBytes = 240
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

// commandArgs is the shared command-line shape of exec_session.
type commandArgs struct {
	Command            string            `json:"command"`
	WorkingDir         string            `json:"working_dir,omitempty"`
	Env                map[string]string `json:"env,omitempty"`
	YieldTimeMs        int64             `json:"yield_time_ms,omitempty"`
	MaxOutputBytes     int64             `json:"max_output_bytes,omitempty"`
	SandboxPermissions string            `json:"sandbox_permissions,omitempty"`
	NeedsGUIOpen       bool              `json:"needs_gui_open,omitempty"`
	Justification      string            `json:"justification,omitempty"`
}

// validateCommandArgs normalizes and bounds the command-line fields; the
// command/env/working_dir checks come from the toolkit helpers shared with
// run_cmd, so both shell tools enforce one protocol.
func validateCommandArgs(validator *workspacepkg.PathValidator, args *commandArgs) (absoluteDir string, err error) {
	command, err := toolkit.ValidateCommandText("exec_session", args.Command)
	if err != nil {
		return "", err
	}
	args.Command = command
	if err := toolkit.ValidateEnv(args.Env); err != nil {
		return "", err
	}
	absoluteDir, displayDir, err := toolkit.ResolveWorkingDir(validator, args.WorkingDir)
	if err != nil {
		return "", err
	}
	args.WorkingDir = displayDir

	switch args.SandboxPermissions {
	case "":
		args.SandboxPermissions = toolkit.SandboxUseDefault
	case toolkit.SandboxUseDefault:
	case toolkit.SandboxRequireEscalated:
		if args.NeedsGUIOpen {
			return "", domain.NewError(domain.ErrInvalidInput, "needs_gui_open cannot be combined with sandbox_permissions=require_escalated (escalated runs already have GUI access; use needs_gui_open for the sandboxed path)")
		}
		if strings.TrimSpace(args.Justification) == "" {
			return "", domain.NewError(domain.ErrInvalidInput, "justification is required with sandbox_permissions=require_escalated (ask the user a short yes/no question)")
		}
	default:
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("sandbox_permissions must be %q or %q", toolkit.SandboxUseDefault, toolkit.SandboxRequireEscalated))
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
	return absoluteDir, nil
}

// riskForCommand mirrors run_cmd's risk tiers: only an escalated
// session (outside the sandbox) is R3. Shell invocations stay at the
// base risk — the sandbox confines them and the permission layer's
// danger screen (shell AST analysis) catches the dangerous shapes.
func riskForCommand(args commandArgs, base domain.RiskLevel) domain.RiskLevel {
	if args.SandboxPermissions == toolkit.SandboxRequireEscalated {
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
		Output:             toolkit.SanitizeUTF8([]byte(read.Data)),
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
