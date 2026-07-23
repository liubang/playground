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

package workspace

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	// EmptyFileSHA256 is the canonical SHA256 hash for an empty file.
	EmptyFileSHA256                = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	defaultNewFileMode fs.FileMode = 0o644
)

var afterTempFileCreatedHook func(string)

// PathValidator ensures file paths stay within the workspace root.
type PathValidator struct {
	root string
}

// ResolvedPath is a lexical workspace path normalized for secure I/O.
type ResolvedPath struct {
	Absolute string
	Relative string
	Display  string
}

// Snapshot describes a point-in-time regular-file snapshot.
type Snapshot struct {
	Path   string
	SHA256 string
	Size   int64
	Mode   fs.FileMode
	MTime  time.Time
}

// AtomicWriteOptions configures AtomicWrite.
type AtomicWriteOptions struct {
	ExpectedHash string
	NewFileMode  fs.FileMode
	SyncParent   bool
}

// NewPathValidator creates a validator for the given workspace root.
// The root is resolved to an absolute, symlink-free path.
func NewPathValidator(root string) (*PathValidator, error) {
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, fmt.Errorf("abs path: %w", err)
	}
	resolved, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return nil, fmt.Errorf("eval symlinks: %w", err)
	}
	return &PathValidator{root: resolved}, nil
}

// Root returns the workspace root.
func (v *PathValidator) Root() string { return v.root }

// Validate checks that the given path is within the workspace root.
// It cleans the path, resolves symlinks, and ensures the result is under root.
func (v *PathValidator) Validate(path string) (string, error) {
	// Reject null bytes
	if strings.ContainsRune(path, 0) {
		return "", fmt.Errorf("null byte in path")
	}

	// Clean the path
	cleaned := filepath.Clean(path)

	// Join with root if not absolute
	var full string
	if filepath.IsAbs(cleaned) {
		full = cleaned
	} else {
		full = filepath.Join(v.root, cleaned)
	}

	// Resolve symlinks (if the file exists)
	resolved, err := filepath.EvalSymlinks(full)
	if err != nil {
		if !os.IsNotExist(err) {
			return "", fmt.Errorf("eval symlinks: %w", err)
		}
		// File doesn't exist yet — walk up to find the deepest existing ancestor
		resolved, err = resolveNonExistent(full)
		if err != nil {
			return "", fmt.Errorf("resolve non-existent path: %w", err)
		}
	}

	// Ensure resolved path is under root
	if !isUnderRoot(resolved, v.root) {
		return "", fmt.Errorf("path %q escapes workspace root %q", path, v.root)
	}

	return resolved, nil
}

// ResolveLexical normalizes a path under the workspace without following symlinks.
func (v *PathValidator) ResolveLexical(path string) (ResolvedPath, error) {
	if strings.TrimSpace(path) == "" {
		return ResolvedPath{}, fmt.Errorf("path is required")
	}
	if strings.ContainsRune(path, 0) {
		return ResolvedPath{}, fmt.Errorf("null byte in path")
	}

	cleaned := filepath.Clean(path)
	var full string
	if filepath.IsAbs(cleaned) {
		full = cleaned
	} else {
		full = filepath.Join(v.root, cleaned)
	}
	full = filepath.Clean(full)
	if !isUnderRoot(full, v.root) {
		return ResolvedPath{}, fmt.Errorf("path %q escapes workspace root %q", path, v.root)
	}

	rel, err := filepath.Rel(v.root, full)
	if err != nil {
		return ResolvedPath{}, fmt.Errorf("rel path: %w", err)
	}
	if containsSensitiveComponent(rel) {
		return ResolvedPath{}, fmt.Errorf("path %q contains a sensitive component", path)
	}

	return ResolvedPath{
		Absolute: full,
		Relative: rel,
		Display:  displayPath(rel),
	}, nil
}

