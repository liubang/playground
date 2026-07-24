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

// Package artifact provides a bounded, content-addressed store for large
// outputs that should not be embedded in the session event log.
package artifact

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	idPrefix      = "art_sha256_"
	copyBufferLen = 32 * 1024
)

// Store persists immutable blobs below a private root directory.
type Store struct {
	root     string
	blobRoot string
	maxBytes int64
}

// Open initializes a content-addressed artifact store. maxBytes is a hard
// per-artifact limit and must be positive.
func Open(root string, maxBytes int64) (*Store, error) {
	root = strings.TrimSpace(root)
	if root == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "artifact root is required")
	}
	if maxBytes <= 0 {
		return nil, domain.NewError(domain.ErrInvalidInput, "artifact size limit must be positive")
	}
	absolute, err := filepath.Abs(root)
	if err != nil {
		return nil, artifactError("resolve artifact root", err)
	}
	if err := ensurePrivateDirectory(absolute); err != nil {
		return nil, err
	}
	blobRoot := filepath.Join(absolute, "sha256")
	if err := ensurePrivateDirectory(blobRoot); err != nil {
		return nil, err
	}
	return &Store{root: absolute, blobRoot: blobRoot, maxBytes: maxBytes}, nil
}

// Root returns the canonical store root.
func (s *Store) Root() string { return s.root }

// StagedWriter incrementally captures one artifact. It always consumes the
// complete input stream; bytes beyond the store limit are counted but not
// persisted so process pipes can continue to drain safely.
type StagedWriter struct {
	store     *Store
	file      *os.File
	path      string
	hash      hashWriter
	mu        sync.Mutex
	stored    int64
	total     int64
	truncated bool
	closed    bool
}

type hashWriter interface {
	io.Writer
	Sum([]byte) []byte
}

// Begin creates a private staging file for incremental artifact capture.
func (s *Store) Begin(ctx context.Context) (domain.StagedArtifact, error) {
	if s == nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "artifact store is required")
	}
	if ctx != nil {
		select {
		case <-ctx.Done():
			return nil, domain.NewError(domain.ErrCancelled, "artifact staging cancelled", domain.WithCause(ctx.Err()))
		default:
		}
	}
	file, err := os.CreateTemp(s.blobRoot, ".artifact-*")
	if err != nil {
		return nil, artifactError("create artifact staging file", err)
	}
	if err := file.Chmod(0o600); err != nil {
		_ = file.Close()
		_ = os.Remove(file.Name())
		return nil, artifactError("protect artifact staging file", err)
	}
	return &StagedWriter{store: s, file: file, path: file.Name(), hash: sha256.New()}, nil
}

// Write captures as much as allowed and reports the entire input consumed.
func (w *StagedWriter) Write(data []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return 0, domain.NewError(domain.ErrConflict, "artifact staging writer is closed")
	}
	w.total += int64(len(data))
	remaining := w.store.maxBytes - w.stored
	if remaining <= 0 {
		w.truncated = w.truncated || len(data) > 0
		return len(data), nil
	}
	captured := data
	if int64(len(captured)) > remaining {
		captured = captured[:remaining]
		w.truncated = true
	}
	if len(captured) > 0 {
		if _, err := w.file.Write(captured); err != nil {
			return 0, artifactError("write artifact staging file", err)
		}
		if _, err := w.hash.Write(captured); err != nil {
			return 0, artifactError("hash artifact staging data", err)
		}
		w.stored += int64(len(captured))
	}
	return len(data), nil
}

// TotalBytes returns all bytes observed, including bytes beyond the store limit.
func (w *StagedWriter) TotalBytes() int64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.total
}

// StoredBytes returns bytes retained in the staging file.
func (w *StagedWriter) StoredBytes() int64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.stored
}

// Truncated reports whether the input exceeded the artifact size limit.
func (w *StagedWriter) Truncated() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.truncated
}

