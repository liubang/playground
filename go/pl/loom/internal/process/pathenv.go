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
// Created: 2026/08/09

package process

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// wellKnownToolchainDirs are the conventional install locations for user
// toolchains, in descending priority. They matter when loom is launched
// from a GUI context (Finder/launchd, desktop shortcuts): the process then
// inherits a sparse default PATH ("/usr/bin:/bin:...") without any user
// toolchain, and sandboxed commands inherit it verbatim — "go: command not
// found" style failures push the model toward pointless escalations. "~/"
// entries expand against the user's home. The mise shims directory alone
// covers every mise-managed tool (go, bazel, node, ...), so versioned
// install dirs never need listing here.
var wellKnownToolchainDirs = []string{
	"/opt/homebrew/bin",          // Homebrew (Apple Silicon)
	"/opt/homebrew/sbin",         // Homebrew (Apple Silicon)
	"/usr/local/bin",             // Homebrew (Intel) / manual installs
	"/usr/local/go/bin",          // official Go installer
	"~/.local/bin",               // pip --user, uv tools, misc user installs
	"~/.local/share/mise/shims",  // mise shims (go, bazel, node, ...)
	"~/.cargo/bin",               // rustup / cargo
	"~/go/bin",                   // GOPATH binaries (go install)
	"~/.bun/bin",                 // Bun
	"~/.volta/bin",               // Volta (node)
	"~/.pyenv/shims",             // pyenv shims
	"/opt/homebrew/opt/llvm/bin", // keg-only LLVM (clang, clang-format, ...)
}

// AugmentedPATH returns base with the existing well-known toolchain
// directories appended: entries already present (exact match) are kept in
// their original position so the user's own precedence always wins, and
// directories that do not exist are skipped. An empty base starts from
// defaultPATH, mirroring the runner's minimal-env fallback. dirExists is
// injectable for tests; pass osDirExists in production.
func AugmentedPATH(base, home string, dirExists func(string) bool) string {
	if strings.TrimSpace(base) == "" {
		base = defaultPATH
	}
	entries := strings.Split(base, ":")
	seen := make(map[string]struct{}, len(entries)+len(wellKnownToolchainDirs))
	for _, entry := range entries {
		seen[entry] = struct{}{}
	}
	for _, dir := range wellKnownToolchainDirs {
		expanded := dir
		if strings.HasPrefix(dir, "~/") {
			if home == "" {
				continue
			}
			expanded = filepath.Join(home, dir[2:])
		}
		if _, ok := seen[expanded]; ok {
			continue
		}
		if dirExists != nil && !dirExists(expanded) {
			continue
		}
		seen[expanded] = struct{}{}
		entries = append(entries, expanded)
	}
	return strings.Join(entries, ":")
}

// AugmentProcessPATH rewrites the process-level PATH with AugmentedPATH and
// reports the directories that were appended (empty when PATH was already
// complete). The process level — rather than only the sandboxed child's
// environment — is augmented on purpose: exec.LookPath resolves programs
// against the parent PATH, and both the minimal (sandboxed) and full
// (escalated) child environments derive from it. Idempotent.
func AugmentProcessPATH() []string {
	before := os.Getenv("PATH")
	after := AugmentedPATH(before, userHome(), osDirExists)
	if after == before {
		return nil
	}
	if err := os.Setenv("PATH", after); err != nil {
		return nil
	}
	base := len(strings.Split(before, ":"))
	return strings.Split(after, ":")[base:]
}

func userHome() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return home
}

// toolchainCacheDirs are regenerable per-user caches written by build
// toolchains ("~/" expands against the user's home). Sandbox-writable via
// ExtraWritableDirs: a corrupted cache costs one rebuild, and the
// sensitive-path denies never cover them.
var toolchainCacheDirs = []string{
	"~/Library/Caches/go-build", // GOCACHE default (macOS)
	"~/.cache/go-build",         // GOCACHE default (Linux)
	"~/go/pkg/mod",              // GOMODCACHE default
	"~/.npm",                    // npm cache
	"~/Library/Caches/pip",      // pip cache (macOS)
	"~/.cache/pip",              // pip cache (Linux)
	"~/.cargo/registry",         // cargo crate cache
	"~/.cargo/git",              // cargo git checkouts
	"~/Library/Caches/ccache",   // ccache (macOS)
	"~/.cache/ccache",           // ccache (Linux)
	"~/.gradle",                 // Gradle caches + wrapper dists
	"~/.m2/repository",          // Maven local repository
}

// ExtraWritableDirs returns the canonical directories every sandboxed
// command may write beyond the workspace: the system scratch dirs
// (workspace.ScratchDirs — $TMPDIR and /tmp) plus the regenerable
// toolchain caches, so builds work out of the box instead of pushing the
// model toward cache-redirection hacks or escalations.
func ExtraWritableDirs() []string {
	dirs := workspacepkg.ScratchDirs()
	if home := userHome(); home != "" {
		for _, dir := range toolchainCacheDirs {
			dirs = append(dirs, workspacepkg.Canonicalize(filepath.Join(home, dir[2:])))
		}
	}
	return uniqueCleanPaths(dirs)
}

// uniqueCleanPaths cleans, dedupes, and sorts absolute paths; relative
// and empty entries are dropped. Platform-neutral home (moved from the
// darwin sandbox file).
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

func osDirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}
