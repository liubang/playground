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
// Created: 2026/09/05

package toolkit

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// Command protocol limits and sandbox permissions shared by run_cmd and
// exec_session, the only two tools that accept an arbitrary shell command.
// The limits align with the JSON schema bounds both tools declare; the
// sandbox_permissions values must stay byte-identical across both tools
// because models learn them as one protocol.
const (
	MaxCommandBytes    = 32 * 1024
	MaxWorkingDirBytes = 4096
	MaxEnvVars         = 64
	MaxEnvKeyBytes     = 256
	MaxEnvValueBytes   = 8192

	SandboxUseDefault       = "use_default"
	SandboxRequireEscalated = "require_escalated"
)

// ValidateCommandText trims and bounds a shell command; toolName labels the
// missing-command error's fix hint.
func ValidateCommandText(toolName, command string) (string, error) {
	command = strings.TrimSpace(command)
	if command == "" {
		return "", MissingCommandError(toolName)
	}
	if len(command) > MaxCommandBytes {
		return "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("command exceeds %d bytes", MaxCommandBytes))
	}
	if strings.ContainsRune(command, 0) {
		return "", domain.NewError(domain.ErrInvalidInput, "command contains null byte")
	}
	return command, nil
}

// ValidateEnv checks the env override bounds shared by run_cmd and
// exec_session: entry count, key/value sizes and control characters.
func ValidateEnv(env map[string]string) error {
	if len(env) > MaxEnvVars {
		return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env exceeds %d entries", MaxEnvVars))
	}
	for key, value := range env {
		if key == "" {
			return domain.NewError(domain.ErrInvalidInput, "env contains an empty key")
		}
		if len(key) > MaxEnvKeyBytes {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q exceeds %d bytes", key, MaxEnvKeyBytes))
		}
		if strings.ContainsAny(key, "=\x00") {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env key %q is invalid", key))
		}
		if len(value) > MaxEnvValueBytes {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q exceeds %d bytes", key, MaxEnvValueBytes))
		}
		if strings.ContainsRune(value, 0) {
			return domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("env value for %q contains null byte", key))
		}
	}
	return nil
}

// ResolveWorkingDir validates a working directory against the workspace
// validator — defaults to the workspace root, must exist and be a
// directory — returning the absolute path and the workspace-relative
// display path. Shared by run_cmd and exec_session.
func ResolveWorkingDir(validator *workspacepkg.PathValidator, workingDir string) (absolute, display string, err error) {
	if validator == nil {
		return "", "", domain.NewError(domain.ErrInvalidInput, "path validator is required")
	}
	workingDir = strings.TrimSpace(workingDir)
	if workingDir == "" {
		workingDir = "."
	}
	if len(workingDir) > MaxWorkingDirBytes {
		return "", "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir exceeds %d bytes", MaxWorkingDirBytes))
	}
	absolute, err = validator.Validate(workingDir)
	if err != nil {
		return "", "", domain.NewError(domain.ErrSecurity, "working_dir escapes workspace or is invalid", domain.WithCause(err))
	}
	info, err := os.Stat(absolute)
	if err != nil {
		if os.IsNotExist(err) {
			// Echo the offending path so the model can correct course
			// without guessing which working_dir was rejected.
			return "", "", domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("working_dir does not exist: %q", domain.TruncateForErrorEcho(workingDir)), domain.WithCause(err))
		}
		return "", "", domain.NewError(domain.ErrUnavailable, "failed to stat working_dir", domain.WithCause(err))
	}
	if !info.IsDir() {
		return "", "", domain.NewError(domain.ErrInvalidInput, "working_dir must be a directory")
	}
	rel, err := filepath.Rel(validator.Root(), absolute)
	if err != nil {
		return "", "", domain.NewError(domain.ErrInternal, "failed to normalize working_dir", domain.WithCause(err))
	}
	return absolute, workspacepkg.DisplayPath(rel), nil
}
