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

package app

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// CheckpointInfo is the picker-facing projection of one persisted
// checkpoint (mirrors session.CheckpointSummary without exposing the
// store package to frontends).
type CheckpointInfo struct {
	Sequence  int64
	CreatedAt time.Time
	Label     string
	Turns     int
}

// RewindOutcome reports what a Rewind command did: the checkpoint the
// session was truncated back to and the per-path file restoration
// breakdown. Restored/Deleted list paths returned to their checkpoint
// state; Conflicts lists paths whose on-disk content did not match the
// recorded post-checkpoint state (an external modification was
// overwritten — reported, never silently clobbered); Skipped lists
// unrestorable paths (snapshot content was never captured, e.g. an
// oversized file).
type RewindOutcome struct {
	Checkpoint CheckpointInfo
	Restored   []string
	Deleted    []string
	Conflicts  []string
	Skipped    []string
}

// ListCheckpoints returns the current session's persisted checkpoints,
// most recent first, each labelled with the last user message it covers.
func (c *Controller) ListCheckpoints(ctx context.Context, limit int) ([]CheckpointInfo, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdListCheckpoints, Limit: limit, ResultCh: resultCh}:
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-c.doneCh:
		return nil, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return nil, result.Err
		}
		out, _ := result.Value.([]CheckpointInfo)
		return out, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-c.doneCh:
		return nil, fmt.Errorf("controller is closed")
	}
}

// Rewind truncates the current session back to the checkpoint covering
// event sequence checkpointSequence and restores every workspace file
// changed after it. The session store mutation is one transaction; the
// file restoration runs afterwards and is idempotent (content-addressed
// snapshots), so a retried rewind converges to the same state.
func (c *Controller) Rewind(ctx context.Context, checkpointSequence int64) (RewindOutcome, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdRewind, CheckpointSequence: checkpointSequence, ResultCh: resultCh}:
	case <-ctx.Done():
		return RewindOutcome{}, ctx.Err()
	case <-c.doneCh:
		return RewindOutcome{}, fmt.Errorf("controller is closed")
	}
	select {
	case result := <-resultCh:
		if result.Err != nil {
			return RewindOutcome{}, result.Err
		}
		out, _ := result.Value.(RewindOutcome)
		return out, nil
	case <-ctx.Done():
		return RewindOutcome{}, ctx.Err()
	case <-c.doneCh:
		return RewindOutcome{}, fmt.Errorf("controller is closed")
	}
}

func (c *Controller) handleListCheckpoints(cmd controllerCommand) {
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("checkpoints are unavailable for this store")}
		return
	}
	c.mu.Lock()
	sessionID := c.sessionID
	c.mu.Unlock()
	if sessionID.IsZero() {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("no active session; call NewSession or ResumeSession first")}
		return
	}
	limit := cmd.Limit
	if limit <= 0 {
		limit = 50
	}
	summaries, err := store.ListCheckpoints(c.sessionCtx, sessionID, limit)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("list checkpoints: %w", err)}
		return
	}
	infos := make([]CheckpointInfo, 0, len(summaries))
	for _, summary := range summaries {
		infos = append(infos, CheckpointInfo{
			Sequence:  summary.Sequence,
			CreatedAt: summary.CreatedAt,
			Label:     summary.Label,
			Turns:     summary.Turns,
		})
	}
	cmd.ResultCh <- controllerResult{Value: infos}
}

