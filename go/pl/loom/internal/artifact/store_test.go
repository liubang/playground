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

package artifact

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestStagedWriterStreamsCommitsAndDeduplicates(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	staged, err := store.Begin(context.Background())
	if err != nil {
		t.Fatalf("Begin: %v", err)
	}
	for _, chunk := range []string{"streamed ", "artifact"} {
		if n, err := staged.Write([]byte(chunk)); err != nil || n != len(chunk) {
			t.Fatalf("Write(%q) = %d, %v", chunk, n, err)
		}
	}
	if staged.TotalBytes() != 17 || staged.StoredBytes() != 17 || staged.Truncated() {
		t.Fatalf("unexpected staged counters: total=%d stored=%d truncated=%v", staged.TotalBytes(), staged.StoredBytes(), staged.Truncated())
	}
	ref, err := staged.Commit(context.Background())
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	data, err := store.ReadAll(context.Background(), ref)
	if err != nil || string(data) != "streamed artifact" {
		t.Fatalf("ReadAll = %q, %v", data, err)
	}
	if err := staged.Abort(); err != nil {
		t.Fatalf("Abort after commit: %v", err)
	}
	duplicate, err := store.PutBytes(context.Background(), data)
	if err != nil || duplicate != ref {
		t.Fatalf("deduplicate = %+v, %v; want %+v", duplicate, err, ref)
	}
}

func TestStagedWriterTruncatesStorageButConsumesInput(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 5)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	staged, err := store.Begin(context.Background())
	if err != nil {
		t.Fatalf("Begin: %v", err)
	}
	if n, err := staged.Write([]byte("123456789")); err != nil || n != 9 {
		t.Fatalf("Write = %d, %v", n, err)
	}
	if staged.TotalBytes() != 9 || staged.StoredBytes() != 5 || !staged.Truncated() {
		t.Fatalf("unexpected counters: total=%d stored=%d truncated=%v", staged.TotalBytes(), staged.StoredBytes(), staged.Truncated())
	}
	ref, err := staged.Commit(context.Background())
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	data, err := store.ReadAll(context.Background(), ref)
	if err != nil || string(data) != "12345" {
		t.Fatalf("ReadAll = %q, %v", data, err)
	}
}

func TestStagedWriterAbortRemovesTemporaryFile(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	staged, err := store.Begin(context.Background())
	if err != nil {
		t.Fatalf("Begin: %v", err)
	}
	if _, err := staged.Write([]byte("discard")); err != nil {
		t.Fatalf("Write: %v", err)
	}
	if err := staged.Abort(); err != nil {
		t.Fatalf("Abort: %v", err)
	}
	if err := staged.Abort(); err != nil {
		t.Fatalf("second Abort: %v", err)
	}
	if count := countRegularFiles(t, store.Root()); count != 0 {
		t.Fatalf("regular files after abort = %d", count)
	}
}

func TestStorePutReadAndDeduplicate(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	first, err := store.PutBytes(context.Background(), []byte("durable output"))
	if err != nil {
		t.Fatalf("PutBytes first: %v", err)
	}
	second, err := store.Put(context.Background(), strings.NewReader("durable output"))
	if err != nil {
		t.Fatalf("Put second: %v", err)
	}
	if first != second {
		t.Fatalf("references differ: first=%+v second=%+v", first, second)
	}
	if !strings.HasPrefix(first.ID.String(), "art_sha256_") || first.Size != int64(len("durable output")) {
		t.Fatalf("unexpected reference: %+v", first)
	}
	data, err := store.ReadAll(context.Background(), first)
	if err != nil {
		t.Fatalf("ReadAll: %v", err)
	}
	if string(data) != "durable output" {
		t.Fatalf("data = %q", data)
	}
	if count := countRegularFiles(t, store.Root()); count != 1 {
		t.Fatalf("regular artifact files = %d, want 1", count)
	}
}

func TestStoreRejectsOversizedArtifactWithoutResidue(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 4)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	_, err = store.PutBytes(context.Background(), []byte("12345"))
	if errorCode(err) != domain.ErrBudget {
		t.Fatalf("PutBytes error = %v, want budget", err)
	}
	if count := countRegularFiles(t, store.Root()); count != 0 {
		t.Fatalf("regular artifact files = %d, want 0", count)
	}
}

func TestStoreCancelledWriteLeavesNoResidue(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err = store.Put(ctx, strings.NewReader("content"))
	if errorCode(err) != domain.ErrCancelled {
		t.Fatalf("Put error = %v, want cancelled", err)
	}
	if count := countRegularFiles(t, store.Root()); count != 0 {
		t.Fatalf("regular artifact files = %d, want 0", count)
	}
}

