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
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const (
	defaultOutputLimit      int64         = 128 * 1024
	defaultTerminationGrace time.Duration = 100 * time.Millisecond
	defaultDrainWaitLimit   time.Duration = 100 * time.Millisecond
	defaultPATH                           = "/usr/bin:/bin:/usr/sbin:/sbin"
)

var defaultEnvAllowlist = []string{"PATH", "LANG", "LC_ALL", "TMPDIR", "HOME"}

var disallowedShells = map[string]struct{}{
	"sh":   {},
	"bash": {},
	"dash": {},
	"ksh":  {},
	"zsh":  {},
	"fish": {},
}

// Runner executes validated commands under sandbox control.
type Runner struct {
	validator        *workspace.PathValidator
	sandbox          Sandbox
	envAllowlist     map[string]struct{}
	outputLimit      int64
	terminationGrace time.Duration
	lookPath         func(string) (string, error)
	now              func() time.Time
}

// NewRunner creates a process runner bound to a workspace validator.
func NewRunner(validator *workspace.PathValidator, opts RunnerOptions) (*Runner, error) {
	if validator == nil {
		return nil, fmt.Errorf("path validator is required")
	}
	outputLimit := opts.OutputLimit
	if outputLimit <= 0 {
		outputLimit = defaultOutputLimit
	}
	terminationGrace := opts.TerminationGrace
	if terminationGrace <= 0 {
		terminationGrace = defaultTerminationGrace
	}
	lookPath := opts.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	now := opts.Now
	if now == nil {
		now = time.Now
	}
	return &Runner{
		validator:        validator,
		sandbox:          opts.Sandbox,
		envAllowlist:     makeEnvAllowlist(opts.EnvAllowlist),
		outputLimit:      outputLimit,
		terminationGrace: terminationGrace,
		lookPath:         lookPath,
		now:              now,
	}, nil
}

