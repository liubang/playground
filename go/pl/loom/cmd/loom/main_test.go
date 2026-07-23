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
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

func TestPrepareSessionDBPathConfiguredPrivateDirectory(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	path := filepath.Join(directory, "custom.db")
	t.Setenv(sessionDBEnv, path)
	got, err := prepareSessionDBPath()
	if err != nil {
		t.Fatalf("prepareSessionDBPath: %v", err)
	}
	if got != path {
		t.Fatalf("path = %q, want %q", got, path)
	}
}

func TestPrepareSessionDBPathRejectsInsecureConfiguredDirectory(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "shared")
	if err := os.MkdirAll(directory, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.Chmod(directory, 0o755); err != nil {
		t.Fatalf("Chmod: %v", err)
	}
	t.Setenv(sessionDBEnv, filepath.Join(directory, "sessions.db"))
	if _, err := prepareSessionDBPath(); err == nil || !strings.Contains(err.Error(), "must not be accessible") {
		t.Fatalf("prepareSessionDBPath error = %v, want insecure directory error", err)
	}
}

func TestPrepareSessionDBPathRejectsSymlinkDirectory(t *testing.T) {
	root := t.TempDir()
	realDirectory := filepath.Join(root, "real")
	if err := os.MkdirAll(realDirectory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	link := filepath.Join(root, "link")
	if err := os.Symlink(realDirectory, link); err != nil {
		t.Fatalf("Symlink: %v", err)
	}
	t.Setenv(sessionDBEnv, filepath.Join(link, "sessions.db"))
	if _, err := prepareSessionDBPath(); err == nil || !strings.Contains(err.Error(), "real directory") {
		t.Fatalf("prepareSessionDBPath error = %v, want symlink error", err)
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
	if err := store.CreateSession(ctx, run.SessionID); err != nil {
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
	path := filepath.Join(t.TempDir(), "missing", "sessions.db")
	t.Setenv(sessionDBEnv, path)
	if err := run(context.Background(), []string{"sessions"}); err != nil {
		t.Fatalf("run sessions: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("sessions created store unexpectedly: %v", err)
	}
}

func TestListSessionsCommandReadsPersistentStore(t *testing.T) {
	ctx := context.Background()
	directory := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	path := filepath.Join(directory, "sessions.db")
	t.Setenv(sessionDBEnv, path)
	store, err := session.OpenSQLiteStore(ctx, path)
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
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
	if err := store.CreateSession(ctx, sessionID); err != nil {
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
	directory := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	path := filepath.Join(directory, "sessions.db")
	t.Setenv(sessionDBEnv, path)
	store, err := session.OpenSQLiteStore(ctx, path)
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	sessionID := domain.NewSessionID()
	if err := store.CreateSession(ctx, sessionID); err != nil {
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
	directory := filepath.Join(t.TempDir(), "private")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	path := filepath.Join(directory, "sessions.db")
	t.Setenv(sessionDBEnv, path)
	store, err := session.OpenSQLiteStore(context.Background(), path)
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
	path := filepath.Join(t.TempDir(), "missing", "sessions.db")
	t.Setenv(sessionDBEnv, path)
	err := run(context.Background(), []string{"inspect", domain.NewSessionID().String()})
	if err == nil || !strings.Contains(err.Error(), "session store does not exist") {
		t.Fatalf("inspect error = %v", err)
	}
	if _, statErr := os.Stat(path); !os.IsNotExist(statErr) {
		t.Fatalf("inspect created store unexpectedly: %v", statErr)
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