// Commit syncs and atomically publishes the captured prefix.
func (w *StagedWriter) Commit(ctx context.Context) (domain.ArtifactRef, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return domain.ArtifactRef{}, domain.NewError(domain.ErrConflict, "artifact staging writer is closed")
	}
	if ctx != nil {
		select {
		case <-ctx.Done():
			return domain.ArtifactRef{}, domain.NewError(domain.ErrCancelled, "artifact commit cancelled", domain.WithCause(ctx.Err()))
		default:
		}
	}
	if err := w.file.Sync(); err != nil {
		return domain.ArtifactRef{}, artifactError("sync staged artifact", err)
	}
	if err := w.file.Close(); err != nil {
		return domain.ArtifactRef{}, artifactError("close staged artifact", err)
	}
	w.closed = true
	ref, err := w.store.commitStaged(w.path, hex.EncodeToString(w.hash.Sum(nil)), w.stored)
	if err != nil {
		_ = os.Remove(w.path)
		return domain.ArtifactRef{}, err
	}
	return ref, nil
}

// Abort closes and removes an uncommitted staging file. It is idempotent.
func (w *StagedWriter) Abort() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return nil
	}
	w.closed = true
	closeErr := w.file.Close()
	removeErr := os.Remove(w.path)
	if removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
		return artifactError("remove artifact staging file", errors.Join(closeErr, removeErr))
	}
	if closeErr != nil {
		return artifactError("close artifact staging file", closeErr)
	}
	return nil
}

// Put stores an immutable blob and returns its content-derived reference.
// Repeated writes of identical content return the same reference.
func (s *Store) Put(ctx context.Context, src io.Reader) (domain.ArtifactRef, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if src == nil {
		return domain.ArtifactRef{}, domain.NewError(domain.ErrInvalidInput, "artifact reader is required")
	}
	staged, err := s.Begin(ctx)
	if err != nil {
		return domain.ArtifactRef{}, err
	}
	defer staged.Abort()
	if _, err := copyBounded(ctx, staged, src, s.maxBytes); err != nil {
		return domain.ArtifactRef{}, err
	}
	return staged.Commit(ctx)
}

// PutBytes stores data as an immutable artifact.
func (s *Store) PutBytes(ctx context.Context, data []byte) (domain.ArtifactRef, error) {
	return s.Put(ctx, bytes.NewReader(data))
}

// OpenArtifact opens and verifies an artifact. The caller must close the result.
func (s *Store) OpenArtifact(ctx context.Context, ref domain.ArtifactRef) (io.ReadCloser, error) {
	digest, err := digestFromRef(ref)
	if err != nil {
		return nil, err
	}
	path := s.pathForDigest(digest)
	info, err := os.Lstat(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, domain.NewError(domain.ErrUnavailable, "artifact not found", domain.WithCause(err))
		}
		return nil, artifactError("inspect artifact", err)
	}
	if ref.Size != info.Size() {
		return nil, domain.NewError(domain.ErrConflict, "artifact size does not match reference")
	}
	if err := validateExisting(path, info, digest, ref.Size); err != nil {
		return nil, err
	}
	if ctx != nil {
		select {
		case <-ctx.Done():
			return nil, domain.NewError(domain.ErrCancelled, "artifact open cancelled", domain.WithCause(ctx.Err()))
		default:
		}
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, artifactError("open artifact", err)
	}
	return file, nil
}

// GCReport summarizes one garbage-collection sweep.
type GCReport struct {
	Scanned       int   `json:"scanned"`
	Retained      int   `json:"retained"`
	GraceRetained int   `json:"grace_retained"`
	Deleted       int   `json:"deleted"`
	DeletedBytes  int64 `json:"deleted_bytes"`
}

