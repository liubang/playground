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
// Created: 2026/08/10

package process

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Login-shell PATH probing, in one sentence: ask the user's own shell for
// its PATH so a GUI-launched loom resolves tools exactly like the user's
// terminal would (custom dirs the curated list can never guess, real
// binaries instead of version-manager shims).
//
// The probe NEVER blocks command execution: AugmentProcessPATH reads the
// last cached result and, when it is stale or missing, kicks an async
// refresh. Failure at any step (unsupported shell, rc garbage, hang,
// timeout) silently degrades to the curated well-known list — the probe
// is an enhancement layer, never a dependency.
const (
	// shellProbeTimeout bounds one probe run. Interactive rc files
	// (oh-my-zsh, nvm, pyenv) dominate the cost: ~0.1s on a lean setup,
	// 1-3s on heavy ones. Beyond this we assume a hang and fall back.
	shellProbeTimeout = 3 * time.Second
	// shellProbeTTL is how long a cached probe result is trusted before
	// the next AugmentProcessPATH call refreshes it in the background.
	shellProbeTTL = 24 * time.Hour
	// shellProbeOutputCap bounds captured stdout so a runaway rc file
	// (e.g. `yes`) cannot exhaust memory before the timeout kills it.
	shellProbeOutputCap = 64 << 10

	shellProbeBeginMarker = "__LOOM_PATH_BEGIN__"
	shellProbeEndMarker   = "__LOOM_PATH_END__"
)

// shellProbeBaselinePATH is the environment PATH the probe shell itself
// starts with: enough for rc files that call bare `brew`/`mise` before
// they export their own PATH. It only affects probe startup, never the
// probed result (rc prepends its own entries on top).
const shellProbeBaselinePATH = "/opt/homebrew/bin:/usr/local/bin:" + defaultPATH

// probeSupportedShells dispatch on the login/interactive flags understood
// by POSIX-style shells. Others (fish, nushell, dash-as-sh) have
// incompatible flag or list semantics and silently fall back to the
// curated list.
var probeSupportedShells = map[string]struct{}{
	"zsh":  {},
	"bash": {},
}

// shellProbeCache is the on-disk JSON snapshot at <CacheDir>/login_shell_path.json.
type shellProbeCache struct {
	Shell    string    `json:"shell"`
	ProbedAt time.Time `json:"probed_at"`
	Entries  []string  `json:"entries"`
}

// shellProbe holds the package-level probe state, configured once at
// process startup via ConfigureShellPathProbe. Lock ordering: callers may
// hold pathAugment's lock while taking this one, never the reverse (the
// refresh goroutine touches only this state).
var shellProbe = struct {
	sync.Mutex
	cachePath  string
	shell      string
	home       string
	entries    []string
	probedAt   time.Time
	loaded     bool
	refreshing bool
}{}

// Test seams: the subprocess runner and the clock are replaceable.
var (
	shellProbeRun = probeLoginShellPATH
	shellProbeNow = time.Now
)

// ConfigureShellPathProbe points the probe at the loom cache directory.
// An empty cacheDir, a missing $SHELL, or an unsupported shell disables
// the probe (the zero state is the disabled state). Safe to call again:
// the state resets, so the next shellProbeDirs re-reads the cache.
func ConfigureShellPathProbe(cacheDir string) {
	shellProbe.Lock()
	defer shellProbe.Unlock()
	shellProbe.cachePath = ""
	shellProbe.shell = ""
	shellProbe.home = ""
	shellProbe.entries = nil
	shellProbe.probedAt = time.Time{}
	shellProbe.loaded = false
	shellProbe.refreshing = false
	if strings.TrimSpace(cacheDir) == "" {
		return
	}
	shell := os.Getenv("SHELL")
	if _, ok := probeSupportedShells[filepath.Base(shell)]; !ok {
		return
	}
	shellProbe.cachePath = filepath.Join(cacheDir, "login_shell_path.json")
	shellProbe.shell = shell
	shellProbe.home = userHome()
}

// shellProbeDirs returns the last known login-shell PATH entries (nil
// when disabled or never probed) and kicks one async refresh when the
// cache is stale. Callers must tolerate stale results — the refresh
// lands in the cache and takes effect on the next AugmentProcessPATH.
func shellProbeDirs() []string {
	shellProbe.Lock()
	defer shellProbe.Unlock()
	if shellProbe.cachePath == "" {
		return nil
	}
	if !shellProbe.loaded {
		shellProbe.loaded = true
		if cache, err := readShellProbeCache(shellProbe.cachePath); err == nil && cache.Shell == shellProbe.shell {
			shellProbe.entries = cache.Entries
			shellProbe.probedAt = cache.ProbedAt
		}
	}
	stale := shellProbe.probedAt.IsZero() || shellProbeNow().Sub(shellProbe.probedAt) >= shellProbeTTL
	if stale && !shellProbe.refreshing {
		shellProbe.refreshing = true
		go refreshShellProbe()
	}
	return shellProbe.entries
}

