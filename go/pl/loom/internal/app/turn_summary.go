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
// Created: 2026/09/06

package app

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/render"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// TurnSummary is the review-oriented projection of one finished turn: which
// files the turn's write tools touched. Frontends render it as the turn's
// closing block (the "turn changes" card) so a long conversation can be reviewed
// turn by turn without walking scattered tool blocks. It is DERIVED state —
// rebuilt from the event log on resume (turnSummariesFromEvents); nothing
// persists it separately.
//
// Coverage note: only loom write tools produce file.changed events; a
// run_cmd that writes files via sed/tee bypasses the ledger (the server
// error message and the card's revert-button tooltip both state this).
//
// Turn is the run's ordinal: 1-based live from the controller's turn
// counter; on rebuild it is the projection's own ordinal (sessions with
// file-less turns cannot be numbered exactly without counting prompts —
// the field is informational either way; blocks key off RunID).
type TurnSummary struct {
	RunID     domain.RunID                  `json:"run_id"`
	Turn      int                           `json:"turn"`
	Cancelled bool                          `json:"cancelled,omitempty"`
	Failed    bool                          `json:"failed,omitempty"`
	Changes   []runtimeevent.TurnFileChange `json:"changes"`
}

// turnSummariesCap bounds the in-memory projection kept (and shipped) per
// snapshot. The cap keeps long sessions' snapshots bounded; the oldest
// summaries fall off — their changes are then reviewable only via the
// workspace git panel, same as sessions written before this projection
// existed.
const turnSummariesCap = 400

// recordTurnChange folds one successful file mutation into the turn's
// deduplicated change list: the first mutation of a path establishes it
// (oldHash empty means the file did not exist before the turn), later ones
// only bump the edit count and refresh the trailing size.
func recordTurnChange(changes []runtimeevent.TurnFileChange, path, oldHash string, size int64) []runtimeevent.TurnFileChange {
	for i := range changes {
		if changes[i].Path == path {
			changes[i].Edits++
			changes[i].Size = size
			return changes
		}
	}
	return append(changes, runtimeevent.TurnFileChange{
		Path:    path,
		Created: oldHash == "",
		Edits:   1,
		Size:    size,
	})
}

// fileChangedDTO mirrors the agent package's unexported fileChangedPayload.
type fileChangedDTO struct {
	Path    string `json:"path"`
	OldHash string `json:"old_hash"`
	NewHash string `json:"new_hash"`
	Size    int64  `json:"size"`
}

// decodeFileChange reads a file.changed payload; ok is false for events
// written by versions without a path (unrecoverable garbage is skipped —
// a projection must never fail the whole snapshot on one bad row).
func decodeFileChange(evt domain.Event) (fileChangedDTO, bool) {
	var payload fileChangedDTO
	if err := json.Unmarshal(evt.Payload, &payload); err != nil || payload.Path == "" {
		return fileChangedDTO{}, false
	}
	return payload, true
}