// CollectGarbage removes unreferenced immutable blobs older than gracePeriod.
// The grace period closes the window between publishing a blob and durably
// recording its reference in the session store.
func (s *Store) CollectGarbage(ctx context.Context, referenced map[domain.ArtifactID]int64, gracePeriod time.Duration, now time.Time) (GCReport, error) {
	if s == nil {
		return GCReport{}, domain.NewError(domain.ErrInvalidInput, "artifact store is required")
	}
	if gracePeriod < 0 {
		return GCReport{}, domain.NewError(domain.ErrInvalidInput, "artifact GC grace period must be non-negative")
	}
	if now.IsZero() {
		now = time.Now()
	}
	cutoff := now.Add(-gracePeriod)
	var report GCReport
	dirtyDirs := make(map[string]struct{})
	err := filepath.WalkDir(s.blobRoot, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if ctx != nil {
			select {
			case <-ctx.Done():
				return ctx.Err()
			default:
			}
		}
		if entry.IsDir() {
			return nil
		}
		if strings.HasPrefix(entry.Name(), ".artifact-") {
			info, err := entry.Info()
			if err != nil {
				return err
			}
			if info.ModTime().After(cutoff) {
				return nil
			}
			if err := os.Remove(path); err != nil {
				return err
			}
			dirtyDirs[filepath.Dir(path)] = struct{}{}
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 {
			return domain.NewError(domain.ErrSecurity, "artifact store contains a non-regular blob")
		}
		relative, err := filepath.Rel(s.blobRoot, path)
		if err != nil {
			return err
		}
		parts := strings.Split(filepath.ToSlash(relative), "/")
		if len(parts) != 2 || len(parts[0]) != 2 || len(parts[1]) != sha256.Size*2-2 {
			return domain.NewError(domain.ErrSecurity, "artifact store contains an invalid blob path")
		}
		id, err := domain.ParseArtifactID(idPrefix + parts[0] + parts[1])
		if err != nil {
			return domain.NewError(domain.ErrSecurity, "artifact store contains an invalid blob ID", domain.WithCause(err))
		}
		report.Scanned++
		if _, ok := referenced[id]; ok {
			report.Retained++
			return nil
		}
		if info.ModTime().After(cutoff) {
			report.GraceRetained++
			return nil
		}
		if err := os.Remove(path); err != nil {
			return err
		}
		report.Deleted++
		report.DeletedBytes += info.Size()
		dirtyDirs[filepath.Dir(path)] = struct{}{}
		return nil
	})
	if err != nil {
		if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return report, domain.NewError(domain.ErrCancelled, "artifact garbage collection cancelled", domain.WithCause(err))
		}
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			return report, err
		}
		return report, artifactError("collect artifact garbage", err)
	}
	for dir := range dirtyDirs {
		if err := syncDirectory(dir); err != nil {
			return report, err
		}
	}
	return report, nil
}

// ReadAll reads a verified artifact while retaining the store's hard bound.
func (s *Store) ReadAll(ctx context.Context, ref domain.ArtifactRef) ([]byte, error) {
	if ref.Size > s.maxBytes {
		return nil, domain.NewError(domain.ErrBudget, "artifact exceeds configured size limit")
	}
	reader, err := s.OpenArtifact(ctx, ref)
	if err != nil {
		return nil, err
	}
	defer reader.Close()
	data, err := io.ReadAll(io.LimitReader(reader, s.maxBytes+1))
	if err != nil {
		return nil, artifactError("read artifact", err)
	}
	if int64(len(data)) > s.maxBytes {
		return nil, domain.NewError(domain.ErrBudget, "artifact exceeds configured size limit")
	}
	return data, nil
}

func (s *Store) commitStaged(stagingPath, digest string, size int64) (domain.ArtifactRef, error) {
	id, _ := domain.ParseArtifactID(idPrefix + digest)
	ref := domain.ArtifactRef{ID: id, Size: size}
	destination := s.pathForDigest(digest)
	if err := ensurePrivateDirectory(filepath.Dir(destination)); err != nil {
		return domain.ArtifactRef{}, err
	}
	if info, err := os.Lstat(destination); err == nil {
		if err := validateExisting(destination, info, digest, size); err != nil {
			return domain.ArtifactRef{}, err
		}
		_ = os.Remove(stagingPath)
		return ref, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return domain.ArtifactRef{}, artifactError("inspect existing artifact", err)
	}
	if err := os.Link(stagingPath, destination); err != nil {
		if info, statErr := os.Lstat(destination); statErr == nil {
			if verifyErr := validateExisting(destination, info, digest, size); verifyErr == nil {
				_ = os.Remove(stagingPath)
				return ref, nil
			}
		}
		return domain.ArtifactRef{}, artifactError("commit artifact", err)
	}
	if err := os.Remove(stagingPath); err != nil {
		return domain.ArtifactRef{}, artifactError("remove committed artifact staging file", err)
	}
	if err := syncDirectory(filepath.Dir(destination)); err != nil {
		return domain.ArtifactRef{}, err
	}
	return ref, nil
}

