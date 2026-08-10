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
	"sync"

	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// DirSource classifies how a candidate PATH directory was nominated.
type DirSource string

const (
	// DirSourceConfig marks directories from the tools.path_extra config;
	// explicit user configuration outranks every built-in candidate.
	DirSourceConfig DirSource = "config"
	// DirSourceShell marks directories captured from the user's login
	// shell by the probe (shellprobe.go): the ambient environment the
	// user actually works in, outranking loom's built-in guesses.
	DirSourceShell DirSource = "shell"
	// DirSourceStatic marks directories from the curated well-known list.
	DirSourceStatic DirSource = "static"
	// DirSourceGlob marks directories expanded from a versioned-layout
	// pattern (e.g. SDKMAN candidates).
	DirSourceGlob DirSource = "glob"
)

// DirStatus records what happened to one candidate directory.
type DirStatus string

const (
	// DirStatusPrepended means loom added the directory ahead of the
	// inherited PATH.
	DirStatusPrepended DirStatus = "prepended"
	// DirStatusExisting means the directory was already on the inherited
	// PATH; its position is left untouched (the user's own order wins).
	DirStatusExisting DirStatus = "existing"
	// DirStatusMissing means the directory does not exist on disk; it is
	// reported for visibility but never added to the PATH.
	DirStatusMissing DirStatus = "missing"
)

// PathDir is one candidate directory with its attribution.
type PathDir struct {
	Path   string    `json:"path"`
	Source DirSource `json:"source"`
	Status DirStatus `json:"status"`
}

// ToolResolution reports where one key tool resolves on the effective PATH
// (empty Path when not found). Versions are deliberately not probed:
// running --version per tool costs real time at startup.
type ToolResolution struct {
	Name  string `json:"name"`
	Path  string `json:"path,omitempty"`
	Found bool   `json:"found"`
}

// ToolchainReport is the point-in-time snapshot of the PATH augmentation:
// what the effective PATH is, which directories loom prepended, how every
// known candidate fared, and where the key tools resolve. Produced by
// AugmentProcessPATH and served read-only to frontends (settings card,
// /doctor). Consumers must not mutate the returned slices.
type ToolchainReport struct {
	EffectivePATH string           `json:"effective_path"`
	AddedDirs     []string         `json:"added_dirs"`
	Dirs          []PathDir        `json:"dirs"`
	Tools         []ToolResolution `json:"tools"`
}

// keyToolNames are the tools whose resolution the report surfaces; the
// list stays short so the settings card answers "can the sandbox build my
// project" at a glance.
var keyToolNames = []string{"go", "bazel", "node", "npm", "python3", "cargo", "java", "rg"}

// pathAugment holds the process-level augmentation state. added tracks the
// directories loom prepended on the last AugmentProcessPATH call so a
// config hot-apply can rebuild from the pre-augmentation base: attribution
// stays correct, and a path_extra entry removed from the config drops out
// of the PATH instead of lingering.
var pathAugment = struct {
	sync.Mutex
	added  []string
	report *ToolchainReport
}{}

// ResolveToolchainPATH computes the effective PATH and the per-candidate
// attribution in one pass: config extras first, then the probed
// login-shell directories, then the static well-known list, then glob
// expansions, all prepended ahead of the base entries in that priority
// order. Base entries are never reordered or duplicated; an empty base
// starts from defaultPATH, mirroring the runner's minimal-env fallback.
// dirExists and globFn are injectable for tests.
func ResolveToolchainPATH(base, home string, extra, shellDirs []string, dirExists func(string) bool, globFn func(string) ([]string, error)) (string, []PathDir) {
	if strings.TrimSpace(base) == "" {
		base = defaultPATH
	}
	baseEntries := strings.Split(base, string(os.PathListSeparator))
	seen := make(map[string]struct{}, len(baseEntries)+len(wellKnownToolchainDirs))
	for _, entry := range baseEntries {
		seen[entry] = struct{}{}
	}

	var dirs []PathDir
	prepend := make([]string, 0, len(wellKnownToolchainDirs))
	consider := func(path string, source DirSource) {
		if _, ok := seen[path]; ok {
			dirs = append(dirs, PathDir{Path: path, Source: source, Status: DirStatusExisting})
			return
		}
		seen[path] = struct{}{}
		if dirExists != nil && !dirExists(path) {
			dirs = append(dirs, PathDir{Path: path, Source: source, Status: DirStatusMissing})
			return
		}
		dirs = append(dirs, PathDir{Path: path, Source: source, Status: DirStatusPrepended})
		prepend = append(prepend, path)
	}

	for _, raw := range extra {
		if expanded := expandHomeDir(raw, home); expanded != "" {
			consider(expanded, DirSourceConfig)
		}
	}
	for _, dir := range shellDirs {
		if strings.TrimSpace(dir) != "" {
			consider(dir, DirSourceShell)
		}
	}
	for _, dir := range wellKnownToolchainDirs {
		if expanded := expandHomeDir(dir, home); expanded != "" {
			consider(expanded, DirSourceStatic)
		}
	}
	if globFn != nil {
		for _, pattern := range wellKnownToolchainGlobs {
			expanded := expandHomeDir(pattern, home)
			if expanded == "" {
				continue
			}
			matches, err := globFn(expanded)
			if err != nil {
				continue
			}
			if len(matches) > maxGlobExpansions {
				matches = matches[:maxGlobExpansions]
			}
			for _, match := range matches {
				consider(match, DirSourceGlob)
			}
		}
	}

	entries := append(prepend, baseEntries...)
	return strings.Join(entries, string(os.PathListSeparator)), dirs
}

