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

package process

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"
)

// DefaultSessionBufferCap bounds the unread merged-output window a session
// keeps in memory. Bytes beyond the cap are dropped oldest-first and the
// drop is reported honestly via ReadOutput.DroppedBytes — they still reach
// the streaming writers (artifact staging), so nothing is lost for audit.
const DefaultSessionBufferCap = 1 << 20

// Session is a long-running asynchronous process started under the same
// validation, environment, and sandbox pipeline as Runner.Run. Unlike Run
// it does not block: stdout and stderr are merged in arrival order into a
// bounded unread window that callers drain incrementally via Read, stdin
// stays writable, and Kill reclaims the whole process group.
//
// The merged stream intentionally mirrors a terminal: interactive programs
// (dev servers, REPLs, watch-mode test runners) interleave their streams,
// and preserving arrival order is what makes the output intelligible.
type Session struct {
	mu           sync.Mutex
	buf          []byte
	bufCap       int
	droppedBytes int64
	stdoutBytes  int64
	stderrBytes  int64

	stdin    io.WriteCloser
	pid      int
	done     chan struct{}
	killCh   chan struct{}
	killOnce sync.Once

	startedAt time.Time
	endedAt   time.Time
	exitCode  int
	signal    string
	killed    bool

	// Immutable metadata captured at start, safe to read without the lock.
	Isolation      string
	ExecutablePath string
	ExecutableHash string
	DroppedEnvKeys []string

	grace   time.Duration
	cleanup func() error
}

// StartSessionWithGrant starts a session with the sandbox mapped from a
// policy grant, mirroring RunWithGrant for the asynchronous path.
func (r *Runner) StartSessionWithGrant(spec CommandSpec, grant Grant) (*Session, error) {
	return r.StartSession(spec, r.sandboxForGrant(grant))
}