// Snapshot returns a secure regular-file snapshot for a workspace path.
func (v *PathValidator) Snapshot(path string) (Snapshot, error) {
	resolved, err := v.ResolveLexical(path)
	if err != nil {
		return Snapshot{}, err
	}
	info, err := ensurePathChain(resolved.Absolute, true)
	if err != nil {
		return Snapshot{}, err
	}
	if !info.Mode().IsRegular() {
		return Snapshot{}, fmt.Errorf("path %q is not a regular file", resolved.Display)
	}
	return snapshotByOpen(resolved)
}

// SHA256 returns the current hash of a secure regular workspace file.
func (v *PathValidator) SHA256(path string) (string, error) {
	snapshot, err := v.Snapshot(path)
	if err != nil {
		return "", err
	}
	return snapshot.SHA256, nil
}

// AtomicWrite replaces a file atomically after validating the expected hash.
func (v *PathValidator) AtomicWrite(path string, data []byte, opts AtomicWriteOptions) (result Snapshot, err error) {
	resolved, err := v.ResolveLexical(path)
	if err != nil {
		return Snapshot{}, err
	}
	if opts.ExpectedHash == "" {
		return Snapshot{}, fmt.Errorf("expected hash is required")
	}
	if !isHexSHA256(opts.ExpectedHash) {
		return Snapshot{}, fmt.Errorf("expected hash must be a lowercase SHA256 hex string")
	}

	parent := filepath.Dir(resolved.Absolute)
	parentInfo, err := ensurePathChain(parent, true)
	if err != nil {
		return Snapshot{}, err
	}
	if !parentInfo.IsDir() {
		return Snapshot{}, fmt.Errorf("parent path %q is not a directory", filepath.Dir(resolved.Display))
	}

	mode, err := currentModeForWrite(resolved, opts)
	if err != nil {
		return Snapshot{}, err
	}
	metadata, err := loadAtomicWriteMetadata(resolved)
	if err != nil {
		return Snapshot{}, err
	}

	tempPath, tempFile, err := createTempSibling(parent, filepath.Base(resolved.Absolute), mode)
	if err != nil {
		return Snapshot{}, err
	}
	defer func() {
		if tempFile != nil {
			_ = tempFile.Close()
		}
		if tempPath != "" {
			_ = os.Remove(tempPath)
		}
	}()

	if err := recheckExpectedHash(resolved, opts.ExpectedHash); err != nil {
		return Snapshot{}, err
	}
	if err := tempFile.Chmod(mode); err != nil {
		return Snapshot{}, fmt.Errorf("chmod temp file: %w", err)
	}
	if _, err := tempFile.Write(data); err != nil {
		return Snapshot{}, fmt.Errorf("write temp file: %w", err)
	}
	if err := tempFile.Sync(); err != nil {
		return Snapshot{}, fmt.Errorf("sync temp file: %w", err)
	}
	if err := tempFile.Close(); err != nil {
		return Snapshot{}, fmt.Errorf("close temp file: %w", err)
	}
	tempFile = nil
	if err := applyAtomicWriteMetadata(tempPath, metadata); err != nil {
		return Snapshot{}, err
	}

	if err := os.Rename(tempPath, resolved.Absolute); err != nil {
		return Snapshot{}, fmt.Errorf("rename temp file: %w", err)
	}
	tempPath = ""

	if opts.SyncParent {
		if err := syncDirectory(parent); err != nil {
			return Snapshot{}, err
		}
	}

	return v.Snapshot(resolved.Absolute)
}

// isUnderRoot checks that path is under root, handling trailing separators.
func isUnderRoot(path, root string) bool {
	// Ensure both have trailing separator for prefix comparison
	normalized := filepath.Clean(path)
	rootNorm := filepath.Clean(root)

	if normalized == rootNorm {
		return true
	}

	return strings.HasPrefix(normalized, rootNorm+string(filepath.Separator))
}

