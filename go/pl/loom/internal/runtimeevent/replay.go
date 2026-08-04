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

import "sync"

// DefaultReplayCap is the default per-session replay ring capacity
// (LOOM_SERVE_REPLAY_CAP overrides it on the serve path).
const DefaultReplayCap = 2048

// ReplayLog is a bounded per-session ring of runtime events, ordered by the
// broker's global sequence. It backs subscription reconnection cursors
// (SSE Last-Event-ID, inproc re-subscribe): a client that remembers the
// last sequence it applied can ask for everything since (docs/SERVE_DESIGN.md
// §4.5).
//
// Sequences are globally monotonic but sparse within a session (other
// sessions' events leave holes), so Since compares by magnitude, never by
// contiguity. The zero value is ready to use with DefaultReplayCap.
type ReplayLog struct {
	mu     sync.Mutex
	events []RuntimeEvent // ring, oldest at head
	cap    int
	// maxEvicted is the highest sequence ever rotated out. A cursor below
	// it means events the client has not seen are already gone.
	maxEvicted uint64
	// maxSeen is the highest sequence ever appended. A cursor above it
	// comes from a different sequence space (server restart) and must be
	// rejected rather than silently "catching up" to nothing.
	maxSeen uint64
}

// NewReplayLog creates a ring with the given capacity (<= 0 selects
// DefaultReplayCap).
func NewReplayLog(capacity int) *ReplayLog {
	if capacity <= 0 {
		capacity = DefaultReplayCap
	}
	return &ReplayLog{cap: capacity}
}

// Append adds an event to the ring, evicting the oldest when full.
func (l *ReplayLog) Append(evt RuntimeEvent) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.cap <= 0 {
		l.cap = DefaultReplayCap
	}
	if len(l.events) == l.cap {
		l.maxEvicted = l.events[0].Sequence
		l.events = append(l.events[1:], evt)
	} else {
		l.events = append(l.events, evt)
	}
	if evt.Sequence > l.maxSeen {
		l.maxSeen = evt.Sequence
	}
}

// Since returns the buffered events with Sequence > seq, oldest first.
// ok is false only when events the caller has not seen already rotated
// out (seq < maxEvicted, where maxEvicted also reflects Invalidate).
//
// A cursor ABOVE this session's maxSeen is NOT rejected here: sequences
// are global, so a cursor sampled from a global watermark (e.g.
// Snapshot.EventSeq) routinely exceeds a quiet session's own maximum —
// that case means "up to date", answered by an empty replay plus the live
// stream. Cross-lifetime cursor detection (server restart) belongs to the
// caller, which compares against the global broker sequence
// (docs/SERVE_DESIGN.md §4.5, review H2).
func (l *ReplayLog) Since(seq uint64) (events []RuntimeEvent, ok bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if seq < l.maxEvicted {
		return nil, false
	}
	out := make([]RuntimeEvent, 0, len(l.events))
	for _, evt := range l.events {
		if evt.Sequence > seq {
			out = append(out, evt)
		}
	}
	return out, true
}

// Invalidate poisons every cursor below floor, forcing the holders of such
// cursors to resync via a fresh snapshot. Used when the event pump loses
// its broker subscription: events published during the gap never reach any
// ring, so pre-gap cursors must fail loudly instead of silently skipping
// the gap (docs/SERVE_DESIGN.md §4.5, review M6). floor should be the
// global broker sequence observed at resubscribe time.
func (l *ReplayLog) Invalidate(floor uint64) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if floor > l.maxEvicted {
		l.maxEvicted = floor
	}
}

// MaxSeen returns the highest sequence ever appended (0 when empty).
func (l *ReplayLog) MaxSeen() uint64 {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.maxSeen
}

// Len returns the number of buffered events.
func (l *ReplayLog) Len() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.events)
}