// refreshShellProbe runs one probe and publishes the result to memory and
// disk. Failures leave the previous state untouched.
func refreshShellProbe() {
	// Snapshot the config and the test seams under the lock: the refresh
	// goroutine must never re-read package-level seams mid-flight (a test
	// cleanup restoring them would race this read).
	shellProbe.Lock()
	cachePath, shell, home := shellProbe.cachePath, shellProbe.shell, shellProbe.home
	run, now := shellProbeRun, shellProbeNow
	shellProbe.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), shellProbeTimeout)
	entries, err := run(ctx, shell, home)
	cancel()

	shellProbe.Lock()
	if err != nil || len(entries) == 0 {
		shellProbe.refreshing = false
		shellProbe.Unlock()
		return
	}
	shellProbe.entries = entries
	shellProbe.probedAt = now()
	probedAt := shellProbe.probedAt
	shellProbe.Unlock()

	_ = writeShellProbeCache(cachePath, shellProbeCache{
		Shell:    shell,
		ProbedAt: probedAt,
		Entries:  entries,
	})

	// Clear refreshing only after the disk write lands, so
	// "refreshing == false" means memory and disk are both published and
	// waiters can rely on the persisted cache being fresh.
	shellProbe.Lock()
	shellProbe.refreshing = false
	shellProbe.Unlock()
}

func readShellProbeCache(path string) (shellProbeCache, error) {
	var cache shellProbeCache
	data, err := os.ReadFile(path)
	if err != nil {
		return cache, err
	}
	if err := json.Unmarshal(data, &cache); err != nil {
		return shellProbeCache{}, err
	}
	if len(cache.Entries) == 0 {
		return shellProbeCache{}, errors.New("probe cache has no entries")
	}
	return cache, nil
}

func writeShellProbeCache(path string, cache shellProbeCache) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	data, err := json.Marshal(cache)
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

// probeLoginShellPATH captures the PATH a login+interactive shell
// computes from the user's rc files. The child gets a scrubbed
// environment (no inherited secrets, TERM=dumb, a minimal baseline PATH);
// the output is extracted between sentinel lines so rc chatter (motd,
// greetings) cannot corrupt the result. The timeout kill targets the
// shell itself — a hung rc may leave a bounded orphan behind, which is
// acceptable for a best-effort probe.
func probeLoginShellPATH(ctx context.Context, shell, home string) ([]string, error) {
	script := fmt.Sprintf("printf '%%s\\n%%s\\n%%s\\n' %s \"$PATH\" %s",
		shellProbeBeginMarker, shellProbeEndMarker)
	cmd := exec.CommandContext(ctx, shell, "-lic", script)
	cmd.Env = []string{
		"HOME=" + home,
		"SHELL=" + shell,
		"TERM=dumb",
		"LANG=C.UTF-8",
		"PATH=" + shellProbeBaselinePATH,
	}
	stdout := &cappedWriter{limit: shellProbeOutputCap}
	cmd.Stdout = stdout
	cmd.Stderr = nil
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("probe login shell: %w", err)
	}
	return parseProbedPATH(stdout.Bytes())
}

// parseProbedPATH extracts the single PATH line between the sentinels and
// keeps absolute entries only (relative entries would resolve against an
// arbitrary cwd inside the sandbox).
func parseProbedPATH(out []byte) ([]string, error) {
	begin := bytes.Index(out, []byte(shellProbeBeginMarker+"\n"))
	if begin < 0 {
		return nil, errors.New("probe output missing begin marker")
	}
	body := out[begin+len(shellProbeBeginMarker)+1:]
	end := bytes.Index(body, []byte(shellProbeEndMarker))
	if end < 0 {
		return nil, errors.New("probe output missing end marker")
	}
	raw := strings.TrimSpace(string(body[:end]))
	if raw == "" {
		return nil, errors.New("probe output has empty PATH")
	}
	var entries []string
	for _, entry := range strings.Split(raw, string(os.PathListSeparator)) {
		if filepath.IsAbs(entry) {
			entries = append(entries, entry)
		}
	}
	if len(entries) == 0 {
		return nil, errors.New("probe output has no absolute PATH entries")
	}
	return entries, nil
}

// cappedWriter discards everything past the limit so unbounded child
// output cannot exhaust memory; the resulting truncated output simply
// fails marker extraction and degrades to the curated list.
type cappedWriter struct {
	buf   bytes.Buffer
	limit int
}

func (w *cappedWriter) Write(p []byte) (int, error) {
	n := len(p)
	if remaining := w.limit - w.buf.Len(); remaining > 0 {
		if len(p) > remaining {
			p = p[:remaining]
		}
		_, _ = w.buf.Write(p)
	}
	// Report the original length: a short write with a nil error is an
	// io.Writer contract violation (io.Copy surfaces ErrShortWrite), and
	// a visible error would fail the probe instead of just truncating.
	return n, nil
}

func (w *cappedWriter) Bytes() []byte { return w.buf.Bytes() }