// resolveNonExistent resolves symlinks for a non-existent path by walking
// up to the deepest existing ancestor, then appending the remaining components.
func resolveNonExistent(path string) (string, error) {
	parent := filepath.Dir(path)
	base := filepath.Base(path)

	// Base case: reached filesystem root
	if parent == path {
		return path, nil
	}

	// Try resolving the parent
	parentResolved, err := filepath.EvalSymlinks(parent)
	if err == nil {
		return filepath.Join(parentResolved, base), nil
	}
	if !os.IsNotExist(err) {
		return "", fmt.Errorf("eval symlinks for %q: %w", parent, err)
	}

	// Parent doesn't exist either — recurse
	resolved, err := resolveNonExistent(parent)
	if err != nil {
		return "", err
	}
	return filepath.Join(resolved, base), nil
}

// IsSensitive reports whether a path should be denied by default.
func IsSensitive(path string) bool {
	base := filepath.Base(path)
	sensitive := []string{
		".git",
		".ssh",
		".gnupg",
		".env",
		".credentials",
		"credentials.json",
		"service-account.json",
	}
	for _, s := range sensitive {
		if base == s {
			return true
		}
	}
	return false
}

func containsSensitiveComponent(path string) bool {
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

func displayPath(rel string) string {
	clean := filepath.Clean(rel)
	if clean == "." || clean == string(filepath.Separator) {
		return "."
	}
	return filepath.ToSlash(clean)
}

func ensurePathChain(path string, requireLeaf bool) (fs.FileInfo, error) {
	clean := filepath.Clean(path)
	if !filepath.IsAbs(clean) {
		abs, err := filepath.Abs(clean)
		if err != nil {
			return nil, fmt.Errorf("abs path: %w", err)
		}
		clean = abs
	}

	parts := make([]string, 0, 8)
	for current := clean; ; current = filepath.Dir(current) {
		parts = append(parts, current)
		parent := filepath.Dir(current)
		if parent == current {
			break
		}
	}
	for i, j := 0, len(parts)-1; i < j; i, j = i+1, j-1 {
		parts[i], parts[j] = parts[j], parts[i]
	}

	for idx, current := range parts {
		info, err := os.Lstat(current)
		if err != nil {
			if os.IsNotExist(err) && idx == len(parts)-1 && !requireLeaf {
				return nil, nil
			}
			return nil, fmt.Errorf("lstat %q: %w", current, err)
		}
		if info.Mode()&os.ModeSymlink != 0 {
			return nil, fmt.Errorf("path %q contains a symlink", current)
		}
		if idx < len(parts)-1 && !info.IsDir() {
			return nil, fmt.Errorf("path component %q is not a directory", current)
		}
		if idx == len(parts)-1 {
			return info, nil
		}
	}
	return nil, fmt.Errorf("path %q could not be validated", clean)
}

func snapshotByOpen(path ResolvedPath) (Snapshot, error) {
	lstatInfo, err := os.Lstat(path.Absolute)
	if err != nil {
		return Snapshot{}, fmt.Errorf("lstat file: %w", err)
	}
	if lstatInfo.Mode()&os.ModeSymlink != 0 {
		return Snapshot{}, fmt.Errorf("path %q is a symlink", path.Display)
	}

	file, err := os.Open(path.Absolute)
	if err != nil {
		return Snapshot{}, fmt.Errorf("open file: %w", err)
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return Snapshot{}, fmt.Errorf("stat file: %w", err)
	}
	if !info.Mode().IsRegular() {
		return Snapshot{}, fmt.Errorf("path %q is not a regular file", path.Display)
	}
	if !os.SameFile(lstatInfo, info) {
		return Snapshot{}, fmt.Errorf("path %q changed during snapshot", path.Display)
	}

	hasher := sha256.New()
	size, err := io.Copy(hasher, file)
	if err != nil {
		return Snapshot{}, fmt.Errorf("read file: %w", err)
	}
	return Snapshot{
		Path:   path.Display,
		SHA256: hex.EncodeToString(hasher.Sum(nil)),
		Size:   size,
		Mode:   info.Mode(),
		MTime:  info.ModTime(),
	}, nil
}

func currentModeForWrite(path ResolvedPath, opts AtomicWriteOptions) (fs.FileMode, error) {
	info, err := ensurePathChain(path.Absolute, false)
	if err != nil {
		return 0, err
	}
	if info == nil {
		if opts.ExpectedHash != EmptyFileSHA256 {
			return 0, fmt.Errorf("new file writes must use the empty-file expected hash")
		}
		mode := opts.NewFileMode.Perm()
		if mode == 0 {
			mode = defaultNewFileMode
		}
		return mode, nil
	}
	if !info.Mode().IsRegular() {
		return 0, fmt.Errorf("path %q is not a regular file", path.Display)
	}
	return info.Mode().Perm(), nil
}

func recheckExpectedHash(path ResolvedPath, expectedHash string) error {
	snapshot, err := snapshotIfExists(path)
	if err != nil {
		return err
	}
	if snapshot == nil {
		if expectedHash != EmptyFileSHA256 {
			return fmt.Errorf("expected hash mismatch for %q: got %q, want %q", path.Display, EmptyFileSHA256, expectedHash)
		}
		return nil
	}
	if snapshot.SHA256 != expectedHash {
		return fmt.Errorf("expected hash mismatch for %q: got %q, want %q", path.Display, snapshot.SHA256, expectedHash)
	}
	return nil
}

func snapshotIfExists(path ResolvedPath) (*Snapshot, error) {
	info, err := ensurePathChain(path.Absolute, false)
	if err != nil {
		return nil, err
	}
	if info == nil {
		return nil, nil
	}
	snapshot, err := snapshotByOpen(path)
	if err != nil {
		return nil, err
	}
	return &snapshot, nil
}

func createTempSibling(dir, base string, mode fs.FileMode) (string, *os.File, error) {
	for range 32 {
		suffix, err := randomSuffix()
		if err != nil {
			return "", nil, fmt.Errorf("random temp suffix: %w", err)
		}
		tempPath := filepath.Join(dir, fmt.Sprintf(".%s.%s.tmp", base, suffix))
		file, err := os.OpenFile(tempPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
		if err == nil {
			if afterTempFileCreatedHook != nil {
				afterTempFileCreatedHook(tempPath)
			}
			return tempPath, file, nil
		}
		if errors.Is(err, os.ErrExist) {
			continue
		}
		return "", nil, fmt.Errorf("create temp file: %w", err)
	}
	return "", nil, fmt.Errorf("create temp file: too many name collisions")
}

func randomSuffix() (string, error) {
	var buf [8]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf[:]), nil
}

func syncDirectory(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("open parent directory: %w", err)
	}
	defer dir.Close()
	if err := dir.Sync(); err != nil {
		return fmt.Errorf("sync parent directory: %w", err)
	}
	return nil
}