// Run resolves, validates, and executes a command under the configured sandbox.
func (r *Runner) Run(ctx context.Context, spec CommandSpec) (Result, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if r.sandbox == nil {
		return Result{Isolation: UnavailableIsolation.Name()}, ErrSandboxRequired
	}
	validated, err := r.validateSpec(spec)
	if err != nil {
		return Result{}, err
	}
	launch, cleanup, isolation, err := r.prepareLaunch(validated)
	result := Result{
		Isolation:      isolation.Name(),
		ExecutablePath: validated.executablePath,
		ExecutableHash: validated.executableHash,
	}
	if err != nil {
		return result, err
	}
	if cleanup != nil {
		defer func() { _ = cleanup() }()
	}
	if err := verifyExecutable(validated.executablePath, validated.executableHash); err != nil {
		return result, err
	}

	execCtx, cancel := withOptionalTimeout(ctx, spec.Timeout)
	defer cancel()
	if execCtx.Err() != nil {
		result.TimedOut = errors.Is(execCtx.Err(), context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		return result, nil
	}

	devNull, err := os.Open(os.DevNull)
	if err != nil {
		return result, fmt.Errorf("open %s: %w", os.DevNull, err)
	}
	defer func() { _ = devNull.Close() }()

	cmd := exec.Command(launch.Program, launch.Args...)
	cmd.Dir = validated.cwd
	cmd.Env = append([]string(nil), launch.Env...)
	cmd.Stdin = devNull
	configureUnixProcessGroup(cmd)

	stdoutPipe, err := cmd.StdoutPipe()
	if err != nil {
		return result, fmt.Errorf("stdout pipe: %w", err)
	}
	stderrPipe, err := cmd.StderrPipe()
	if err != nil {
		return result, fmt.Errorf("stderr pipe: %w", err)
	}

	collector := newOutputCollector(validated.outputLimit)
	var copyWG sync.WaitGroup
	copyWG.Add(2)
	go func() {
		defer copyWG.Done()
		collector.collect(stdoutPipe, true)
	}()
	go func() {
		defer copyWG.Done()
		collector.collect(stderrPipe, false)
	}()

	startedAt := r.now()
	if err := cmd.Start(); err != nil {
		closeReadPipe(stdoutPipe)
		closeReadPipe(stderrPipe)
		if !waitForCopyWG(&copyWG, defaultDrainWaitLimit) {
			return result, fmt.Errorf("start command: output readers did not stop")
		}
		return result, fmt.Errorf("start command: %w", err)
	}

	waitErrCh := make(chan error, 1)
	go func() {
		waitErrCh <- cmd.Wait()
	}()

	waitErr, timedOut, cancelled := r.waitAndReap(execCtx, cmd, waitErrCh)
	closeReadPipe(stdoutPipe)
	closeReadPipe(stderrPipe)
	if !waitForCopyWG(&copyWG, defaultDrainWaitLimit) {
		return result, fmt.Errorf("wait command: output readers did not stop")
	}

	result.Duration = r.now().Sub(startedAt)
	result.Stdout = collector.stdout()
	result.Stderr = collector.stderr()
	result.Truncated = collector.truncated()
	result.TimedOut = timedOut
	result.Cancelled = cancelled
	fillExitStatus(&result, cmd.ProcessState)
	if err := interpretWaitError(waitErr); err != nil {
		return result, fmt.Errorf("wait command: %w", err)
	}
	return result, nil
}

type validatedSpec struct {
	executablePath string
	executableHash string
	args           []string
	cwd            string
	env            []string
	outputLimit    int64
}

func (r *Runner) validateSpec(spec CommandSpec) (validatedSpec, error) {
	program := strings.TrimSpace(spec.Program)
	if program == "" {
		return validatedSpec{}, fmt.Errorf("program is required")
	}
	if strings.ContainsRune(program, 0) {
		return validatedSpec{}, fmt.Errorf("program contains null byte")
	}
	if isShellProgram(program) {
		return validatedSpec{}, ErrShellNotAllowed
	}
	for _, arg := range spec.Args {
		if strings.ContainsRune(arg, 0) {
			return validatedSpec{}, fmt.Errorf("argument contains null byte")
		}
	}
	if strings.ContainsRune(spec.Cwd, 0) {
		return validatedSpec{}, fmt.Errorf("cwd contains null byte")
	}
	if spec.Timeout < 0 {
		return validatedSpec{}, fmt.Errorf("timeout must be non-negative")
	}

	executablePath, executableHash, err := r.resolveExecutable(program)
	if err != nil {
		return validatedSpec{}, err
	}
	cwd, err := r.validator.Validate(spec.Cwd)
	if err != nil {
		return validatedSpec{}, fmt.Errorf("validate cwd: %w", err)
	}
	outputLimit := spec.OutputLimit
	if outputLimit <= 0 {
		outputLimit = r.outputLimit
	}
	return validatedSpec{
		executablePath: executablePath,
		executableHash: executableHash,
		args:           append([]string(nil), spec.Args...),
		cwd:            cwd,
		env:            buildMinimalEnv(spec.Env, r.envAllowlist),
		outputLimit:    outputLimit,
	}, nil
}

func (r *Runner) prepareLaunch(spec validatedSpec) (SandboxLaunch, func() error, Isolation, error) {
	isolation := UnavailableIsolation
	if r.sandbox != nil && r.sandbox.Isolation() != nil {
		isolation = r.sandbox.Isolation()
	}
	launch, err := r.sandbox.Prepare(SandboxSpec{
		ExecutablePath: spec.executablePath,
		Args:           append([]string(nil), spec.args...),
		WorkingDir:     spec.cwd,
		WorkspaceRoot:  r.validator.Root(),
		WritablePaths:  []string{r.validator.Root()},
		Env:            append([]string(nil), spec.env...),
	})
	if err != nil {
		return SandboxLaunch{}, nil, isolation, err
	}
	if strings.TrimSpace(launch.Program) == "" {
		return SandboxLaunch{}, nil, isolation, fmt.Errorf("sandbox returned empty program")
	}
	if len(launch.Env) == 0 {
		launch.Env = append([]string(nil), spec.env...)
	}
	return launch, launch.Cleanup, isolation, nil
}

func (r *Runner) waitAndReap(ctx context.Context, cmd *exec.Cmd, waitErrCh <-chan error) (error, bool, bool) {
	select {
	case err := <-waitErrCh:
		return err, false, false
	case <-ctx.Done():
		timedOut := errors.Is(ctx.Err(), context.DeadlineExceeded)
		cancelled := !timedOut
		signalUnixProcessGroup(cmd.Process.Pid, syscall.SIGTERM)
		timer := time.NewTimer(r.terminationGrace)
		defer timer.Stop()
		select {
		case err := <-waitErrCh:
			return err, timedOut, cancelled
		case <-timer.C:
		}
		signalUnixProcessGroup(cmd.Process.Pid, syscall.SIGKILL)
		return <-waitErrCh, timedOut, cancelled
	}
}

func (r *Runner) resolveExecutable(program string) (string, string, error) {
	path, err := r.lookPath(program)
	if err != nil {
		return "", "", fmt.Errorf("look path %q: %w", program, err)
	}
	absolutePath, err := filepath.Abs(path)
	if err != nil {
		return "", "", fmt.Errorf("absolute executable path: %w", err)
	}
	resolvedPath, err := filepath.EvalSymlinks(absolutePath)
	if err != nil {
		return "", "", fmt.Errorf("resolve executable symlinks: %w", err)
	}
	hash, err := hashExecutable(resolvedPath)
	if err != nil {
		return "", "", err
	}
	return resolvedPath, hash, nil
}

func withOptionalTimeout(ctx context.Context, timeout time.Duration) (context.Context, context.CancelFunc) {
	if timeout <= 0 {
		return context.WithCancel(ctx)
	}
	return context.WithTimeout(ctx, timeout)
}

func hashExecutable(path string) (string, error) {
	info, err := os.Stat(path)
	if err != nil {
		return "", fmt.Errorf("stat executable: %w", err)
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("executable %q is not a regular file", path)
	}
	if info.Mode().Perm()&0o111 == 0 {
		return "", fmt.Errorf("executable %q is not executable", path)
	}
	file, err := os.Open(path)
	if err != nil {
		return "", fmt.Errorf("open executable: %w", err)
	}
	defer func() { _ = file.Close() }()
	checksum := sha256.New()
	if _, err := io.Copy(checksum, file); err != nil {
		return "", fmt.Errorf("hash executable: %w", err)
	}
	return hex.EncodeToString(checksum.Sum(nil)), nil
}

func verifyExecutable(path, expectedHash string) error {
	actualHash, err := hashExecutable(path)
	if err != nil {
		return err
	}
	if actualHash != expectedHash {
		return fmt.Errorf("%w: %s", ErrExecutableHashChanged, path)
	}
	return nil
}

func buildMinimalEnv(overrides map[string]string, allowlist map[string]struct{}) []string {
	values := map[string]string{}
	for key := range allowlist {
		if isDeniedEnvKey(key) {
			continue
		}
		if value, ok := os.LookupEnv(key); ok {
			values[key] = value
		}
	}
	if value, ok := values["PATH"]; !ok || strings.TrimSpace(value) == "" {
		values["PATH"] = defaultPATH
	}
	if _, ok := allowlist["LANG"]; ok {
		if value := strings.TrimSpace(values["LANG"]); value == "" {
			values["LANG"] = "C.UTF-8"
		}
	}
	if _, ok := allowlist["TMPDIR"]; ok {
		if value := strings.TrimSpace(values["TMPDIR"]); value == "" {
			values["TMPDIR"] = os.TempDir()
		}
	}
	for key, value := range overrides {
		if !allowedEnvKey(key, allowlist) {
			continue
		}
		values[key] = value
	}
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	result := make([]string, 0, len(keys))
	for _, key := range keys {
		result = append(result, key+"="+values[key])
	}
	return result
}

func makeEnvAllowlist(keys []string) map[string]struct{} {
	if len(keys) == 0 {
		keys = defaultEnvAllowlist
	}
	allowlist := make(map[string]struct{}, len(keys)+1)
	allowlist["PATH"] = struct{}{}
	for _, key := range keys {
		key = strings.TrimSpace(key)
		if key == "" || strings.ContainsAny(key, "=\x00") {
			continue
		}
		allowlist[key] = struct{}{}
	}
	return allowlist
}

func allowedEnvKey(key string, allowlist map[string]struct{}) bool {
	key = strings.TrimSpace(key)
	if key == "" || strings.ContainsAny(key, "=\x00") || isDeniedEnvKey(key) {
		return false
	}
	_, ok := allowlist[key]
	return ok
}

func isDeniedEnvKey(key string) bool {
	upper := strings.ToUpper(strings.TrimSpace(key))
	if upper == "" {
		return true
	}
	for _, needle := range []string{"KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "LOOM", "OPENAI", "ANTHROPIC", "AWS", "GCP"} {
		if strings.Contains(upper, needle) {
			return true
		}
	}
	return false
}

func isShellProgram(program string) bool {
	base := strings.ToLower(filepath.Base(strings.TrimSpace(program)))
	_, forbidden := disallowedShells[base]
	return forbidden
}

func interpretWaitError(err error) error {
	if err == nil {
		return nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return nil
	}
	return err
}

func fillExitStatus(result *Result, state *os.ProcessState) {
	if result == nil || state == nil {
		return
	}
	result.ExitCode = state.ExitCode()
	status, ok := state.Sys().(syscall.WaitStatus)
	if !ok {
		return
	}
	if status.Signaled() {
		result.Signal = status.Signal().String()
	}
}

type outputCollector struct {
	limit        int64
	mu           sync.Mutex
	stdoutBuf    []byte
	stderrBuf    []byte
	total        int64
	wasTruncated bool
}

func newOutputCollector(limit int64) *outputCollector {
	return &outputCollector{limit: limit}
}

func (c *outputCollector) collect(r io.Reader, stdout bool) {
	buf := make([]byte, 4096)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			c.append(buf[:n], stdout)
		}
		if err != nil {
			if !errors.Is(err, io.EOF) && !isBenignPipeReadError(err) {
				c.markTruncated()
			}
			return
		}
	}
}

