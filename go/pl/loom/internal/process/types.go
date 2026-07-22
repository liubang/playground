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

package process

import (
	"errors"
	"fmt"
	"time"
)

var (
	// ErrSandboxRequired reports that a runner without a sandbox refuses to execute.
	ErrSandboxRequired = errors.New("process sandbox is required")
	// ErrSandboxUnavailable reports that the configured sandbox cannot be used safely.
	ErrSandboxUnavailable = errors.New("process sandbox is unavailable")
	// ErrShellNotAllowed reports that shell interpreters cannot be executed.
	ErrShellNotAllowed = errors.New("shell execution is not allowed")
	// ErrExecutableHashChanged reports that the resolved executable changed before start.
	ErrExecutableHashChanged = errors.New("executable hash changed before start")
)

// Isolation describes the active isolation mode used for a process.
type Isolation interface {
	Name() string
	Unsafe() bool
}

type isolationMode struct {
	name   string
	unsafe bool
}

func (m isolationMode) Name() string { return m.name }

func (m isolationMode) Unsafe() bool { return m.unsafe }

func (m isolationMode) String() string { return m.name }

var (
	SeatbeltIsolation     Isolation = isolationMode{name: "seatbelt"}
	ProcessGroupIsolation Isolation = isolationMode{name: "process_group", unsafe: true}
	UnsupportedIsolation  Isolation = isolationMode{name: "unsupported"}
	UnavailableIsolation  Isolation = isolationMode{name: "unavailable"}
)

// CommandSpec describes a single process execution request.
type CommandSpec struct {
	Program     string
	Args        []string
	Cwd         string
	Env         map[string]string
	Timeout     time.Duration
	OutputLimit int64
}

// Result captures the outcome of a process execution.
type Result struct {
	ExitCode       int
	Signal         string
	Duration       time.Duration
	TimedOut       bool
	Cancelled      bool
	Isolation      string
	ExecutablePath string
	ExecutableHash string
	Stdout         []byte
	Stderr         []byte
	Truncated      bool
}

// Sandbox constrains process execution before the command starts.
type Sandbox interface {
	Isolation() Isolation
	Prepare(SandboxSpec) (SandboxLaunch, error)
}

// SandboxSpec is the validated input passed into a sandbox implementation.
type SandboxSpec struct {
	ExecutablePath string
	Args           []string
	WorkingDir     string
	WorkspaceRoot  string
	WritablePaths  []string
	Env            []string
}

// SandboxLaunch describes the concrete program invocation the runner will start.
type SandboxLaunch struct {
	Program string
	Args    []string
	Env     []string
	Cleanup func() error
}

// RunnerOptions configures a runner.
type RunnerOptions struct {
	Sandbox          Sandbox
	EnvAllowlist     []string
	OutputLimit      int64
	TerminationGrace time.Duration
	LookPath         func(string) (string, error)
	Now              func() time.Time
}

// PlatformSandboxOptions configures the default platform sandbox.
type PlatformSandboxOptions struct {
	AllowNetwork  bool
	WritablePaths []string
}

// ExplicitTestSandbox intentionally exposes only unsafe process-group isolation.
type ExplicitTestSandbox struct {
	IsolationMode Isolation
	PrepareFunc   func(SandboxSpec) (SandboxLaunch, error)
}

// Isolation returns the configured isolation mode.
func (s ExplicitTestSandbox) Isolation() Isolation {
	if s.IsolationMode != nil {
		return s.IsolationMode
	}
	return ProcessGroupIsolation
}

// Prepare returns a direct process-group launch and is intended for tests only.
func (s ExplicitTestSandbox) Prepare(spec SandboxSpec) (SandboxLaunch, error) {
	if s.PrepareFunc != nil {
		return s.PrepareFunc(spec)
	}
	if spec.ExecutablePath == "" {
		return SandboxLaunch{}, fmt.Errorf("executable path is required")
	}
	return SandboxLaunch{
		Program: spec.ExecutablePath,
		Args:    append([]string(nil), spec.Args...),
		Env:     append([]string(nil), spec.Env...),
	}, nil
}

// UnsupportedSandbox fails closed on platforms without a reliable sandbox.
type UnsupportedSandbox struct {
	Reason string
}

// Isolation reports the unsupported isolation mode.
func (s UnsupportedSandbox) Isolation() Isolation { return UnsupportedIsolation }

// Prepare always fails closed for unsupported platforms.
func (s UnsupportedSandbox) Prepare(SandboxSpec) (SandboxLaunch, error) {
	reason := s.Reason
	if reason == "" {
		reason = "sandbox is unavailable"
	}
	return SandboxLaunch{}, fmt.Errorf("%w: %s", ErrSandboxUnavailable, reason)
}
