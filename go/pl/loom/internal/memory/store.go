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
// Created: 2026/08/02

// Package memory implements the Codex-style long-term memory system:
// two-phase extraction/consolidation pipeline, file-based storage with
// git versioning, and progressive disclosure via memory tools.
package memory

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// DirName is the memories directory name under the loom base directory
// (config.ResolvedStorage.MemoriesDir).
const DirName = "memories"

// SummaryFile is the hot-tier memory injected into the system prompt.
const SummaryFile = "memory_summary.md"

// MainFile is the warm-tier searchable memory handbook.
const MainFile = "MEMORY.md"

// RawFile is the cold-tier raw extraction staging area.
const RawFile = "raw_memories.md"

// RolloutDir holds per-session rollout summaries.
const RolloutDir = "rollout_summaries"

// SkillsDir holds reusable procedure definitions.
const SkillsDir = "skills"

// NotesDir holds ad-hoc user-flagged notes.
const NotesDir = "extensions/ad_hoc/notes"

// errLimitReached is the sentinel for Search early termination.
var errLimitReached = errors.New("limit reached")

// Store manages the memory directory tree (by default <loom home>/memories).
type Store struct {
	root string
}

// OpenStore creates or opens the memory store at the given root; the root
// is required (the caller derives it from the loom home — the config
// file's directory).
func OpenStore(root string) (*Store, error) {
	if root == "" {
		return nil, fmt.Errorf("memory store root is required")
	}
	if err := os.MkdirAll(root, 0o700); err != nil {
		return nil, fmt.Errorf("create memory root: %w", err)
	}
	s := &Store{root: root}
	// Ensure sub-directories exist.
	for _, dir := range []string{RolloutDir, SkillsDir, NotesDir} {
		if err := os.MkdirAll(filepath.Join(root, dir), 0o700); err != nil {
			return nil, fmt.Errorf("create memory subdir %s: %w", dir, err)
		}
	}
	return s, nil
}

// Root returns the memory store root path.
func (s *Store) Root() string { return s.root }

// ReadSummary reads memory_summary.md (hot tier). Returns empty string
// if the file does not exist.
func (s *Store) ReadSummary() (string, error) {
	data, err := os.ReadFile(filepath.Join(s.root, SummaryFile))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	return string(data), nil
}

// WriteSummary writes memory_summary.md (hot tier).
func (s *Store) WriteSummary(content string) error {
	return os.WriteFile(filepath.Join(s.root, SummaryFile), []byte(content), 0o600)
}

// ReadMain reads MEMORY.md (warm tier). Returns empty string if not found.
func (s *Store) ReadMain() (string, error) {
	data, err := os.ReadFile(filepath.Join(s.root, MainFile))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	return string(data), nil
}

// WriteMain writes MEMORY.md (warm tier).
func (s *Store) WriteMain(content string) error {
	return os.WriteFile(filepath.Join(s.root, MainFile), []byte(content), 0o600)
}

// ReadRaw reads raw_memories.md (cold tier staging). Returns empty string
// if not found.
func (s *Store) ReadRaw() (string, error) {
	data, err := os.ReadFile(filepath.Join(s.root, RawFile))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	return string(data), nil
}

// WriteRaw writes raw_memories.md (cold tier staging).
func (s *Store) WriteRaw(content string) error {
	return os.WriteFile(filepath.Join(s.root, RawFile), []byte(content), 0o600)
}

// AppendRaw appends content to raw_memories.md.
func (s *Store) AppendRaw(content string) error {
	f, err := os.OpenFile(filepath.Join(s.root, RawFile), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = f.WriteString(content)
	return err
}

// WriteRolloutSummary writes a per-session rollout summary.
func (s *Store) WriteRolloutSummary(slug, content string) error {
	return os.WriteFile(filepath.Join(s.root, RolloutDir, slug+".md"), []byte(content), 0o600)
}

// ReadRolloutSummary reads a per-session rollout summary.
func (s *Store) ReadRolloutSummary(slug string) (string, error) {
	data, err := os.ReadFile(filepath.Join(s.root, RolloutDir, slug+".md"))
	if err != nil {
		return "", err
	}
	return string(data), nil
}

// ListRolloutSummaries lists all rollout summary slugs.
func (s *Store) ListRolloutSummaries() ([]string, error) {
	entries, err := os.ReadDir(filepath.Join(s.root, RolloutDir))
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var slugs []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".md") {
			slugs = append(slugs, strings.TrimSuffix(e.Name(), ".md"))
		}
	}
	return slugs, nil
}