func (c *outputCollector) append(chunk []byte, stdout bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	remaining := c.limit - c.total
	if remaining <= 0 {
		c.wasTruncated = true
		return
	}
	if int64(len(chunk)) > remaining {
		chunk = chunk[:remaining]
		c.wasTruncated = true
	}
	if stdout {
		c.stdoutBuf = append(c.stdoutBuf, chunk...)
	} else {
		c.stderrBuf = append(c.stderrBuf, chunk...)
	}
	c.total += int64(len(chunk))
}

func (c *outputCollector) markTruncated() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.wasTruncated = true
}

func (c *outputCollector) stdout() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]byte(nil), c.stdoutBuf...)
}

func (c *outputCollector) stderr() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]byte(nil), c.stderrBuf...)
}

func (c *outputCollector) truncated() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.wasTruncated
}

func closeReadPipe(pipe io.ReadCloser) {
	if pipe == nil {
		return
	}
	_ = pipe.Close()
}

func waitForCopyWG(wg *sync.WaitGroup, limit time.Duration) bool {
	if wg == nil {
		return true
	}
	if limit <= 0 {
		limit = defaultDrainWaitLimit
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		wg.Wait()
	}()
	select {
	case <-done:
		return true
	case <-time.After(limit):
		return false
	}
}

func isBenignPipeReadError(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, os.ErrClosed) || errors.Is(err, syscall.EBADF) {
		return true
	}
	var pathErr *os.PathError
	if errors.As(err, &pathErr) {
		return errors.Is(pathErr.Err, os.ErrClosed) || errors.Is(pathErr.Err, syscall.EBADF)
	}
	return false
}
