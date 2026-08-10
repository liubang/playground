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
// Created: 2026/07/23

package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// writeTestConfig points LOOM_CONFIG at an empty offline config inside
// the given loom home — the config file's directory is the data root.
func writeTestConfig(t *testing.T, baseDir string) {
	t.Helper()
	if err := os.MkdirAll(baseDir, 0o700); err != nil {
		t.Fatalf("mkdir loom home: %v", err)
	}
	cfgPath := filepath.Join(baseDir, "config.yaml")
	if err := os.WriteFile(cfgPath, nil, 0o600); err != nil {
		t.Fatalf("write test config: %v", err)
	}
	t.Setenv(configPathEnv, cfgPath)
}

// testSessionDB returns the session store path inside base's sessions
// subdirectory (the layout SessionDBPath derives from the loom home).
func testSessionDB(base string) string {
	return filepath.Join(base, "sessions", "sessions.db")
}

func TestPrepareStorageCreatesPrivateDirectories(t *testing.T) {
	base := filepath.Join(t.TempDir(), "loom")
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: base}}
	if err := prepareStorage(resolved, true); err != nil {
		t.Fatalf("prepareStorage: %v", err)
	}
	for _, dir := range []string{base, resolved.Storage.SessionsDir()} {
		fi, err := os.Stat(dir)
		if err != nil {
			t.Fatalf("stat %s: %v", dir, err)
		}
		if fi.Mode().Perm() != 0o700 {
			t.Fatalf("%s mode = %v; want 0700", dir, fi.Mode().Perm())
		}
	}
	if got, want := resolved.Storage.SessionDBPath(), testSessionDB(base); got != want {
		t.Fatalf("SessionDBPath() = %q, want %q", got, want)
	}
}

func TestPrepareStorageTightensBaseDirPermissions(t *testing.T) {
	base := filepath.Join(t.TempDir(), "loom")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.Chmod(base, 0o755); err != nil {
		t.Fatalf("Chmod: %v", err)
	}
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: base}}
	if err := prepareStorage(resolved, true); err != nil {
		t.Fatalf("prepareStorage: %v", err)
	}
	fi, err := os.Stat(base)
	if err != nil {
		t.Fatalf("stat base dir: %v", err)
	}
	if fi.Mode().Perm() != 0o700 {
		t.Fatalf("base dir mode = %v; want 0700 (loom-owned dirs are tightened)", fi.Mode().Perm())
	}
}

func TestPrepareStorageRejectsSymlinkBaseDir(t *testing.T) {
	root := t.TempDir()
	realDirectory := filepath.Join(root, "real")
	if err := os.MkdirAll(realDirectory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	link := filepath.Join(root, "link")
	if err := os.Symlink(realDirectory, link); err != nil {
		t.Fatalf("Symlink: %v", err)
	}
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: link}}
	if err := prepareStorage(resolved, true); err == nil || !strings.Contains(err.Error(), "real directory") {
		t.Fatalf("prepareStorage error = %v, want symlink error", err)
	}
}

func TestPrepareStorageNoCreateLeavesFilesystemUntouched(t *testing.T) {
	base := filepath.Join(t.TempDir(), "loom")
	resolved := &config.ResolvedConfig{Storage: config.ResolvedStorage{BaseDir: base}}
	if err := prepareStorage(resolved, false); err != nil {
		t.Fatalf("prepareStorage: %v", err)
	}
	if _, err := os.Stat(base); !os.IsNotExist(err) {
		t.Fatalf("prepareStorage(create=false) created %s unexpectedly: %v", base, err)
	}
}

func TestSaveTerminalCheckpointSurvivesCancelledContext(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store, err := session.OpenSQLiteStore(ctx, path)
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()
	run := agent.NewRun(domain.NewSessionID(), domain.DefaultLimits(), domain.RealClock{})
	if err := store.CreateSession(ctx, run.SessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	run.State = domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeCancelled}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if err := saveTerminalCheckpoint(cancelled, store, run); err != nil {
		t.Fatalf("saveTerminalCheckpoint: %v", err)
	}
	checkpoint, err := store.LoadLatestCheckpoint(ctx, run.SessionID)
	if err != nil {
		t.Fatalf("LoadLatestCheckpoint: %v", err)
	}
	if checkpoint.State.Outcome != domain.OutcomeCancelled || checkpoint.Sequence != 0 {
		t.Fatalf("unexpected checkpoint: %+v", checkpoint)
	}
}

