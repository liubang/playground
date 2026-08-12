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
// Created: 2026/08/12

package workspace

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// This file is the SINGLE source of truth for sensitive-path policy. Two
// enforcement layers consume it and must never keep their own copies:
//
//   - the OS sandbox (process/sandbox_darwin.go) derives its seatbelt
//     read/unlink deny rules from SensitiveHomeSubpaths/SensitiveHomeLiterals;
//   - the user-space path validators (PathValidator, and through it every
//     builtin file tool) deny via IsSensitive/ContainsSensitiveComponent/
//     IsSensitiveAbsolute.
//
// The two lists answer different questions: the component list names path
// components that are sensitive WHEREVER they appear (a `.git` directory is
// protected in any repository, not just the workspace one); the home lists
// name credential locations rooted at the user's home directory (`.aws` is
// only sensitive at ~/.aws, an `aws` directory elsewhere is ordinary data).

// sensitiveComponents are base names denied wherever they appear in a
// path: repository metadata that can escalate the agent (.git), and
// secret-bearing files by convention (.env, credentials.json, ...).
var sensitiveComponents = []string{
	".git",
	".ssh",
	".gnupg",
	".env",
	".credentials",
	"credentials.json",
	"service-account.json",
}

// sensitiveHomeSubpaths are credential-like DIRECTORIES under the user's
// home; everything beneath them is sensitive.
var sensitiveHomeSubpaths = []string{
	".ssh",
	".gnupg",
	".aws",
	".azure",
	".kube",
	".docker",
	".config/gcloud",
	".config/gh",
	".config/snowflake",
	"Library/Keychains",
}

// sensitiveHomeLiterals are credential FILES directly under the user's home.
var sensitiveHomeLiterals = []string{
	".netrc",
	".git-credentials",
	".env",
	"credentials.json",
	"service-account.json",
	".npmrc",
	".pypirc",
}

// SensitiveHome returns the canonicalized home directory used to root the
// home-relative sensitive lists. Canonicalization matters: enforcement
// layers (seatbelt profiles, resolved validator paths) match canonical
// paths, so a symlinked HOME must be resolved or the denies silently match
// nothing. Empty when the home directory cannot be determined.
// The canonicalization is cached keyed by the raw home (IsSensitiveAbsolute
// sits on directory-walk hot paths), but the raw home itself is re-read
// every call so tests overriding $HOME still take effect.
var SensitiveHome = func() func() string {
	var mu sync.Mutex
	var cachedRaw, cachedCanonical string
	return func() string {
		home, err := os.UserHomeDir()
		if err != nil || home == "" {
			return ""
		}
		mu.Lock()
		defer mu.Unlock()
		if home == cachedRaw {
			return cachedCanonical
		}
		canonical := Canonicalize(home)
		cachedRaw, cachedCanonical = home, canonical
		return canonical
	}
}()

// SensitiveHomeSubpaths returns the home-relative sensitive directories
// (defensive copy).
func SensitiveHomeSubpaths() []string {
	return append([]string(nil), sensitiveHomeSubpaths...)
}

// SensitiveComponents returns the sensitive base names (defensive copy).
// External engines that enforce their own traversal (ripgrep) derive
// their exclusion rules from this list instead of duplicating it.
func SensitiveComponents() []string {
	return append([]string(nil), sensitiveComponents...)
}

// SensitiveHomeLiterals returns the home-relative sensitive files
// (defensive copy).
func SensitiveHomeLiterals() []string {
	return append([]string(nil), sensitiveHomeLiterals...)
}

// IsSensitive reports whether a single path component (a base name) is on
// the sensitive component list.
func IsSensitive(path string) bool {
	base := filepath.Base(path)
	for _, s := range sensitiveComponents {
		if base == s {
			return true
		}
	}
	return false
}

// ContainsSensitiveComponent reports whether any component of the path is
// on the sensitive list (see IsSensitive). Tool packages must call this
// instead of keeping their own copy of the list (REVIEW R4).
func ContainsSensitiveComponent(path string) bool {
	clean := filepath.Clean(path)
	if clean == "." || clean == string(filepath.Separator) {
		return false
	}
	for _, part := range strings.Split(clean, string(filepath.Separator)) {
		if IsSensitive(part) {
			return true
		}
	}
	return false
}

// IsSensitiveAbsolute reports whether a canonical absolute path (one
// returned by Canonicalize, Validate, or ValidateRead) lands in a sensitive
// location: under a sensitive home directory, exactly a sensitive home
// file, or containing a sensitive component anywhere along the path.
// It is the gate for paths OUTSIDE the workspace roots, where the
// workspace-relative component check does not apply.
func IsSensitiveAbsolute(path string) bool {
	if !filepath.IsAbs(path) {
		return false
	}
	if ContainsSensitiveComponent(path) {
		return true
	}
	home := SensitiveHome()
	if home == "" {
		return false
	}
	for _, rel := range sensitiveHomeSubpaths {
		if isUnderRoot(path, filepath.Join(home, rel)) {
			return true
		}
	}
	for _, rel := range sensitiveHomeLiterals {
		if path == filepath.Join(home, rel) {
			return true
		}
	}
	return false
}
