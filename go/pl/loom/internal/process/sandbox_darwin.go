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

//go:build darwin

package process

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const sandboxExecPath = "/usr/bin/sandbox-exec"

// NewPlatformSandbox returns a seatbelt sandbox when sandbox-exec is available.
func NewPlatformSandbox(opts PlatformSandboxOptions) Sandbox {
	info, err := os.Stat(sandboxExecPath)
	if err != nil || info.IsDir() {
		return UnsupportedSandbox{Reason: sandboxExecPath + " is unavailable"}
	}
	return SeatbeltSandbox{
		allowNetwork:  opts.AllowNetwork,
		writablePaths: append([]string(nil), opts.WritablePaths...),
	}
}

// SeatbeltSandbox wraps execution in macOS sandbox-exec.
type SeatbeltSandbox struct {
	allowNetwork  bool
	writablePaths []string
}

// Isolation reports the active seatbelt isolation mode.
func (s SeatbeltSandbox) Isolation() Isolation { return SeatbeltIsolation }

// Prepare creates a temporary seatbelt profile and wraps the child process.
func (s SeatbeltSandbox) Prepare(spec SandboxSpec) (SandboxLaunch, error) {
	profile, err := s.profile(spec)
	if err != nil {
		return SandboxLaunch{}, fmt.Errorf("%w: %v", ErrSandboxUnavailable, err)
	}
	profileFile, err := os.CreateTemp("", "loom-seatbelt-*.sb")
	if err != nil {
		return SandboxLaunch{}, fmt.Errorf("%w: create seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	if _, err := profileFile.WriteString(profile); err != nil {
		_ = profileFile.Close()
		_ = os.Remove(profileFile.Name())
		return SandboxLaunch{}, fmt.Errorf("%w: write seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	if err := profileFile.Close(); err != nil {
		_ = os.Remove(profileFile.Name())
		return SandboxLaunch{}, fmt.Errorf("%w: close seatbelt profile: %v", ErrSandboxUnavailable, err)
	}
	args := []string{"-f", profileFile.Name(), spec.ExecutablePath}
	args = append(args, spec.Args...)
	return SandboxLaunch{
		Program: sandboxExecPath,
		Args:    args,
		Env:     append([]string(nil), spec.Env...),
		Cleanup: func() error { return os.Remove(profileFile.Name()) },
	}, nil
}

func (s SeatbeltSandbox) profile(spec SandboxSpec) (string, error) {
	if strings.TrimSpace(spec.ExecutablePath) == "" {
		return "", fmt.Errorf("executable path is required")
	}
	if !filepath.IsAbs(spec.ExecutablePath) || !filepath.IsAbs(spec.WorkingDir) || !filepath.IsAbs(spec.WorkspaceRoot) {
		return "", fmt.Errorf("seatbelt requires absolute paths")
	}

	readPaths := []string{
		"/bin",
		"/usr",
		"/System",
		"/dev",
		"/private/etc",
		"/private/var/db/timezone",
		filepath.Dir(spec.ExecutablePath),
		filepath.Clean(spec.WorkspaceRoot),
	}
	writePaths := []string{filepath.Clean(spec.WorkspaceRoot)}
	for _, path := range spec.WritablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, filepath.Clean(path))
		readPaths = append(readPaths, filepath.Clean(path))
	}
	for _, path := range s.writablePaths {
		if strings.TrimSpace(path) == "" {
			continue
		}
		writePaths = append(writePaths, filepath.Clean(path))
		readPaths = append(readPaths, filepath.Clean(path))
	}
	readPaths = append(readPaths, filepath.Clean(spec.WorkingDir), os.TempDir())
	readPaths = uniqueCleanPaths(readPaths)
	writePaths = uniqueCleanPaths(writePaths)

	var lines []string
	lines = append(
		lines,
		"(version 1)",
		"(deny default)",
		"(allow process-exec)",
		"(allow process-fork)",
		"(allow signal (target self))",
		"(allow sysctl-read)",
		"(allow file-read-metadata)",
	)
	for _, path := range readPaths {
		lines = append(lines, fmt.Sprintf("(allow file-read* (subpath %s))", seatbeltQuote(path)))
	}
	for _, path := range writePaths {
		lines = append(lines, fmt.Sprintf("(allow file-write* (subpath %s))", seatbeltQuote(path)))
	}
	if s.allowNetwork {
		lines = append(lines, "(allow network*)")
	}
	return strings.Join(lines, "\n") + "\n", nil
}

func uniqueCleanPaths(paths []string) []string {
	seen := map[string]struct{}{}
	result := make([]string, 0, len(paths))
	for _, path := range paths {
		path = filepath.Clean(strings.TrimSpace(path))
		if path == "." || path == "" || !filepath.IsAbs(path) {
			continue
		}
		if _, ok := seen[path]; ok {
			continue
		}
		seen[path] = struct{}{}
		result = append(result, path)
	}
	sort.Strings(result)
	return result
}

func seatbeltQuote(path string) string {
	replacer := strings.NewReplacer(`\\`, `\\\\`, `"`, `\\"`)
	return `"` + replacer.Replace(path) + `"`
}