func TestListSessionsDoesNotCreateMissingStore(t *testing.T) {
	base := filepath.Join(t.TempDir(), "missing")
	writeTestConfig(t, base)
	if err := run(context.Background(), []string{"sessions"}); err != nil {
		t.Fatalf("run sessions: %v", err)
	}
	if _, err := os.Stat(testSessionDB(base)); !os.IsNotExist(err) {
		t.Fatalf("sessions created store unexpectedly: %v", err)
	}
}

func TestListSessionsCommandReadsPersistentStore(t *testing.T) {
	ctx := context.Background()
	base := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(filepath.Dir(testSessionDB(base)), 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	writeTestConfig(t, base)
	store, err := session.OpenSQLiteStore(ctx, testSessionDB(base))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	output := captureStdout(t, func() {
		if err := run(ctx, []string{"sessions"}); err != nil {
			t.Fatalf("run sessions: %v", err)
		}
	})
	if !strings.Contains(output, sessionID.String()+"\t0\t") {
		t.Fatalf("sessions output = %q, want session %s", output, sessionID)
	}
}

func TestGCCommandDeletesOnlyOldUnreferencedArtifacts(t *testing.T) {
	ctx := context.Background()
	base := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(filepath.Dir(testSessionDB(base)), 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	writeTestConfig(t, base)
	store, err := session.OpenSQLiteStore(ctx, testSessionDB(base))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	artifactStore, err := artifact.Open(filepath.Join(filepath.Dir(testSessionDB(base)), artifactDirectoryName), domain.DefaultLimits().MaxArtifactBytes)
	if err != nil {
		t.Fatalf("artifact.Open: %v", err)
	}
	orphan, err := artifactStore.PutBytes(ctx, []byte("old orphan"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}
	digest := strings.TrimPrefix(orphan.ID.String(), "art_sha256_")
	blobPath := filepath.Join(artifactStore.Root(), "sha256", digest[:2], digest[2:])
	oldTime := time.Now().Add(-artifactGCGracePeriod - time.Hour)
	if err := os.Chtimes(blobPath, oldTime, oldTime); err != nil {
		t.Fatalf("Chtimes: %v", err)
	}
	output := captureStdout(t, func() {
		if err := run(ctx, []string{"gc"}); err != nil {
			t.Fatalf("run gc: %v", err)
		}
	})
	var report artifact.GCReport
	if err := json.Unmarshal([]byte(output), &report); err != nil {
		t.Fatalf("decode GC report %q: %v", output, err)
	}
	if report.Deleted != 1 || report.DeletedBytes != orphan.Size {
		t.Fatalf("unexpected GC report: %+v", report)
	}
	if _, err := os.Stat(blobPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("orphan still exists: %v", err)
	}
}

func TestResumeCommandValidatesArgumentsBeforeProviderSetup(t *testing.T) {
	if err := run(context.Background(), []string{"resume"}); err == nil ||
		!strings.Contains(err.Error(), "usage: loom resume") {
		t.Fatalf("missing arguments error = %v", err)
	}
	if err := run(context.Background(), []string{"resume", "invalid", "continue"}); err == nil ||
		!strings.Contains(err.Error(), "parse session ID") {
		t.Fatalf("invalid session error = %v", err)
	}
}

func TestContinueRunPersistsAtExistingSessionVersion(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "sessions.db")
	store, err := session.OpenSQLiteStore(ctx, path)
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	defer store.Close()
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	// Build a terminal checkpoint over an empty persisted session; continuation
	// adds run.created and the next user message atomically at version zero.
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 0,
		State: domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeSucceeded}, CreatedAt: time.Now().UTC(),
	}
	if err := store.SaveCheckpoint(ctx, checkpoint); err != nil {
		t.Fatalf("SaveCheckpoint: %v", err)
	}
	continued, err := agent.ContinueRun(checkpoint, nil, 0, domain.DefaultLimits(), domain.RealClock{})
	if err != nil {
		t.Fatalf("ContinueRun: %v", err)
	}
	continued.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts: []domain.ContentPart{{Kind: domain.PartText, Text: "continue"}}, CreatedAt: time.Now().UTC(),
	})
	loop := agent.Loop{Run: continued, Model: &failingModel{}, Store: store, Registry: agent.NewToolRegistry()}
	if err := loop.Execute(ctx); err == nil || !strings.Contains(err.Error(), "model unavailable") {
		t.Fatalf("Execute error = %v, want model failure", err)
	}
	events, err := store.LoadEvents(ctx, sessionID, 0)
	if err != nil {
		t.Fatalf("LoadEvents: %v", err)
	}
	if len(events) != 7 || events[0].Type != domain.EventRunCreated ||
		events[1].Type != domain.EventUserMessageAdded || events[2].Type != domain.EventRunStateChanged ||
		events[3].Type != domain.EventBudgetUpdated || events[4].Type != domain.EventModelRequestStarted ||
		events[5].Type != domain.EventModelRequestFailed || events[6].Type != domain.EventRunFailed {
		t.Fatalf("unexpected continuation events: %+v", events)
	}
}

