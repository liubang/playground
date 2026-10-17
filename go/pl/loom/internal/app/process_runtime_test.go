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
// Created: 2026/08/23

package app

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

type memoryTestPrompt struct {
	sections prompt.Sections
	err      error
}

func (p memoryTestPrompt) Build(context.Context) (string, []domain.ContextRuleRef, error) {
	return joinTexts(p.sections.Static, p.sections.Dynamic), p.sections.Refs, p.err
}

func (p memoryTestPrompt) BuildSections(context.Context) (prompt.Sections, error) {
	return p.sections, p.err
}

// The adapter exposes only Build to exercise the legacy builder path too.
type memoryTestLegacyPrompt struct{ inner memoryTestPrompt }

func (p memoryTestLegacyPrompt) Build(ctx context.Context) (string, []domain.ContextRuleRef, error) {
	return p.inner.Build(ctx)
}

func TestMemoryPromptFailurePreservesSystemPrompt(t *testing.T) {
	ctx := t.Context()
	cfg := testResolvedConfig(nil)
	cfg.Storage.BaseDir = t.TempDir()
	cfg.Memory.Enabled = true
	if err := os.MkdirAll(cfg.Storage.SessionsDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	proc, err := NewProcessRuntime(ctx, cfg, ProcessRuntimeConfig{ArtifactDir: filepath.Join(t.TempDir(), "artifacts"), Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(proc.Close)
	b, err := NewWorkspaceBootstrap(ctx, proc, BootstrapConfig{WorkspaceRoot: t.TempDir()})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(b.Close)
	wrapper := b.CurrentPrompt().(*memoryPromptWrapper)
	inner := memoryTestPrompt{sections: prompt.Sections{Static: "base system prompt", Dynamic: "prior dynamic context", Refs: []domain.ContextRuleRef{{Source: "base", Hash: "base-hash"}}}}
	wrapper.inner = inner
	healthy, err := wrapper.BuildSections(ctx)
	if err != nil {
		t.Fatal(err)
	}
	// Closing the real SQLite repository produces a storage failure, not cancellation.
	if err := proc.Store.(*session.SQLiteStore).Close(); err != nil {
		t.Fatal(err)
	}
	for _, legacy := range []bool{false, true} {
		t.Run(fmt.Sprintf("legacy=%v", legacy), func(t *testing.T) {
			wrapper.inner = inner
			if legacy {
				wrapper.inner = memoryTestLegacyPrompt{inner: inner}
			}
			var logs bytes.Buffer
			wrapper.logger = slog.New(slog.NewTextHandler(&logs, nil))
			secs, err := wrapper.BuildSections(ctx)
			if err != nil {
				t.Fatalf("storage failure must degrade: %v", err)
			}
			if !strings.Contains(secs.Static, inner.sections.Static) || !strings.Contains(secs.Static, "You have durable memory shared across sessions, refreshed every turn.") || strings.Contains(secs.Static, "[MEMORY_UNAVAILABLE]") {
				t.Fatalf("static instructions lost or failure marker cached: %q", secs.Static)
			}
			if !strings.Contains(secs.Dynamic, "[MEMORY_UNAVAILABLE] Memory is unavailable; use memory tools to verify prior context rather than assume it does not exist.") || !strings.Contains(joinTexts(secs.Static, secs.Dynamic), inner.sections.Dynamic) {
				t.Fatalf("prior context or observable failure marker missing: %+v", secs)
			}
			if len(secs.Refs) != 2 || !slices.Equal(secs.Refs[:1], inner.sections.Refs) || secs.Refs[1].Hash == "" || secs.Refs[1].Hash == healthy.Refs[1].Hash {
				t.Fatalf("base refs or failure-specific memory audit hash lost: %+v", secs.Refs)
			}
			if !strings.Contains(logs.String(), "level=WARN") || !strings.Contains(logs.String(), "memory prompt injection failed") || !strings.Contains(logs.String(), "error=") {
				t.Fatalf("failure was not logged: %s", logs.String())
			}
			text, refs, err := wrapper.Build(ctx)
			if err != nil || !strings.Contains(text, "base system prompt") || !strings.Contains(text, "[MEMORY_UNAVAILABLE]") || !slices.Equal(refs, secs.Refs) {
				t.Fatalf("Build lost degraded system prompt: %q %+v %v", text, refs, err)
			}
			for _, deadline := range []bool{false, true} {
				cancelled, cancel := context.WithCancel(ctx)
				want := context.Canceled
				if deadline {
					cancel()
					cancelled, cancel = context.WithDeadline(ctx, time.Now().Add(-time.Second))
					want = context.DeadlineExceeded
				} else {
					cancel()
				}
				defer cancel()
				if _, _, err := wrapper.Build(cancelled); !errors.Is(err, want) {
					t.Fatalf("parent cancellation swallowed: %v", err)
				}
			}
			failure := errors.New("inner prompt failed")
			innerFailure := inner
			innerFailure.err = failure
			wrapper.inner = innerFailure
			if legacy {
				wrapper.inner = memoryTestLegacyPrompt{inner: innerFailure}
			}
			if _, _, err := wrapper.Build(ctx); !errors.Is(err, failure) {
				t.Fatalf("inner error swallowed: %v", err)
			}
		})
	}
}

type memoryPipelineTestLog struct {
	slog.Handler
	completed chan slog.Record
}

func (h *memoryPipelineTestLog) Handle(ctx context.Context, record slog.Record) error {
	if record.Message == "memory pipeline pass complete" {
		h.completed <- record.Clone()
	}
	return h.Handler.Handle(ctx, record)
}

func TestMemoryBootstrapWithOnlyConsolidationModel(t *testing.T) {
	ctx := t.Context()
	model := fakes.NewFakeModel()
	cfg := testResolvedConfig(model)
	cfg.Storage.BaseDir = t.TempDir()
	cfg.Memory.Enabled = true
	// Hand-assembled configuration deliberately leaves extraction unresolved;
	// normal config resolution validates these references before assembly.
	cfg.Default = config.ProviderModelRef{Provider: "missing", Model: "missing"}
	cfg.Memory.ConsolidationModel = new(config.ProviderModelRef{Provider: "test", Model: "test-model"})
	cfg.Memory.RunInterval = 0
	if err := os.MkdirAll(cfg.Storage.SessionsDir(), 0o700); err != nil {
		t.Fatal(err)
	}
	handler := &memoryPipelineTestLog{Handler: slog.NewTextHandler(io.Discard, nil), completed: make(chan slog.Record, 1)}
	proc, err := NewProcessRuntime(ctx, cfg, ProcessRuntimeConfig{ArtifactDir: filepath.Join(t.TempDir(), "artifacts"), Logger: slog.New(handler)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(proc.Close)
	if proc.MemoryStore == nil || proc.MemoryJobQueue == nil || proc.MemoryExtractor != nil || proc.MemoryConsolidator == nil || proc.memoryPipelineStop == nil {
		t.Fatal("consolidation-only model must start the shared pipeline without extraction")
	}
	select {
	case record := <-handler.completed:
		var phase2 bool
		record.Attrs(func(attr slog.Attr) bool {
			if attr.Key == "phase2_ran" {
				phase2 = attr.Value.Bool()
			}
			return true
		})
		if !phase2 {
			t.Fatal("startup pass did not execute consolidation")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("consolidation-only pipeline did not run its startup pass")
	}
	if calls := model.Calls(); len(calls) != 0 {
		t.Fatalf("empty memory must not call the model: %+v", calls)
	}
}

func TestMemoryBootstrapWithoutModel(t *testing.T) {
	for _, withProvider := range []bool{false, true} {
		t.Run(fmt.Sprintf("provider=%v", withProvider), func(t *testing.T) {
			ctx := t.Context()
			cfg := testResolvedConfig(nil)
			if !withProvider {
				cfg.Providers = nil
			}
			cfg.Storage.BaseDir = t.TempDir()
			cfg.Memory.Enabled = true
			// Entry points prepare the storage directories before assembly.
			if err := os.MkdirAll(cfg.Storage.SessionsDir(), 0o700); err != nil {
				t.Fatal(err)
			}
			proc, err := NewProcessRuntime(ctx, cfg, ProcessRuntimeConfig{
				ArtifactDir: filepath.Join(t.TempDir(), "artifacts"),
				Logger:      slog.New(slog.NewTextHandler(io.Discard, nil)),
			})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(proc.Close)
			if proc.MemoryStore == nil || proc.MemoryJobQueue == nil || proc.MemoryExtractor != nil || proc.MemoryConsolidator != nil || proc.memoryPipelineStop != nil {
				t.Fatal("explicit memory must remain available without starting model work")
			}
			repo := proc.Store.(*session.SQLiteStore)
			root, otherRoot := t.TempDir(), t.TempDir()
			bootstrap := func(root string) *Bootstrap {
				t.Helper()
				b, err := NewWorkspaceBootstrap(ctx, proc, BootstrapConfig{WorkspaceRoot: root})
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(b.Close)
				return b
			}
			first := bootstrap(root)
			mutate := func(b *Bootstrap, args string) {
				t.Helper()
				tool, ok := b.Registry.Lookup("memory")
				if !ok {
					t.Fatal("memory tool missing without provider")
				}
				call, err := tool.Prepare(ctx, domain.ToolCall{ID: domain.NewToolCallID(), Name: "memory", Arguments: json.RawMessage(args)})
				if err != nil {
					t.Fatal(err)
				}
				if result := tool.Execute(ctx, call); result.Status != domain.ToolStatusSuccess {
					t.Fatalf("memory Execute: %+v", result)
				}
			}
			mutate(first, `{"action":"remember","content":"workspace preference alpha","scope":"workspace"}`)
			mutate(first, `{"action":"remember","content":"global preference beta","scope":"global"}`)
			second, other := bootstrap(root), bootstrap(otherRoot)
			checkPrompt := func(b *Bootstrap, present, absent string) {
				t.Helper()
				text, _, err := b.CurrentPrompt().Build(ctx)
				if err != nil {
					t.Fatal(err)
				}
				if !strings.Contains(text, present) || (absent != "" && strings.Contains(text, absent)) {
					t.Fatalf("prompt visibility: want %q, exclude %q: %s", present, absent, text)
				}
			}
			checkPrompt(second, "workspace preference alpha", "")
			checkPrompt(other, "global preference beta", "workspace preference alpha")
			snapshot, err := repo.MemorySnapshot(ctx, root)
			if err != nil || len(snapshot.Records) != 1 {
				t.Fatalf("workspace snapshot: %+v, %v", snapshot, err)
			}
			record := snapshot.Records[0]
			tool, _ := first.Registry.Lookup("memory")
			for _, args := range []string{
				fmt.Sprintf(`{"action":"remember","id":%q,"revision":%d,"content":"replacement","scope":"workspace"}`, record.ID, record.Revision),
				fmt.Sprintf(`{"action":"forget","id":%q,"revision":%d,"scope":"workspace"}`, record.ID, record.Revision),
				fmt.Sprintf(`{"action":"forget","id":%q,"revision":%d,"scope":"global"}`, record.ID, record.Revision),
			} {
				prepared, err := tool.Prepare(ctx, domain.ToolCall{ID: domain.NewToolCallID(), Name: "memory", Arguments: json.RawMessage(args)})
				if err != nil {
					t.Fatal(err)
				}
				if prepared.Risk != domain.R2 {
					t.Fatalf("memory mutation must prepare R2 (no approval): %+v", prepared)
				}
				for _, mode := range []permission.ApprovalMode{permission.ModeOnRequest, permission.ModeDangerOnly, permission.ModeNever} {
					if err := first.SetApprovalMode(ctx, mode); err != nil {
						t.Fatal(err)
					}
					if verdict := first.CurrentPolicy().Evaluate(prepared); verdict.Decision != domain.DecisionAllow {
						t.Fatalf("bootstrap memory policy in mode %s: %+v, want allow", mode, verdict)
					}
				}
			}
			mutate(first, fmt.Sprintf(`{"action":"remember","id":%q,"revision":%d,"content":"workspace preference gamma","scope":"workspace"}`, record.ID, record.Revision))
			checkPrompt(second, "workspace preference gamma", "workspace preference alpha")
			mutate(first, fmt.Sprintf(`{"action":"forget","id":%q,"revision":%d,"scope":"workspace"}`, record.ID, record.Revision+1))
			checkPrompt(second, "global preference beta", "workspace preference gamma")
			cancelled, cancel := context.WithCancel(ctx)
			cancel()
			if _, _, err := second.CurrentPrompt().Build(cancelled); !errors.Is(err, context.Canceled) {
				t.Fatalf("cancelled memory read error was swallowed: %v", err)
			}
			if _, err := os.Stat(filepath.Join(cfg.Storage.BaseDir, "memories")); !os.IsNotExist(err) {
				t.Fatalf("memory should not create a filesystem store: %v", err)
			}
		})
	}
}

// One sweep pass purges sessions archived beyond gc_archived_after (and
// notifies the purge hook), collects orphaned artifacts past the grace
// period, and leaves active sessions and fresh blobs untouched. A repeat
// pass is a no-op.
func TestRunSessionSweepPurgesAndCollects(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}

	archived, active := domain.NewSessionID(), domain.NewSessionID()
	for _, id := range []domain.SessionID{archived, active} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
	}
	if _, err := store.SetSessionArchived(ctx, archived, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	oldOrphan, err := artStore.PutBytes(ctx, []byte("old orphan"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	digest := strings.TrimPrefix(oldOrphan.ID.String(), "art_sha256_")
	blobPath := filepath.Join(artStore.Root(), "sha256", digest[:2], digest[2:])
	oldTime := time.Now().Add(-artifact.DefaultGCGracePeriod - time.Hour)
	if err := os.Chtimes(blobPath, oldTime, oldTime); err != nil {
		t.Fatalf("Chtimes: %v", err)
	}
	fresh, err := artStore.PutBytes(ctx, []byte("fresh orphan"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	// A 1ns retention purges anything archived before the pass runs — the
	// artifact writes between archiving and sweeping make the gap orders
	// of magnitude wider than the retention.
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{GCArchivedAfter: time.Nanosecond},
	})
	var purged []domain.SessionID
	proc.SetSessionPurgeHook(func(_ context.Context, ids []domain.SessionID) {
		purged = append(purged, ids...)
	})

	proc.runSessionSweepOnce(ctx, store, artStore)

	if len(purged) != 1 || purged[0] != archived {
		t.Fatalf("purged = %v, want [%s]", purged, archived)
	}
	def, _, err := store.ListSessions(ctx, "", 10, false, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions default: %v", err)
	}
	if len(def) != 1 || def[0].ID != active {
		t.Fatalf("default listing = %+v, want only the active session", def)
	}
	arch, _, err := store.ListSessions(ctx, "", 10, true, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions archived: %v", err)
	}
	if len(arch) != 0 {
		t.Fatalf("archived listing = %+v, want empty", arch)
	}
	if _, err := os.Stat(blobPath); !os.IsNotExist(err) {
		t.Fatalf("old orphan still exists: %v", err)
	}
	if _, ok := artStore.PathForRef(fresh); !ok {
		t.Fatal("fresh blob must survive the grace period")
	}

	// A repeat pass finds nothing: the hook must not fire again.
	proc.runSessionSweepOnce(ctx, store, artStore)
	if len(purged) != 1 {
		t.Fatalf("purged after repeat pass = %v, want unchanged", purged)
	}
}

// A blob shared by two sessions (content addressing makes identical
// outputs the same blob) must survive the purge of one referrer: the
// remaining session's artifact_refs row keeps it referenced.
func TestRunSessionSweepKeepsSharedArtifact(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	shared, err := artStore.PutBytes(ctx, []byte("shared blob"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	expired, active := domain.NewSessionID(), domain.NewSessionID()
	for _, id := range []domain.SessionID{expired, active} {
		if err := store.CreateSession(ctx, id, domain.WorkspaceID{}); err != nil {
			t.Fatalf("CreateSession: %v", err)
		}
		appendArtifactRef(t, store, id, shared)
	}
	if _, err := store.SetSessionArchived(ctx, expired, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{GCArchivedAfter: time.Nanosecond},
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	if _, err := store.IsSessionArchived(ctx, expired); err == nil {
		t.Fatal("expired session must be purged")
	}
	if _, ok := artStore.PathForRef(shared); !ok {
		t.Fatal("shared blob must survive: the active session still references it")
	}
}

// A session archived by the sweep itself must never be purged by the same
// pass: archiving stamps archived_at after the pass cutoff.
func TestRunSessionSweepDoesNotPurgeFreshlyArchived(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	stale := domain.NewSessionID()
	if err := store.CreateSession(ctx, stale, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{
		Sessions: config.ResolvedSessions{
			AutoArchiveAfter: time.Nanosecond,
			GCArchivedAfter:  time.Nanosecond,
		},
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	archived, err := store.IsSessionArchived(ctx, stale)
	if err != nil {
		t.Fatalf("IsSessionArchived: %v (the session must survive the pass)", err)
	}
	if !archived {
		t.Fatal("stale session must be archived by the pass")
	}
}

// appendArtifactRef persists one event plus a checkpoint referencing the
// artifact — the durable reference artifact GC consults.
func appendArtifactRef(t *testing.T, store *session.SQLiteStore, id domain.SessionID, ref domain.ArtifactRef) {
	t.Helper()
	now := time.Now().UTC()
	evt := domain.Event{
		ID: domain.NewEventID(), SessionID: id, Sequence: 1,
		Type: domain.EventSessionCreated, Timestamp: now,
	}
	ckpt := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: id, Sequence: 1,
		Messages: []domain.Message{{
			ID: domain.NewMessageID(), Sequence: 1, Role: domain.RoleAssistant,
			Status: domain.MessageStatusFinal, Revision: 1, CreatedAt: now,
			Parts: []domain.ContentPart{{Kind: domain.PartArtifact, Artifact: &ref}},
		}},
		CreatedAt: now,
	}
	if err := store.AppendEventsAndCheckpoint(context.Background(), id, 0, []domain.Event{evt}, ckpt); err != nil {
		t.Fatalf("AppendEventsAndCheckpoint: %v", err)
	}
}

// With every knob off the sweep still collects orphaned artifacts but
// never purges a session.
func TestRunSessionSweepKeepsArchivedSessionsWhenPurgeDisabled(t *testing.T) {
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	archived := domain.NewSessionID()
	if err := store.CreateSession(ctx, archived, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if _, err := store.SetSessionArchived(ctx, archived, true); err != nil {
		t.Fatalf("SetSessionArchived: %v", err)
	}

	proc := &ProcessRuntime{
		Store:    store,
		Artifact: artStore,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	proc.resolved.Store(&config.ResolvedConfig{})
	proc.SetSessionPurgeHook(func(_ context.Context, ids []domain.SessionID) {
		t.Fatalf("purge hook fired with purge disabled: %v", ids)
	})
	proc.runSessionSweepOnce(ctx, store, artStore)

	arch, _, err := store.ListSessions(ctx, "", 10, true, domain.WorkspaceID{})
	if err != nil {
		t.Fatalf("ListSessions archived: %v", err)
	}
	if len(arch) != 1 || arch[0].ID != archived {
		t.Fatalf("archived listing = %+v, want the archived session kept", arch)
	}
}