func (s *Store) pathForDigest(digest string) string {
	return filepath.Join(s.blobRoot, digest[:2], digest[2:])
}

func digestFromRef(ref domain.ArtifactRef) (string, error) {
	if ref.ID.IsZero() || ref.Size < 0 {
		return "", domain.NewError(domain.ErrInvalidInput, "valid artifact reference is required")
	}
	value := ref.ID.String()
	if !strings.HasPrefix(value, idPrefix) {
		return "", domain.NewError(domain.ErrInvalidInput, "unsupported artifact ID")
	}
	digest := strings.TrimPrefix(value, idPrefix)
	decoded, err := hex.DecodeString(digest)
	if err != nil || len(decoded) != sha256.Size || digest != strings.ToLower(digest) {
		return "", domain.NewError(domain.ErrInvalidInput, "invalid artifact digest")
	}
	return digest, nil
}

func copyBounded(ctx context.Context, dst io.Writer, src io.Reader, maxBytes int64) (int64, error) {
	buffer := make([]byte, copyBufferLen)
	var total int64
	for {
		select {
		case <-ctx.Done():
			return total, domain.NewError(domain.ErrCancelled, "artifact write cancelled", domain.WithCause(ctx.Err()))
		default:
		}
		remaining := maxBytes - total + 1
		if remaining < int64(len(buffer)) {
			buffer = buffer[:remaining]
		}
		n, readErr := src.Read(buffer)
		if n > 0 {
			total += int64(n)
			if total > maxBytes {
				return total, domain.NewError(domain.ErrBudget, fmt.Sprintf("artifact exceeds %d byte limit", maxBytes))
			}
			if _, err := dst.Write(buffer[:n]); err != nil {
				return total, artifactError("write artifact", err)
			}
		}
		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				return total, nil
			}
			return total, artifactError("read artifact input", readErr)
		}
		if n == 0 {
			return total, domain.NewError(domain.ErrUnavailable, "artifact reader made no progress")
		}
	}
}

func ensurePrivateDirectory(path string) error {
	if err := os.MkdirAll(path, 0o700); err != nil {
		return artifactError("create artifact directory", err)
	}
	info, err := os.Lstat(path)
	if err != nil {
		return artifactError("inspect artifact directory", err)
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return domain.NewError(domain.ErrSecurity, "artifact path is not a real directory")
	}
	if info.Mode().Perm()&0o077 != 0 {
		if err := os.Chmod(path, 0o700); err != nil {
			return artifactError("protect artifact directory", err)
		}
	}
	return nil
}

func validateExisting(path string, info os.FileInfo, digest string, size int64) error {
	if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 {
		return domain.NewError(domain.ErrSecurity, "artifact blob is not a regular file")
	}
	if info.Size() != size {
		return domain.NewError(domain.ErrConflict, "artifact blob has unexpected size")
	}
	file, err := os.Open(path)
	if err != nil {
		return artifactError("open artifact for verification", err)
	}
	hash := sha256.New()
	_, copyErr := io.Copy(hash, file)
	closeErr := file.Close()
	if copyErr != nil {
		return artifactError("verify artifact", copyErr)
	}
	if closeErr != nil {
		return artifactError("close verified artifact", closeErr)
	}
	if hex.EncodeToString(hash.Sum(nil)) != digest {
		return domain.NewError(domain.ErrConflict, "artifact digest verification failed")
	}
	return nil
}

func syncDirectory(path string) error {
	dir, err := os.Open(path)
	if err != nil {
		return artifactError("open artifact directory for sync", err)
	}
	defer dir.Close()
	if err := dir.Sync(); err != nil {
		return artifactError("sync artifact directory", err)
	}
	return nil
}

func artifactError(action string, err error) error {
	return domain.NewError(domain.ErrUnavailable, action, domain.WithCause(err))
}
