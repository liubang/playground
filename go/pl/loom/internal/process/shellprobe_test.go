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
	"context"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"sync/atomic"
	"testing"
	"time"
)

// resetShellProbe restores the package-level probe state and test seams.
// Tests in this package run sequentially, so the global state is safe.
func resetShellProbe(t *testing.T) {
	t.Helper()
	origRun, origNow := shellProbeRun, shellProbeNow
	t.Cleanup(func() {
		shellProbeRun, shellProbeNow = origRun, origNow
		shellProbe.Lock()
		defer shellProbe.Unlock()
		shellProbe.cachePath = ""
		shellProbe.shell = ""
		shellProbe.home = ""
		shellProbe.entries = nil
		shellProbe.probedAt = time.Time{}
		shellProbe.loaded = false
		shellProbe.refreshing = false
	})
}

// writeFakeShell creates an executable that prints rc-style chatter around
// the sentinel-wrapped PATH, standing in for `zsh -lic`.
func writeFakeShell(t *testing.T, pathLine string) string {
	t.Helper()
	script := fmt.Sprintf("#!/bin/sh\n"+
		"echo 'welcome, user'\n"+
		"printf '%%s\\n%%s\\n%%s\\n' %s '%s' %s\n"+
		"echo 'trailing junk'\n",
		shellProbeBeginMarker, pathLine, shellProbeEndMarker)
	path := filepath.Join(t.TempDir(), "fakezsh")
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return path
}

func eventually(t *testing.T, timeout time.Duration, cond func() bool, msg string) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("condition not met within %v: %s", timeout, msg)
}

func TestProbeLoginShellPATHExtractsSentinelWrappedPATH(t *testing.T) {
	shell := writeFakeShell(t, "/probe/a:/probe/b:relative/c")
	entries, err := probeLoginShellPATH(context.Background(), shell, t.TempDir())
	if err != nil {
		t.Fatalf("probeLoginShellPATH() error = %v", err)
	}
	// Relative entries are dropped: they would resolve against an
	// arbitrary cwd inside the sandbox.
	want := []string{"/probe/a", "/probe/b"}
	if !reflect.DeepEqual(entries, want) {
		t.Fatalf("entries = %v, want %v", entries, want)
	}
}

func TestProbeLoginShellPATHRejectsGarbage(t *testing.T) {
	bodies := map[string]string{
		"no markers":    "#!/bin/sh\necho 'just chatter'\n",
		"empty PATH":    "#!/bin/sh\nprintf '%s\\n%s\\n%s\\n' " + shellProbeBeginMarker + " '' " + shellProbeEndMarker + "\n",
		"only relative": "#!/bin/sh\nprintf '%s\\n%s\\n%s\\n' " + shellProbeBeginMarker + " 'a:b' " + shellProbeEndMarker + "\n",
	}
	for name, body := range bodies {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "fakezsh")
			if err := os.WriteFile(path, []byte(body), 0o755); err != nil {
				t.Fatal(err)
			}
			if _, err := probeLoginShellPATH(context.Background(), path, t.TempDir()); err == nil {
				t.Fatal("probeLoginShellPATH() error = nil, want failure")
			}
		})
	}
}

func TestCappedWriterDiscardsOverflow(t *testing.T) {
	w := &cappedWriter{limit: 8}
	n, err := w.Write([]byte("0123456789abcdef"))
	if n != 16 || err != nil {
		t.Fatalf("Write() = %d, %v, want 16, nil (short writes stay invisible to the child)", n, err)
	}
	if got := string(w.Bytes()); got != "01234567" {
		t.Fatalf("Bytes() = %q, want %q", got, "01234567")
	}
	// Once full, later writes are swallowed whole so a flooding rc file
	// cannot grow the buffer without bound.
	if n, err := w.Write([]byte("more")); n != 4 || err != nil || string(w.Bytes()) != "01234567" {
		t.Fatalf("post-limit Write() = %d, %v, buffer %q", n, err, string(w.Bytes()))
	}
}