// turnSummariesFromEvents rebuilds the per-turn change projection from the
// persisted timeline — the resume/rewind/reconnect path, where no live
// accumulation exists. Runs are delimited by run.created and the terminal
// run.{completed,failed,cancelled} events; a run left open at the log tail
// is in flight (or died mid-turn) and is not projected.
//
// Run attribution details:
//   - Continuation/recovery runs carry their id on run.created.
//   - The FIRST run of a session never writes run.created (NewRun appends
//     nothing); its id is recovered from the run_id stamped on persisted
//     assistant-message metadata.
//   - A run.interrupted marker (written by crash recovery) closes the
//     open segment without a cancelled/failed flag.
func turnSummariesFromEvents(events []domain.Event) []TurnSummary {
	var summaries []TurnSummary
	var runID domain.RunID
	var changes []runtimeevent.TurnFileChange
	open := false

	flush := func(cancelled, failed bool) {
		if !open || len(changes) == 0 || runID.IsZero() {
			open, changes, runID = false, nil, domain.RunID{}
			return
		}
		summaries = append(summaries, TurnSummary{
			RunID:     runID,
			Turn:      len(summaries) + 1,
			Cancelled: cancelled,
			Failed:    failed,
			Changes:   changes,
		})
		open, changes, runID = false, nil, domain.RunID{}
	}

	for _, evt := range events {
		switch evt.Type {
		case domain.EventRunCreated:
			// Defensive: a new run with the previous one still open (a log
			// missing its terminal marker) closes the left-open segment.
			flush(false, false)
			var payload struct {
				RunID domain.RunID `json:"run_id"`
			}
			if err := json.Unmarshal(evt.Payload, &payload); err == nil {
				runID = payload.RunID
			}
			open = true
		case domain.EventModelResponseCompleted:
			// First-run attribution (see doc comment): borrow the run id the
			// loop stamps into assistant-message metadata.
			if open && runID.IsZero() {
				var payload domain.ResponseCompletedPayload
				if err := json.Unmarshal(evt.Payload, &payload); err == nil {
					if id, parseErr := domain.ParseRunID(payload.Message.Metadata["run_id"]); parseErr == nil {
						runID = id
					}
				}
			}
		case domain.EventFileChanged:
			if change, ok := decodeFileChange(evt); ok {
				open = true
				changes = recordTurnChange(changes, change.Path, change.OldHash, change.Size)
			}
		case domain.EventRunCompleted:
			flush(false, false)
		case domain.EventRunCancelled:
			flush(true, false)
		case domain.EventRunFailed:
			flush(false, true)
		case domain.EventRunInterrupted:
			flush(false, false)
		}
	}
	// A still-open tail segment is a turn in flight (snapshot mid-turn) or a
	// crash with no recovery marker yet — the live path projects it once the
	// turn resolves.
	return summaries
}

// appendTurnSummary appends one finished turn's projection, enforcing the
// newest-wins cap.
func appendTurnSummary(summaries []TurnSummary, summary TurnSummary) []TurnSummary {
	summaries = append(summaries, summary)
	if len(summaries) > turnSummariesCap {
		summaries = summaries[len(summaries)-turnSummariesCap:]
	}
	return summaries
}

// noteFileChange folds one successful file mutation into the in-flight
// turn's accumulator. Called from the publishing store wrapper as
// file.changed events are persisted (single observation point shared by
// edit/write batch execution paths).
func (c *Controller) noteFileChange(path, oldHash string, size int64) {
	c.mu.Lock()
	c.turnChanges = recordTurnChange(c.turnChanges, path, oldHash, size)
	c.mu.Unlock()
}

// finishTurnChanges consumes the in-flight turn's accumulated mutations at
// the turn boundary: the returned slice rides the turn.finished payload,
// and the same data joins the turnSummaries projection for snapshot
// rebuilds. Nil when the turn wrote no files (or carried no run id).
func (c *Controller) finishTurnChanges(runID domain.RunID, turn int, cancelled, failed bool) []runtimeevent.TurnFileChange {
	c.mu.Lock()
	defer c.mu.Unlock()
	changes := c.turnChanges
	c.turnChanges = nil
	if len(changes) == 0 || runID.IsZero() {
		return nil
	}
	// One defensive copy separates the projection from the payload slice:
	// both must stay independent of any later append elsewhere.
	c.turnSummaries = appendTurnSummary(c.turnSummaries, TurnSummary{
		RunID:     runID,
		Turn:      turn,
		Cancelled: cancelled,
		Failed:    failed,
		Changes:   append([]runtimeevent.TurnFileChange(nil), changes...),
	})
	return changes
}