func (c *Controller) handleRewind(cmd controllerCommand) {
	c.mu.Lock()
	if c.state != ControllerStateIdle && c.state != ControllerStateBooting {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot rewind in state %q", c.state)}
		return
	}
	sessionID := c.sessionID
	c.mu.Unlock()
	if sessionID.IsZero() {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("no active session; call NewSession or ResumeSession first")}
		return
	}
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("rewind is unavailable for this store")}
		return
	}

	// Truncate the session (one transaction), then restore the files the
	// truncation uncovered. Restoration is deliberately outside the store
	// transaction and idempotent.
	result, err := store.RewindSession(c.sessionCtx, sessionID, cmd.CheckpointSequence)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("rewind session: %w", err)}
		return
	}
	outcome := restoreRewindChanges(c.bootstrap.Validator, c.bootstrap.FileStateBook, result.Changes)
	outcome.Checkpoint = CheckpointInfo{
		Sequence:  result.Checkpoint.Sequence,
		CreatedAt: result.Checkpoint.CreatedAt,
		Label:     result.Checkpoint.Label,
		Turns:     result.Checkpoint.Turns,
	}

	// Rebuild the in-memory run from the truncated session so the next
	// turn appends onto the rewound version — the same projection swap as
	// handleResumeSession, against the same session ID.
	inspection, err := store.InspectSession(c.sessionCtx, sessionID)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("inspect rewound session: %w", err)}
		return
	}
	run, err := agent.RecoverRun(inspection.Session.ID, inspection.Checkpoint,
		inspection.Transcript.Messages, inspection.Events, inspection.Session.Version,
		c.bootstrap.Resolved().Limits, c.clock, c.bootstrap.Validator)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("recover rewound session: %w", err)}
		return
	}

	c.mu.Lock()
	c.runID = domain.RunID{}
	c.turnCounter = 0
	c.messages = append([]domain.Message(nil), inspection.Transcript.Messages...)
	c.lastUsage = run.Usage
	c.resumedRun = run
	c.resumed = true
	// A pending compaction belongs to the truncated-away transcript.
	c.forceCompact = false
	c.state = ControllerStateIdle
	c.mu.Unlock()

	c.logger.Info("session rewound",
		"session_id", sessionID,
		"checkpoint_sequence", cmd.CheckpointSequence,
		"restored", len(outcome.Restored),
		"deleted", len(outcome.Deleted),
		"conflicts", len(outcome.Conflicts),
		"skipped", len(outcome.Skipped))
	cmd.ResultCh <- controllerResult{Value: outcome}
}

// restoreRewindChanges applies one deduplicated rewind change set to the
// workspace: files that existed at the checkpoint get their captured
// pre-mutation content written back; files created after the checkpoint
// are removed. Every path is re-validated against the workspace root and
// every write goes through the same atomic-write discipline as the edit
// tool. A path whose current content does not match the recorded latest
// post-checkpoint hash was modified outside the recorded history; it is
// still rewound (the user asked for it) but reported as a conflict. The
// file-state book is updated so a subsequent edit sees the restored
// content as the drift baseline.
func restoreRewindChanges(validator *workspace.PathValidator, book *workspace.FileStateBook, changes []session.FileChange) RewindOutcome {
	var out RewindOutcome
	for _, change := range changes {
		if !change.Restorable {
			out.Skipped = append(out.Skipped, change.Path)
			continue
		}
		resolved, err := validator.ResolveLexical(change.Path)
		if err != nil {
			out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (path no longer resolvable: %v)", change.Path, err))
			continue
		}

		// Conflict detection: compare the on-disk state with the LATEST
		// recorded post-checkpoint state. A mismatch means the file was
		// modified outside the recorded history.
		conflict := ""
		snapshot, statErr := validator.Snapshot(resolved.Absolute)
		switch {
		case statErr == nil:
			if change.LatestAfterHash != "" && snapshot.SHA256 != change.LatestAfterHash {
				conflict = "external modification overwritten"
			}
		case errors.Is(statErr, os.ErrNotExist):
			// Currently absent: restoring recreates the file; deleting is a no-op.
		default:
			out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (stat failed: %v)", change.Path, statErr))
			continue
		}

		if change.BeforeExisted {
			content := change.BeforeContent
			if content == nil {
				// Restorable with nil content is the empty-file sentinel
				// (session.RecordFileChange normalizes it to an empty
				// blob, but a driver may still read an empty blob back
				// as nil).
				content = []byte{}
			}
			expected := workspace.EmptyFileSHA256
			if statErr == nil {
				expected = snapshot.SHA256
			} else if err := os.MkdirAll(filepath.Dir(resolved.Absolute), 0o755); err != nil {
				out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (recreate parent: %v)", change.Path, err))
				continue
			}
			written, err := validator.AtomicWrite(resolved.Absolute, content, workspace.AtomicWriteOptions{
				ExpectedHash: expected,
				SyncParent:   true,
			})
			if err != nil {
				out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (restore failed: %v)", change.Path, err))
				continue
			}
			book.Record(resolved.Absolute, written.SHA256)
			if conflict != "" {
				out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (%s)", change.Path, conflict))
			}
			out.Restored = append(out.Restored, change.Path)
			continue
		}

		// The file was created after the checkpoint: rewind removes it.
		if statErr != nil {
			// Already absent — nothing to do.
			continue
		}
		if err := os.Remove(resolved.Absolute); err != nil {
			out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (delete failed: %v)", change.Path, err))
			continue
		}
		book.Forget(resolved.Absolute)
		if conflict != "" {
			out.Conflicts = append(out.Conflicts, fmt.Sprintf("%s (%s)", change.Path, conflict))
		}
		out.Deleted = append(out.Deleted, change.Path)
	}
	return out
}