// AddNote writes an ad-hoc note to the extensions/ad_hoc/notes/ directory.
// The filename must be a timestamped slug: YYYY-MM-DDTHH-MM-SS-slug.md
func (s *Store) AddNote(filename, content string) error {
	path := filepath.Join(s.root, NotesDir, filename)
	if !isWithinRoot(path, filepath.Join(s.root, NotesDir)) {
		return fmt.Errorf("note path escapes notes directory")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(content), 0o600)
}

// ListNotes lists all ad-hoc notes.
func (s *Store) ListNotes() ([]string, error) {
	dir := filepath.Join(s.root, NotesDir)
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".md") {
			names = append(names, e.Name())
		}
	}
	return names, nil
}

// ReadNote reads an ad-hoc note by filename.
func (s *Store) ReadNote(filename string) (string, error) {
	path := filepath.Join(s.root, NotesDir, filename)
	if !isWithinRoot(path, filepath.Join(s.root, NotesDir)) {
		return "", fmt.Errorf("note path escapes notes directory")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	return string(data), nil
}

// InitGit initializes the memory root as a git repository for
// incremental diff detection. Idempotent — does nothing if .git exists.
func (s *Store) InitGit(ctx context.Context) error {
	gitDir := filepath.Join(s.root, ".git")
	if _, err := os.Stat(gitDir); err == nil {
		return nil // already initialized
	}
	cmd := exec.CommandContext(ctx, "git", "init")
	cmd.Dir = s.root
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("git init: %s: %w", out, err)
	}
	// Create initial commit with current state.
	if err := s.gitCommitAll(ctx, "memory: initial baseline"); err != nil {
		return fmt.Errorf("git initial commit: %w", err)
	}
	return nil
}

// GitDiff returns the diff since the last commit, or empty string if
// no changes. Returns empty string if git is not initialized.
func (s *Store) GitDiff(ctx context.Context) (string, error) {
	gitDir := filepath.Join(s.root, ".git")
	if _, err := os.Stat(gitDir); os.IsNotExist(err) {
		return "", nil
	}
	// Stage all changes for diff.
	addCmd := exec.CommandContext(ctx, "git", "add", "-A")
	addCmd.Dir = s.root
	if out, err := addCmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("git add: %s: %w", out, err)
	}
	diffCmd := exec.CommandContext(ctx, "git", "diff", "--cached")
	diffCmd.Dir = s.root
	out, err := diffCmd.Output()
	if err != nil {
		return "", fmt.Errorf("git diff: %w", err)
	}
	return string(out), nil
}

// GitCommitAll commits all pending changes with the given message.
func (s *Store) GitCommitAll(ctx context.Context, message string) error {
	return s.gitCommitAll(ctx, message)
}

// gitUserFlags returns -c flags that set a local identity for the memory
// repo so that git commit works even without a global user.name/email.
func gitUserFlags() []string {
	return []string{"-c", "user.name=loom", "-c", "user.email=loom@localhost"}
}

func (s *Store) gitCommitAll(ctx context.Context, message string) error {
	addCmd := exec.CommandContext(ctx, "git", "add", "-A")
	addCmd.Dir = s.root
	if out, err := addCmd.CombinedOutput(); err != nil {
		return fmt.Errorf("git add: %s: %w", out, err)
	}
	args := append(gitUserFlags(), "commit", "--allow-empty", "-m", message)
	commitCmd := exec.CommandContext(ctx, "git", args...)
	commitCmd.Dir = s.root
	if out, err := commitCmd.CombinedOutput(); err != nil {
		return fmt.Errorf("git commit: %s: %w", out, err)
	}
	return nil
}

// Cleanup removes raw memories older than maxAge. Returns the number of
// files removed.
func (s *Store) Cleanup(maxAge time.Duration) (int, error) {
	cutoff := time.Now().Add(-maxAge)
	removed := 0

	// Clean old rollout summaries.
	entries, err := os.ReadDir(filepath.Join(s.root, RolloutDir))
	if err != nil && !os.IsNotExist(err) {
		return 0, err
	}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		if info.ModTime().Before(cutoff) {
			if err := os.Remove(filepath.Join(s.root, RolloutDir, e.Name())); err == nil {
				removed++
			}
		}
	}
	return removed, nil
}