func TestStoreDetectsTampering(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	ref, err := store.PutBytes(context.Background(), []byte("original"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	digest := strings.TrimPrefix(ref.ID.String(), idPrefix)
	path := store.pathForDigest(digest)
	if err := os.WriteFile(path, []byte("modified"), 0o600); err != nil {
		t.Fatalf("tamper: %v", err)
	}
	_, err = store.ReadAll(context.Background(), ref)
	if errorCode(err) != domain.ErrConflict {
		t.Fatalf("ReadAll error = %v, want conflict", err)
	}
	_, err = store.PutBytes(context.Background(), []byte("original"))
	if errorCode(err) != domain.ErrConflict {
		t.Fatalf("PutBytes error = %v, want conflict", err)
	}
}

func TestStoreCollectGarbageRetainsReferencesAndYoungOrphans(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	ctx := context.Background()
	referenced, err := store.PutBytes(ctx, []byte("referenced"))
	if err != nil {
		t.Fatalf("PutBytes referenced: %v", err)
	}
	oldOrphan, err := store.PutBytes(ctx, []byte("old orphan"))
	if err != nil {
		t.Fatalf("PutBytes orphan: %v", err)
	}
	youngOrphan, err := store.PutBytes(ctx, []byte("young orphan"))
	if err != nil {
		t.Fatalf("PutBytes young orphan: %v", err)
	}
	now := time.Now()
	oldTime := now.Add(-2 * time.Hour)
	for _, ref := range []domain.ArtifactRef{referenced, oldOrphan} {
		digest := strings.TrimPrefix(ref.ID.String(), idPrefix)
		if err := os.Chtimes(store.pathForDigest(digest), oldTime, oldTime); err != nil {
			t.Fatalf("Chtimes: %v", err)
		}
	}
	report, err := store.CollectGarbage(ctx, map[domain.ArtifactID]int64{referenced.ID: referenced.Size}, time.Hour, now)
	if err != nil {
		t.Fatalf("CollectGarbage: %v", err)
	}
	if report.Scanned != 3 || report.Retained != 1 || report.GraceRetained != 1 || report.Deleted != 1 || report.DeletedBytes != oldOrphan.Size {
		t.Fatalf("unexpected report: %+v", report)
	}
	if _, err := store.ReadAll(ctx, referenced); err != nil {
		t.Fatalf("referenced artifact removed: %v", err)
	}
	if _, err := store.ReadAll(ctx, youngOrphan); err != nil {
		t.Fatalf("young orphan removed: %v", err)
	}
	if _, err := store.ReadAll(ctx, oldOrphan); errorCode(err) != domain.ErrUnavailable {
		t.Fatalf("old orphan read error = %v, want unavailable", err)
	}
}

func TestStoreCollectGarbageCleansOldStagingAndHonorsCancellation(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	staging := filepath.Join(store.blobRoot, ".artifact-crashed")
	if err := os.WriteFile(staging, []byte("partial"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	oldTime := time.Now().Add(-2 * time.Hour)
	if err := os.Chtimes(staging, oldTime, oldTime); err != nil {
		t.Fatalf("Chtimes: %v", err)
	}
	if _, err := store.CollectGarbage(context.Background(), nil, time.Hour, time.Now()); err != nil {
		t.Fatalf("CollectGarbage: %v", err)
	}
	if _, err := os.Stat(staging); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("staging residue stat = %v", err)
	}
	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := store.CollectGarbage(cancelled, nil, time.Hour, time.Now()); errorCode(err) != domain.ErrCancelled {
		t.Fatalf("cancelled GC error = %v, want cancelled", err)
	}
}

func TestStoreRejectsInvalidReference(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	badID, _ := domain.ParseArtifactID("art_elsewhere")
	_, err = store.OpenArtifact(context.Background(), domain.ArtifactRef{ID: badID, Size: 1})
	if errorCode(err) != domain.ErrInvalidInput {
		t.Fatalf("OpenArtifact error = %v, want invalid_input", err)
	}
}

func TestOpenRejectsSymlinkRoot(t *testing.T) {
	base := t.TempDir()
	realRoot := filepath.Join(base, "real")
	if err := os.Mkdir(realRoot, 0o700); err != nil {
		t.Fatalf("Mkdir: %v", err)
	}
	link := filepath.Join(base, "link")
	if err := os.Symlink(realRoot, link); err != nil {
		t.Fatalf("Symlink: %v", err)
	}
	_, err := Open(link, 1024)
	if errorCode(err) != domain.ErrSecurity {
		t.Fatalf("Open error = %v, want security", err)
	}
}

func TestStoreOpenArtifactReturnsReadableStream(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "artifacts"), 1024)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	ref, err := store.PutBytes(context.Background(), []byte("stream"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	reader, err := store.OpenArtifact(context.Background(), ref)
	if err != nil {
		t.Fatalf("OpenArtifact: %v", err)
	}
	defer reader.Close()
	data, err := io.ReadAll(reader)
	if err != nil {
		t.Fatalf("ReadAll stream: %v", err)
	}
	if string(data) != "stream" {
		t.Fatalf("stream data = %q", data)
	}
}

func countRegularFiles(t *testing.T, root string) int {
	t.Helper()
	count := 0
	err := filepath.WalkDir(root, func(_ string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.Type().IsRegular() {
			count++
		}
		return nil
	})
	if err != nil {
		t.Fatalf("WalkDir: %v", err)
	}
	return count
}

func errorCode(err error) domain.ErrorCode {
	var agentErr *domain.AgentError
	if errors.As(err, &agentErr) {
		return agentErr.Code
	}
	return ""
}