type failingModel struct{}

func (*failingModel) Stream(context.Context, domain.ModelRequest) (domain.ModelStream, error) {
	return nil, errors.New("model unavailable")
}

func TestInspectSessionCommandOutputsRecoveredJSON(t *testing.T) {
	ctx := context.Background()
	base := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(filepath.Dir(testSessionDB(base)), 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	writeTestConfig(t, base)
	store, err := session.OpenSQLiteStore(ctx, testSessionDB(base))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID, domain.WorkspaceID{}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	checkpoint := domain.Checkpoint{
		ID: domain.NewCheckpointID(), SessionID: sessionID, Sequence: 0,
		State:     domain.RunState{Lifecycle: domain.LifecycleTerminal, Outcome: domain.OutcomeSucceeded},
		Usage:     domain.Usage{Turns: 2, InputTokens: 12, OutputTokens: 7, CostUSD: 0.25},
		CreatedAt: time.Now().UTC(),
	}
	if err := store.SaveCheckpoint(ctx, checkpoint); err != nil {
		t.Fatalf("SaveCheckpoint: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	output := captureStdout(t, func() {
		if err := run(ctx, []string{"inspect", sessionID.String()}); err != nil {
			t.Fatalf("run inspect: %v", err)
		}
	})
	var inspection session.SessionInspection
	if err := json.Unmarshal([]byte(output), &inspection); err != nil {
		t.Fatalf("decode inspect output %q: %v", output, err)
	}
	if inspection.Session.ID != sessionID || inspection.Checkpoint == nil ||
		inspection.Checkpoint.Usage.CostUSD != 0.25 || inspection.Transcript.SessionID != sessionID {
		t.Fatalf("unexpected inspection: %+v", inspection)
	}
}

func TestInspectSessionCommandRejectsInvalidAndMissingSession(t *testing.T) {
	if err := run(context.Background(), []string{"inspect", "invalid"}); err == nil ||
		!strings.Contains(err.Error(), "parse session ID") {
		t.Fatalf("invalid session error = %v", err)
	}
	base := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(filepath.Dir(testSessionDB(base)), 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	writeTestConfig(t, base)
	store, err := session.OpenSQLiteStore(context.Background(), testSessionDB(base))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if err := run(context.Background(), []string{"inspect", domain.NewSessionID().String()}); err == nil ||
		!strings.Contains(err.Error(), "session not found") {
		t.Fatalf("missing session error = %v", err)
	}
}

func TestInspectSessionDoesNotCreateMissingStore(t *testing.T) {
	base := filepath.Join(t.TempDir(), "missing")
	writeTestConfig(t, base)
	err := run(context.Background(), []string{"inspect", domain.NewSessionID().String()})
	if err == nil || !strings.Contains(err.Error(), "session store does not exist") {
		t.Fatalf("inspect error = %v", err)
	}
	if _, statErr := os.Stat(testSessionDB(base)); !os.IsNotExist(statErr) {
		t.Fatalf("inspect created store unexpectedly: %v", statErr)
	}
}

// Regression (REVIEW M13): consoleApprover used to create a fresh
// bufio.Reader per approval — bytes the previous reader had already
// buffered were silently dropped, and every ctx-cancelled approval leaked a
// goroutine that then raced the next one on stdin. The shared reader must
// deliver pre-typed lines in order.
func TestConsoleApproverSharedReaderPreservesBufferedInput(t *testing.T) {
	approver := &consoleApprover{}
	approver.start(strings.NewReader("y\nn\n"))

	first, err := approver.awaitAnswer(context.Background())
	if err != nil || first != domain.DecisionAllow {
		t.Fatalf("first answer = %v, %v; want allow", first, err)
	}
	second, err := approver.awaitAnswer(context.Background())
	if err != nil || second != domain.DecisionDeny {
		t.Fatalf("second answer = %v, %v; want deny (buffered line must not be lost)", second, err)
	}
	// EOF closes the line channel; further approvals deny without blocking.
	third, err := approver.awaitAnswer(context.Background())
	if err != nil || third != domain.DecisionDeny {
		t.Fatalf("answer after EOF = %v, %v; want deny", third, err)
	}
}

func TestConsoleApproverAwaitAnswerRespectsCancellation(t *testing.T) {
	approver := &consoleApprover{}
	approver.start(strings.NewReader("")) // no input ever arrives until EOF
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := approver.awaitAnswer(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("awaitAnswer error = %v, want context.Canceled", err)
	}
}

// TestLoadConfigFirstRunBootstrapsDefaultPath: the first agent launch with
// no config anywhere must not die with a bare not-found error. It writes
// the starter template at ~/.loom/config.yaml and returns a directed
// "edit this file" error (the caller exits non-zero), and the second
// attempt loads the template cleanly once the user has filled a key.
func TestLoadConfigFirstRunBootstrapsDefaultPath(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv(configPathEnv, "")

	_, err := loadConfig(true, slog.Default())
	if err == nil {
		t.Fatal("loadConfig(true) without config: want first-run error")
	}
	if !strings.Contains(err.Error(), "first run") {
		t.Fatalf("loadConfig(true) error = %v, want first-run guidance", err)
	}
	def, derr := config.DefaultBaseDir()
	if derr != nil {
		t.Fatal(derr)
	}
	if _, serr := os.Stat(filepath.Join(def, config.FileName)); serr != nil {
		t.Fatalf("starter config not created: %v", serr)
	}

	// The generated template loads with providers present and the key left
	// as a comment — the state the user edits before the next launch.
	resolved, err := loadConfig(true, slog.Default())
	if err != nil {
		t.Fatalf("loadConfig(true) after bootstrap: %v", err)
	}
	if len(resolved.Providers) == 0 {
		t.Fatal("template providers = 0, want the deepseek starter provider")
	}
}

// TestLoadConfigExplicitMissingPathStaysHardError: LOOM_CONFIG names a file
// that should exist; a missing explicit path is a user error and must not
// be papered over with an auto-created template.
func TestLoadConfigExplicitMissingPathStaysHardError(t *testing.T) {
	explicit := filepath.Join(t.TempDir(), "custom", "config.yaml")
	t.Setenv(configPathEnv, explicit)

	_, err := loadConfig(true, slog.Default())
	if err == nil || !strings.Contains(err.Error(), "not found") {
		t.Fatalf("loadConfig(true) error = %v, want not-found", err)
	}
	if _, serr := os.Stat(explicit); !os.IsNotExist(serr) {
		t.Fatalf("explicit missing path was auto-created: %v", serr)
	}
}

func captureStdout(t *testing.T, fn func()) string {
	t.Helper()
	old := os.Stdout
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatalf("Pipe: %v", err)
	}
	os.Stdout = writer
	defer func() { os.Stdout = old }()

	fn()
	if err := writer.Close(); err != nil {
		t.Fatalf("close writer: %v", err)
	}
	data, err := io.ReadAll(reader)
	if err != nil {
		t.Fatalf("ReadAll: %v", err)
	}
	if err := reader.Close(); err != nil {
		t.Fatalf("close reader: %v", err)
	}
	return string(data)
}