// FileEntry represents a file or directory in the memory store.
type FileEntry struct {
	Path  string `json:"path"`
	IsDir bool   `json:"is_dir"`
}

// isWithinRoot checks that absPath stays within the memory store root,
// guarding against sibling-directory attacks (e.g. root="/a/memories",
// relPath="../memories-evil" → absPath="/a/memories-evil" has the same
// string prefix but is outside the tree).
func isWithinRoot(absPath, root string) bool {
	if absPath == root {
		return true
	}
	return strings.HasPrefix(absPath, root+string(os.PathSeparator))
}

// List lists files and directories under a relative path in the memory store.
func (s *Store) List(relPath string, maxResults int) ([]FileEntry, error) {
	absPath := filepath.Join(s.root, relPath)
	if !isWithinRoot(absPath, s.root) {
		return nil, fmt.Errorf("path escapes memory root")
	}
	entries, err := os.ReadDir(absPath)
	if err != nil {
		return nil, err
	}
	var out []FileEntry
	for _, e := range entries {
		name := e.Name()
		// Skip .git directory.
		if name == ".git" {
			continue
		}
		rel := filepath.Join(relPath, name)
		out = append(out, FileEntry{Path: rel, IsDir: e.IsDir()})
		if len(out) >= maxResults {
			break
		}
	}
	return out, nil
}

// ReadFile reads a file from the memory store by relative path, with
// optional line offset and limit. Returns the content and total line count.
// Path components starting with ".git" are rejected to prevent reading
// git internals through this API.
func (s *Store) ReadFile(relPath string, offset, limit int) (string, int, error) {
	absPath := filepath.Join(s.root, relPath)
	if !isWithinRoot(absPath, s.root) {
		return "", 0, fmt.Errorf("path escapes memory root")
	}
	// Reject paths that point into .git internals.
	for _, part := range strings.Split(relPath, "/") {
		if part == ".git" {
			return "", 0, fmt.Errorf("path escapes memory root")
		}
	}
	data, err := os.ReadFile(absPath)
	if err != nil {
		return "", 0, err
	}
	lines := strings.Split(string(data), "\n")
	total := len(lines)
	if offset > 0 {
		if offset > total {
			return "", total, nil // offset beyond file — return empty, not the whole file
		}
		lines = lines[offset-1:]
	}
	if limit > 0 && limit < len(lines) {
		lines = lines[:limit]
	}
	return strings.Join(lines, "\n"), total, nil
}

// SearchMatch represents a search result.
type SearchMatch struct {
	Path    string `json:"path"`
	Line    int    `json:"line"`
	Content string `json:"content"`
}

// Search performs a substring search across all .md files in the memory
// store. Returns up to maxResults matches. If maxResults <= 0, it defaults
// to DefaultSearchMaxResults.
func (s *Store) Search(query string, maxResults int) ([]SearchMatch, error) {
	if maxResults <= 0 {
		maxResults = DefaultSearchMaxResults
	}
	if strings.TrimSpace(query) == "" {
		return nil, nil // empty query matches nothing
	}
	var matches []SearchMatch
	err := filepath.Walk(s.root, func(path string, info os.FileInfo, walkErr error) error {
		if walkErr != nil {
			return walkErr // propagate walk errors (e.g. permission denied on root)
		}
		if info.IsDir() {
			if info.Name() == ".git" {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(info.Name(), ".md") {
			return nil
		}
		if len(matches) >= maxResults {
			return errLimitReached
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return nil
		}
		relPath, _ := filepath.Rel(s.root, path)
		for i, line := range strings.Split(string(data), "\n") {
			if strings.Contains(line, query) {
				matches = append(matches, SearchMatch{
					Path:    relPath,
					Line:    i + 1,
					Content: line,
				})
				if len(matches) >= maxResults {
					return errLimitReached
				}
			}
		}
		return nil
	})
	if err != nil && !errors.Is(err, errLimitReached) {
		return matches, err
	}
	return matches, nil
}