type atomicWriteMetadata struct {
	hasExistingFile bool
	xattrs          []xattrEntry
}

type xattrEntry struct {
	name  string
	value []byte
}

func loadAtomicWriteMetadata(path ResolvedPath) (atomicWriteMetadata, error) {
	snapshot, err := snapshotIfExists(path)
	if err != nil {
		return atomicWriteMetadata{}, err
	}
	if snapshot == nil {
		return atomicWriteMetadata{}, nil
	}
	entries, err := readFileXAttrs(path.Absolute)
	if err != nil {
		return atomicWriteMetadata{}, fmt.Errorf("preserve metadata for %q: %w", path.Display, err)
	}
	if err := ensureAtomicWriteACLPolicy(path.Absolute); err != nil {
		return atomicWriteMetadata{}, fmt.Errorf("preserve metadata for %q: %w", path.Display, err)
	}
	return atomicWriteMetadata{hasExistingFile: true, xattrs: entries}, nil
}

func applyAtomicWriteMetadata(path string, metadata atomicWriteMetadata) error {
	if !metadata.hasExistingFile {
		return nil
	}
	for _, entry := range metadata.xattrs {
		if err := writeFileXAttr(path, entry.name, entry.value); err != nil {
			return fmt.Errorf("apply preserved metadata: %w", err)
		}
	}
	return nil
}

func isHexSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, r := range value {
		if (r < '0' || r > '9') && (r < 'a' || r > 'f') {
			return false
		}
	}
	return true
}