// RevertRunChanges undoes the file mutations of ONE past turn (cancelled
// turns lose their partial edits; finished turns roll their changes back).
// Unlike Rewind it neither truncates the event log nor deletes the ledger
// entries: the mutation stays auditable, and the returned breakdown lists
// per-path conflicts (files touched outside the recorded history after the
// turn) instead of silently clobbering them.
func (c *Controller) RevertRunChanges(ctx context.Context, runID string) (RewindOutcome, error) {
	resultCh := make(chan controllerResult, 1)
	select {
	case c.cmdCh <- controllerCommand{Kind: cmdRevertRunChanges, RunID: runID, ResultCh: resultCh}:
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

// --- per-turn change review (stats + inline diff) ---

// RunFileStat pairs ONE turn's ledger entry with the file's CURRENT state
// on disk: sizes, line-level +/−, and a compact rendered diff. The
// comparison deliberately involves no git — it is "the ledger's
// before-content vs the file as it exists now", so the review affordance
// works in non-git workspaces too (at the honest cost of including any
// edits made after the turn, by anyone).
type RunFileStat struct {
	Path       string `json:"path"`
	Created    bool   `json:"created,omitempty"`
	Edits      int    `json:"edits,omitempty"`
	BeforeSize int    `json:"before_size"` // -1 when the ledger never captured the content
	AfterSize  int64  `json:"after_size"`  // -1 when the file no longer exists or is unreadable
	Added      int    `json:"added"`
	Removed    int    `json:"removed"`
	// Diff is REAL unified hunks (@@ headers with line numbers, +/-/space
	// prefixed lines — the review format; tool.prepared's compact
	// "..."-separated form is for glanceable tool cards, not this).
	// Empty when identical or when NotComparable explains why.
	Diff          string `json:"diff,omitempty"`
	DiffTruncated bool   `json:"diff_truncated,omitempty"`
	// NotComparable carries the reason Diff is empty (content never captured,
	// oversized/binary/unreadable file); empty means Diff is trustworthy.
	NotComparable string `json:"not_comparable,omitempty"`
}

const (
	// runDiffMaxReadBytes caps the current-content read per file (metadata
	// and binary safety; the ledger's own capture cap covers the before side).
	runDiffMaxReadBytes = 2 << 20
	// runDiffRenderMaxLines bounds the served compact diff per file.
	runDiffRenderMaxLines = 240
)

// RunChangeStats renders the per-path review data of one run (stats +
// inline diffs) for the turn-summary block's diff affordances. It is a
// pure read path — the store is read under WAL semantics and the fields
// consulted are immutable or taken under RLock — so it deliberately does
// NOT queue on the command channel (a diff fetch must not wait behind a
// long-running turn).
func (c *Controller) RunChangeStats(ctx context.Context, runID string) ([]RunFileStat, error) {
	c.mu.Lock()
	sessionID := c.sessionID
	summaries := append([]TurnSummary(nil), c.turnSummaries...)
	c.mu.Unlock()
	if sessionID.IsZero() {
		return nil, fmt.Errorf("no active session; call NewSession or ResumeSession first")
	}
	rid, err := domain.ParseRunID(runID)
	if err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, "invalid run id", domain.WithCause(err))
	}
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("turn change review is unavailable for this store")
	}
	changes, err := store.ListFileChangesForRun(ctx, sessionID, rid)
	if err != nil {
		return nil, fmt.Errorf("list turn file changes: %w", err)
	}
	editsByPath := map[string]int{}
	for _, s := range summaries {
		if s.RunID != rid {
			continue
		}
		for _, ch := range s.Changes {
			editsByPath[ch.Path] = ch.Edits
		}
	}
	stats := make([]RunFileStat, 0, len(changes))
	for _, change := range changes {
		stats = append(stats, buildRunFileStat(c.bootstrap.Validator, change, editsByPath[change.Path]))
	}
	return stats, nil
}

