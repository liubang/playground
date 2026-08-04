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
// Created: 2026/08/03

package runtimeevent

import (
	"fmt"
	"testing"
)

func replayEvent(seq uint64) RuntimeEvent {
	return RuntimeEvent{Version: RuntimeEventVersion, Sequence: seq, Kind: KindTurnStarted}
}

func replaySeqs(events []RuntimeEvent) []uint64 {
	out := make([]uint64, 0, len(events))
	for _, e := range events {
		out = append(out, e.Sequence)
	}
	return out
}

func TestReplayLogSinceReturnsEventsAfterCursor(t *testing.T) {
	l := NewReplayLog(8)
	for seq := uint64(1); seq <= 5; seq++ {
		l.Append(replayEvent(seq))
	}
	events, ok := l.Since(2)
	if !ok {
		t.Fatalf("Since(2) ok = false, want true")
	}
	got := replaySeqs(events)
	want := []uint64{3, 4, 5}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("Since(2) = %v, want %v", got, want)
	}
}

func TestReplayLogSparseSequencesCompareByMagnitude(t *testing.T) {
	l := NewReplayLog(8)
	// Global sequences leave holes within a session (other sessions' events).
	for _, seq := range []uint64{3, 7, 42, 100} {
		l.Append(replayEvent(seq))
	}
	events, ok := l.Since(8)
	if !ok {
		t.Fatalf("Since(8) ok = false, want true")
	}
	got := replaySeqs(events)
	want := []uint64{42, 100}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("Since(8) = %v, want %v", got, want)
	}
}

func TestReplayLogRotationEvictsOldest(t *testing.T) {
	l := NewReplayLog(3)
	for seq := uint64(1); seq <= 5; seq++ {
		l.Append(replayEvent(seq))
	}
	if l.Len() != 3 {
		t.Fatalf("Len = %d, want 3", l.Len())
	}
	// seq 1 and 2 rotated out; a cursor below maxEvicted (2) fails.
	if _, ok := l.Since(1); ok {
		t.Fatalf("Since(1) ok = true, want false (cursor rotated out)")
	}
	events, ok := l.Since(2)
	if !ok {
		t.Fatalf("Since(2) ok = false, want true (boundary is still honorably served)")
	}
	got := replaySeqs(events)
	want := []uint64{3, 4, 5}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("Since(2) = %v, want %v", got, want)
	}
}

func TestReplayLogGlobalWatermarkCursorAccepted(t *testing.T) {
	l := NewReplayLog(4)
	l.Append(replayEvent(1))
	l.Append(replayEvent(2))
	// A cursor sampled from the GLOBAL sequence space (e.g. Snapshot's
	// EventSeq watermark) routinely exceeds this session's own max seq in
	// multi-session processes: it means "up to date", served with an empty
	// replay (review H2). Cross-lifetime detection happens one level up,
	// against the global broker sequence.
	events, ok := l.Since(1234)
	if !ok {
		t.Fatalf("Since(1234) ok = false, want true (global watermark cursor)")
	}
	if len(events) != 0 {
		t.Fatalf("Since(1234) = %v, want empty", events)
	}
}

func TestReplayLogInvalidatePoisonsPreGapCursors(t *testing.T) {
	l := NewReplayLog(8)
	for seq := uint64(1); seq <= 3; seq++ {
		l.Append(replayEvent(seq))
	}
	// The pump lost events 4..9 (broker disconnect gap); the resubscribe
	// invalidates everything below the global watermark 9.
	l.Invalidate(9)
	if _, ok := l.Since(3); ok {
		t.Fatalf("Since(3) ok = true, want false (pre-gap cursor poisoned)")
	}
	if _, ok := l.Since(8); ok {
		t.Fatalf("Since(8) ok = true, want false (pre-gap cursor poisoned)")
	}
	// A cursor at/above the floor is honored: the holder has a post-gap
	// snapshot and only needs future events.
	if _, ok := l.Since(9); !ok {
		t.Fatalf("Since(9) ok = false, want true (post-resync cursor)")
	}
	// The ring keeps accepting events after invalidation.
	l.Append(replayEvent(10))
	events, ok := l.Since(9)
	if !ok || len(events) != 1 || events[0].Sequence != 10 {
		t.Fatalf("Since(9) = (%v, %v), want ([10], true)", events, ok)
	}
}

func TestReplayLogEmptyLog(t *testing.T) {
	l := NewReplayLog(4)
	events, ok := l.Since(0)
	if !ok {
		t.Fatalf("Since(0) on empty log ok = false, want true")
	}
	if len(events) != 0 {
		t.Fatalf("Since(0) on empty log = %v, want empty", events)
	}
}

func TestReplayLogUpToDateCursor(t *testing.T) {
	l := NewReplayLog(4)
	l.Append(replayEvent(7))
	events, ok := l.Since(7)
	if !ok {
		t.Fatalf("Since(7) ok = false, want true")
	}
	if len(events) != 0 {
		t.Fatalf("Since(7) = %v, want empty (up to date)", events)
	}
}

// TestReplayLogPropertyRandomCursors checks the no-loss/no-dup contract
// against a golden slice across many cursor positions and rotations.
func TestReplayLogPropertyRandomCursors(t *testing.T) {
	const total = 500
	const cap = 64
	l := NewReplayLog(cap)
	var golden []uint64
	for seq := uint64(1); seq <= total; seq++ {
		l.Append(replayEvent(seq))
		golden = append(golden, seq)
	}
	maxEvicted := golden[total-cap-1] // the last sequence rotated out
	for cursor := uint64(0); cursor <= total+5; cursor++ {
		events, ok := l.Since(cursor)
		if cursor < maxEvicted {
			if ok {
				t.Fatalf("Since(%d) ok = true, want false", cursor)
			}
			continue
		}
		if !ok {
			t.Fatalf("Since(%d) ok = false, want true", cursor)
		}
		// Expect exactly the golden suffix (cursor, total] intersected with
		// the retained window [maxEvicted+1, total]. Cursors beyond total
		// (global watermark past this session's max) yield an empty replay.
		var want []uint64
		for _, seq := range golden {
			if seq > cursor && seq > maxEvicted {
				want = append(want, seq)
			}
		}
		got := replaySeqs(events)
		if fmt.Sprint(got) != fmt.Sprint(want) {
			t.Fatalf("Since(%d) = %v, want %v", cursor, got, want)
		}
	}
}