// StartSession launches spec under sandbox (nil falls back to the runner's
// default) and returns immediately with the process running in the
// background. CommandSpec.Timeout is ignored: a session's lifetime is
// governed by the caller via Kill, not by a wall clock. StdoutWriter and
// StderrWriter, when set, receive the full untruncated streams exactly as
// in Run (e.g. artifact staging).
func (r *Runner) StartSession(spec CommandSpec, sandbox Sandbox) (*Session, error) {
	if sandbox == nil {
		sandbox = r.sandbox
	}
	if sandbox == nil {
		return nil, ErrSandboxRequired
	}
	validated, err := r.validateSpec(spec)
	if err != nil {
		return nil, err
	}
	env, droppedEnv := r.envForSandbox(spec.Env, sandbox)
	validated.env = r.applySessionEnv(env)
	launch, cleanup, isolation, err := r.prepareLaunch(validated, sandbox)
	if err != nil {
		return nil, err
	}
	if err := verifyExecutable(validated.executablePath, validated.executableHash); err != nil {
		if cleanup != nil {
			_ = cleanup()
		}
		return nil, err
	}

	cmd := exec.Command(launch.Program, launch.Args...)
	cmd.Dir = validated.cwd
	cmd.Env = append([]string(nil), launch.Env...)
	configureUnixProcessGroup(cmd)

	stdinPipe, err := cmd.StdinPipe()
	if err != nil {
		if cleanup != nil {
			_ = cleanup()
		}
		return nil, fmt.Errorf("stdin pipe: %w", err)
	}

	// Self-managed pipes, same rationale as Run (REVIEW H7): exec.Cmd.Wait
	// must not be able to race the drain of the kernel buffer.
	stdoutR, stdoutW, err := os.Pipe()
	if err != nil {
		if cleanup != nil {
			_ = cleanup()
		}
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	stderrR, stderrW, err := os.Pipe()
	if err != nil {
		_ = stdoutR.Close()
		_ = stdoutW.Close()
		if cleanup != nil {
			_ = cleanup()
		}
		return nil, fmt.Errorf("stderr pipe: %w", err)
	}
	cmd.Stdout = stdoutW
	cmd.Stderr = stderrW

	s := &Session{
		bufCap:         DefaultSessionBufferCap,
		stdin:          stdinPipe,
		done:           make(chan struct{}),
		killCh:         make(chan struct{}),
		exitCode:       -1,
		Isolation:      isolation.Name(),
		ExecutablePath: validated.executablePath,
		ExecutableHash: validated.executableHash,
		DroppedEnvKeys: droppedEnv,
		grace:          r.terminationGrace,
		cleanup:        cleanup,
	}

	var copyWG sync.WaitGroup
	copyWG.Add(2)
	go func() {
		defer copyWG.Done()
		s.pump(stdoutR, spec.StdoutWriter, true)
	}()
	go func() {
		defer copyWG.Done()
		s.pump(stderrR, spec.StderrWriter, false)
	}()

	if err := cmd.Start(); err != nil {
		_ = stdoutW.Close()
		_ = stderrW.Close()
		closeReadPipe(stdoutR)
		closeReadPipe(stderrR)
		copyWG.Wait()
		if cleanup != nil {
			_ = cleanup()
		}
		return nil, fmt.Errorf("start command: %w", err)
	}
	// The parent drops its own write ends so the readers see EOF once the
	// process group is dead.
	_ = stdoutW.Close()
	_ = stderrW.Close()
	s.pid = cmd.Process.Pid
	s.startedAt = r.now()

	waitErrCh := make(chan error, 1)
	go func() { waitErrCh <- cmd.Wait() }()

	// Supervisor: resolve kill requests, wait for exit, then let the pumps
	// drain to EOF before closing done. Readers may keep calling Read after
	// done closes — the buffered tail stays available.
	go func() {
		var waitErr error
		select {
		case waitErr = <-waitErrCh:
		case <-s.killCh:
			s.signalGroup(syscall.SIGTERM)
			timer := time.NewTimer(s.grace)
			select {
			case waitErr = <-waitErrCh:
			case <-timer.C:
				s.signalGroup(syscall.SIGKILL)
				waitErr = <-waitErrCh
			}
			timer.Stop()
			s.mu.Lock()
			s.killed = true
			s.mu.Unlock()
		}
		// A detached descendant can hold a write end open and stall the
		// drain; force-close the read ends in that case, like Run does.
		if !waitForCopyWG(&copyWG, defaultDrainWaitLimit) {
			closeReadPipe(stdoutR)
			closeReadPipe(stderrR)
			copyWG.Wait()
		}
		s.mu.Lock()
		s.endedAt = r.now()
		s.exitCode, s.signal = exitStatusOf(cmd.ProcessState, waitErr)
		s.mu.Unlock()
		if s.cleanup != nil {
			_ = s.cleanup()
		}
		close(s.done)
	}()

	return s, nil
}

// pump copies one stream into the session window and the optional external
// writer, counting observed bytes per stream.
func (s *Session) pump(r io.Reader, w io.Writer, isStdout bool) {
	chunk := make([]byte, 32<<10)
	for {
		n, err := r.Read(chunk)
		if n > 0 {
			if w != nil {
				_, _ = w.Write(chunk[:n])
			}
			s.mu.Lock()
			s.appendLocked(chunk[:n])
			if isStdout {
				s.stdoutBytes += int64(n)
			} else {
				s.stderrBytes += int64(n)
			}
			s.mu.Unlock()
		}
		if err != nil {
			return
		}
	}
}

// appendLocked adds data to the unread window, dropping the oldest bytes
// when the cap is exceeded. s.mu must be held.
func (s *Session) appendLocked(data []byte) {
	if len(data) >= s.bufCap {
		s.droppedBytes += int64(len(s.buf)) + int64(len(data)-s.bufCap)
		s.buf = append(s.buf[:0], data[len(data)-s.bufCap:]...)
		return
	}
	s.buf = append(s.buf, data...)
	if overflow := len(s.buf) - s.bufCap; overflow > 0 {
		s.droppedBytes += int64(overflow)
		s.buf = append([]byte(nil), s.buf[overflow:]...)
	}
}

// ReadOutput is one incremental drain of the session window.
type ReadOutput struct {
	// Data is the merged stdout/stderr produced since the previous Read.
	Data string
	// Running reports whether the process (group) is still alive.
	Running bool
	// Killed reports whether the session ended via Kill.
	Killed bool
	// ExitCode is valid once Running is false; -1 while running.
	ExitCode int
	// Signal is the terminating signal name, when any.
	Signal string
	// DroppedBytes counts window bytes discarded because the unread window
	// exceeded its cap (slow consumer), or because Data hit maxBytes.
	DroppedBytes int64
	// StdoutBytes/StderrBytes are cumulative observed stream sizes.
	StdoutBytes int64
	StderrBytes int64
	// Duration is the wall time since start (or until exit).
	Duration time.Duration
}

// Read drains the unread window, returning at most maxBytes (the tail is
// kept when the window exceeds maxBytes; the skipped head is accounted in
// DroppedBytes). Read is safe to call after the session ends.
func (s *Session) Read(maxBytes int) ReadOutput {
	s.mu.Lock()
	defer s.mu.Unlock()

	data := s.buf
	s.buf = nil
	dropped := s.droppedBytes
	s.droppedBytes = 0
	if maxBytes > 0 && len(data) > maxBytes {
		dropped += int64(len(data) - maxBytes)
		data = data[len(data)-maxBytes:]
	}

	running := true
	select {
	case <-s.done:
		running = false
	default:
	}
	duration := time.Since(s.startedAt)
	if !running {
		duration = s.endedAt.Sub(s.startedAt)
	}
	return ReadOutput{
		Data:         string(data),
		Running:      running,
		Killed:       s.killed,
		ExitCode:     s.exitCode,
		Signal:       s.signal,
		DroppedBytes: dropped,
		StdoutBytes:  s.stdoutBytes,
		StderrBytes:  s.stderrBytes,
		Duration:     duration,
	}
}

// Write sends bytes to the process stdin. An empty string is a no-op, so
// callers can use Write("") as a pure poll. Writing after the process
// exits is an error.
func (s *Session) Write(chars string) error {
	if chars == "" {
		return nil
	}
	select {
	case <-s.done:
		return fmt.Errorf("session process has exited")
	default:
	}
	if _, err := io.WriteString(s.stdin, chars); err != nil {
		return fmt.Errorf("write stdin: %w", err)
	}
	return nil
}

// Close stdin signals EOF to an interactive program without killing it.
func (s *Session) CloseStdin() error {
	return s.stdin.Close()
}

// Kill terminates the whole process group (SIGTERM, then SIGKILL after the
// runner's grace period) and waits for the session to finish draining.
// Kill is idempotent.
func (s *Session) Kill() {
	s.killOnce.Do(func() { close(s.killCh) })
	<-s.done
}

// Done reports session completion: the process exited (or was killed) and
// its output has been fully drained into the window.
func (s *Session) Done() <-chan struct{} {
	return s.done
}

// Running reports whether the process is still alive.
func (s *Session) Running() bool {
	select {
	case <-s.done:
		return false
	default:
		return true
	}
}

func (s *Session) signalGroup(sig syscall.Signal) {
	signalUnixProcessGroup(s.pid, sig)
}

// exitStatusOf derives the exit code and signal name from the final process
// state and wait error, mirroring fillExitStatus for the async path.
func exitStatusOf(state *os.ProcessState, waitErr error) (int, string) {
	if state != nil {
		if status, ok := state.Sys().(syscall.WaitStatus); ok {
			switch {
			case status.Exited():
				return status.ExitStatus(), ""
			case status.Signaled():
				return -1, status.Signal().String()
			}
		}
		if state.Exited() {
			return state.ExitCode(), ""
		}
	}
	var exitErr *exec.ExitError
	if errors.As(waitErr, &exitErr) {
		return exitErr.ExitCode(), ""
	}
	return -1, ""
}