// buildRunFileStat resolves one ledger entry against the current file.
// Top-level (validator is a parameter, like restoreRewindChanges) so the
// size/content/branch matrix is unit-testable without a controller.
func buildRunFileStat(validator *workspace.PathValidator, change session.FileChange, edits int) RunFileStat {
	stat := RunFileStat{
		Path:       change.Path,
		Created:    !change.BeforeExisted,
		Edits:      edits,
		BeforeSize: -1,
		AfterSize:  -1,
	}
	beforeCaptured := change.Restorable
	var before []byte
	if beforeCaptured {
		before = change.BeforeContent
		stat.BeforeSize = len(before)
	}

	// Current content: missing files compare as all-removal; unreadable/oversized
	// ones still report their size but skip the diff.
	var after []byte
	afterPresent := false
	afterOversized := false
	resolved, err := validator.ResolveLexical(change.Path)
	if err == nil {
		content, size, truncated, readErr := readCappedFile(resolved.Absolute, runDiffMaxReadBytes)
		switch {
		case readErr == nil:
			after, afterPresent, stat.AfterSize = content, true, size
			afterOversized = truncated
		case errors.Is(readErr, os.ErrNotExist):
			// after stays empty: afterStat already -1
		default:
			stat.NotComparable = "Path cannot be resolved or read"
			afterPresent = true // do not fall into the all-removal interpretation
		}
	} else {
		stat.NotComparable = "Path is outside the workspace; cannot read"
	}

	switch {
	case stat.NotComparable != "":
	case !beforeCaptured:
		stat.NotComparable = "Original content not recorded (file too large; not captured in snapshot)"
	case afterOversized:
		stat.NotComparable = "File too large (>2MB); inline diff unavailable"
	case !afterPresent && !change.BeforeExisted:
		// Turn created a file that no longer exists: full content unknown on
		// both sides — nothing meaningful to compare beyond the record itself.
		stat.NotComparable = "File deleted (created this turn)"
	default:
		beforeText, afterText := string(before), string(after)
		if !utf8.ValidString(beforeText) || !utf8.ValidString(afterText) {
			stat.NotComparable = "Binary file; inline diff unavailable"
			break
		}
		stat.Added, stat.Removed = render.CountLineDiff(beforeText, afterText)
		stat.DiffTruncated = render.DiffInputCapped(beforeText, afterText)
		stat.Diff = render.UnifiedTexts(beforeText, afterText, 1, runDiffRenderMaxLines)
		if !afterPresent {
			// Deleted after the turn (or reverted): the all-removal diff above
			// is exactly the review artifact for that state.
			stat.DiffTruncated = false
		}
	}
	return stat
}

// readCappedFile reads at most limit+1 bytes so oversize is distinguishable
// from exactly-limit-sized content. size is always the on-disk size from
// Stat (even when content is truncated).
func readCappedFile(path string, limit int64) (content []byte, size int64, truncated bool, err error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, 0, false, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, 0, false, err
	}
	if info.IsDir() {
		return nil, 0, false, fmt.Errorf("path is a directory")
	}
	size = info.Size()
	buf, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil {
		return nil, size, false, err
	}
	if int64(len(buf)) > limit {
		// The diff affordance drops whole-file comparison past the cap; the
		// caller still reports the true size.
		return nil, size, true, nil
	}
	return buf, size, false, nil
}

func (c *Controller) handleRevertRunChanges(cmd controllerCommand) {
	c.mu.Lock()
	if c.state != ControllerStateIdle && c.state != ControllerStateBooting {
		c.mu.Unlock()
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("cannot revert turn changes in state %q", c.state)}
		return
	}
	sessionID := c.sessionID
	c.mu.Unlock()
	if sessionID.IsZero() {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("no active session; call NewSession or ResumeSession first")}
		return
	}
	runID, err := domain.ParseRunID(cmd.RunID)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: domain.NewError(domain.ErrInvalidInput, "invalid run id", domain.WithCause(err))}
		return
	}
	store, ok := c.bootstrap.Store.(*session.SQLiteStore)
	if !ok {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("turn revert is unavailable for this store")}
		return
	}
	changes, err := store.ListFileChangesForRun(c.sessionCtx, sessionID, runID)
	if err != nil {
		cmd.ResultCh <- controllerResult{Err: fmt.Errorf("list turn file changes: %w", err)}
		return
	}
	if len(changes) == 0 {
		cmd.ResultCh <- controllerResult{Err: domain.NewError(domain.ErrInvalidInput,
			"the turn recorded no restorable file changes (run_cmd writes bypass the change ledger)")}
		return
	}
	outcome := restoreRewindChanges(c.bootstrap.Validator, c.bootstrap.FileStateBook, changes)
	c.logger.Info("turn changes reverted",
		"session_id", sessionID,
		"run_id", runID,
		"restored", len(outcome.Restored),
		"deleted", len(outcome.Deleted),
		"conflicts", len(outcome.Conflicts),
		"skipped", len(outcome.Skipped))
	cmd.ResultCh <- controllerResult{Value: outcome}
}