func TestShellProbeDirsUsesFreshCache(t *testing.T) {
	resetShellProbe(t)
	t.Setenv("SHELL", "/bin/zsh")
	cacheDir := t.TempDir()
	entries := []string{"/shell/a", "/shell/b"}
	if err := writeShellProbeCache(filepath.Join(cacheDir, "login_shell_path.json"), shellProbeCache{
		Shell:    "/bin/zsh",
		ProbedAt: shellProbeNow(),
		Entries:  entries,
	}); err != nil {
		t.Fatal(err)
	}
	runs := &atomic.Int32{}
	shellProbeRun = func(context.Context, string, string) ([]string, error) {
		runs.Add(1)
		return nil, nil
	}

	ConfigureShellPathProbe(cacheDir)
	if got := shellProbeDirs(); !reflect.DeepEqual(got, entries) {
		t.Fatalf("shellProbeDirs() = %v, want cached %v", got, entries)
	}
	if got := runs.Load(); got != 0 {
		t.Fatalf("probe ran %d times on a fresh cache, want 0", got)
	}
}

func TestShellProbeDirsRefreshesStaleCacheInBackground(t *testing.T) {
	resetShellProbe(t)
	t.Setenv("SHELL", "/bin/zsh")
	cacheDir := t.TempDir()
	cachePath := filepath.Join(cacheDir, "login_shell_path.json")
	stale := []string{"/shell/stale"}
	if err := writeShellProbeCache(cachePath, shellProbeCache{
		Shell:    "/bin/zsh",
		ProbedAt: shellProbeNow().Add(-shellProbeTTL - time.Minute),
		Entries:  stale,
	}); err != nil {
		t.Fatal(err)
	}

	release := make(chan struct{})
	runs := &atomic.Int32{}
	fresh := []string{"/shell/fresh"}
	shellProbeRun = func(context.Context, string, string) ([]string, error) {
		runs.Add(1)
		<-release
		return fresh, nil
	}

	ConfigureShellPathProbe(cacheDir)
	// Stale entries keep serving while the refresh is in flight.
	if got := shellProbeDirs(); !reflect.DeepEqual(got, stale) {
		t.Fatalf("shellProbeDirs() = %v, want stale %v during refresh", got, stale)
	}
	// A second call must not kick a duplicate refresh (asserted via the
	// total run count below: a duplicate would block on the same release
	// channel and push the count to 2).
	_ = shellProbeDirs()

	close(release)
	eventually(t, 2*time.Second, func() bool {
		shellProbe.Lock()
		defer shellProbe.Unlock()
		return !shellProbe.refreshing && reflect.DeepEqual(shellProbe.entries, fresh)
	}, "refresh completes and publishes fresh entries")
	if got := runs.Load(); got != 1 {
		t.Fatalf("probe runs = %d, want exactly 1 (no duplicate refresh)", got)
	}

	cache, err := readShellProbeCache(cachePath)
	if err != nil {
		t.Fatalf("readShellProbeCache() error = %v", err)
	}
	if !reflect.DeepEqual(cache.Entries, fresh) {
		t.Fatalf("persisted entries = %v, want %v", cache.Entries, fresh)
	}
}

func TestShellProbeDisabledWithoutConfigure(t *testing.T) {
	resetShellProbe(t)
	ConfigureShellPathProbe("")
	if got := shellProbeDirs(); got != nil {
		t.Fatalf("shellProbeDirs() = %v, want nil when disabled", got)
	}
}

func TestShellProbeDisabledForUnsupportedShell(t *testing.T) {
	resetShellProbe(t)
	t.Setenv("SHELL", "/opt/homebrew/bin/fish")
	ConfigureShellPathProbe(t.TempDir())
	if got := shellProbeDirs(); got != nil {
		t.Fatalf("shellProbeDirs() = %v, want nil for unsupported shell", got)
	}
}

func TestShellProbeIgnoresCacheFromForeignShell(t *testing.T) {
	resetShellProbe(t)
	t.Setenv("SHELL", "/bin/zsh")
	cacheDir := t.TempDir()
	if err := writeShellProbeCache(filepath.Join(cacheDir, "login_shell_path.json"), shellProbeCache{
		Shell:    "/bin/bash", // probed under a different shell
		ProbedAt: shellProbeNow(),
		Entries:  []string{"/shell/foreign"},
	}); err != nil {
		t.Fatal(err)
	}
	release := make(chan struct{})
	shellProbeRun = func(context.Context, string, string) ([]string, error) {
		<-release
		return []string{"/shell/fresh"}, nil
	}
	t.Cleanup(func() { close(release) })

	ConfigureShellPathProbe(cacheDir)
	if got := shellProbeDirs(); got != nil {
		t.Fatalf("shellProbeDirs() = %v, want nil for foreign-shell cache", got)
	}
}