// AugmentProcessPATH rewrites the process-level PATH with the resolved
// toolchain directories and reports the directories that were prepended
// (empty when PATH was already complete). The process level — rather than
// only the sandboxed child's environment — is augmented on purpose:
// exec.LookPath resolves programs against the parent PATH, and both the
// minimal (sandboxed) and full (escalated) child environments derive from
// it. Safe to call again on config hot-apply: the rebuild starts from the
// pre-augmentation base, so changed tools.path_extra entries take effect
// and removed ones drop out. The resulting ToolchainReport is cached for
// CurrentToolchainReport.
func AugmentProcessPATH(extra []string) []string {
	pathAugment.Lock()
	defer pathAugment.Unlock()

	current := os.Getenv("PATH")
	base := subtractPathList(current, pathAugment.added)
	effective, dirs := ResolveToolchainPATH(base, userHome(), extra, shellProbeDirs(), osDirExists, filepath.Glob)
	if effective != current {
		if err := os.Setenv("PATH", effective); err != nil {
			return nil
		}
	}
	added := addedDirsOf(dirs)
	pathAugment.added = added
	pathAugment.report = &ToolchainReport{
		EffectivePATH: effective,
		AddedDirs:     added,
		Dirs:          dirs,
		Tools:         probeKeyTools(effective),
	}
	return added
}

// CurrentToolchainReport returns the latest augmentation snapshot, or nil
// when AugmentProcessPATH never ran (e.g. a bare runner in tests).
func CurrentToolchainReport() *ToolchainReport {
	pathAugment.Lock()
	defer pathAugment.Unlock()
	return pathAugment.report
}

// addedDirsOf collects the prepended directories in priority order.
func addedDirsOf(dirs []PathDir) []string {
	var added []string
	for _, d := range dirs {
		if d.Status == DirStatusPrepended {
			added = append(added, d.Path)
		}
	}
	return added
}

// subtractPathList removes the given entries from a PATH-style list,
// preserving the order of the survivors.
func subtractPathList(list string, remove []string) string {
	if len(remove) == 0 || list == "" {
		return list
	}
	drop := make(map[string]struct{}, len(remove))
	for _, entry := range remove {
		drop[entry] = struct{}{}
	}
	entries := strings.Split(list, string(os.PathListSeparator))
	kept := entries[:0]
	for _, entry := range entries {
		if _, ok := drop[entry]; !ok {
			kept = append(kept, entry)
		}
	}
	return strings.Join(kept, string(os.PathListSeparator))
}

// expandHomeDir expands a leading "~/" against home; tilde paths without a
// home yield "" (skipped by the caller).
func expandHomeDir(path, home string) string {
	path = strings.TrimSpace(path)
	if strings.HasPrefix(path, "~/") {
		if home == "" {
			return ""
		}
		return filepath.Join(home, path[2:])
	}
	return path
}

// probeKeyTools resolves the key tools against an explicit PATH list
// (exec.LookPath would read the process env, which may lag the value being
// computed).
func probeKeyTools(pathEnv string) []ToolResolution {
	out := make([]ToolResolution, 0, len(keyToolNames))
	for _, name := range keyToolNames {
		path := lookPathOn(pathEnv, name)
		out = append(out, ToolResolution{Name: name, Path: path, Found: path != ""})
	}
	return out
}

// lookPathOn resolves name against an explicit PATH list: the first
// existing executable file wins.
func lookPathOn(pathEnv, name string) string {
	for _, dir := range strings.Split(pathEnv, string(os.PathListSeparator)) {
		if dir == "" {
			continue
		}
		full := filepath.Join(dir, name)
		info, err := os.Stat(full)
		if err == nil && !info.IsDir() && info.Mode()&0o111 != 0 {
			return full
		}
	}
	return ""
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
	"~/Library/Caches/mise",     // mise exec-env cache (macOS)
	"~/.cache/mise",             // mise exec-env cache (Linux)
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
