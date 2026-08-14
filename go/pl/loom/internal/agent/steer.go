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
// Created: 2026/07/26

package agent

import (
	"errors"
	"slices"
	"strings"
	"sync"
)

// SteerCellCapacity bounds the pending queues (both kinds share it). It is
// a soft cap against runaway scripts; a human cannot out-type it.
const SteerCellCapacity = 8

// ErrSteerCellFull is returned by SteerCell.Put/PutFollowup when the queues
// are at capacity; the UI restores the draft so nothing is lost.
var ErrSteerCellFull = errors.New("steer queue is full (8 pending); wait for injection or press Ctrl+C to flush")

// SteerCell is the mailbox between the controller (receiving user input
// while a turn is busy) and the loop (which owns the Run). Messages carry a
// delivery target (deepseek-harness inbox style):
//
//   - steer (next-step): the loop drains this queue in prepare, before
//     every model call, injecting the messages into the RUNNING turn as
//     regular user messages.
//   - followup (next-turn): held back while the turn runs; the controller
//     relays exactly one per turn boundary as the next turn's prompt.
//
// The cell lives on the Bootstrap, so leftovers survive a turn boundary
// (docs/STEER_DESIGN.md §3.1). It is deliberately volatile — a message is
// durable only after the loop drains it (docs/STEER_DESIGN.md §3.3). Note
// the loop POLLS the cell at each step boundary, so injection needs no
// wakeup signal: dsh's "inject without waking the driver" is the pull
// model's natural state.
type SteerCell struct {
	mu        sync.Mutex
	steer     []string
	followups []string
}

// NewSteerCell creates an empty cell.
func NewSteerCell() *SteerCell { return &SteerCell{} }

// Put appends a message to the steer (next-step) queue. Blank text and a
// full cell are errors; the caller (controller) surfaces them so the UI
// can restore the composer's draft.
func (c *SteerCell) Put(text string) error {
	return c.put(text, true)
}

// PutFollowup appends a message to the followup (next-turn) queue.
func (c *SteerCell) PutFollowup(text string) error {
	return c.put(text, false)
}

func (c *SteerCell) put(text string, steer bool) error {
	text = strings.TrimSpace(text)
	if text == "" {
		return errors.New("steer text must not be empty")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.steer)+len(c.followups) >= SteerCellCapacity {
		return ErrSteerCellFull
	}
	if steer {
		c.steer = append(c.steer, text)
	} else {
		c.followups = append(c.followups, text)
	}
	return nil
}

// Take drains the steer queue and returns all messages in submission
// order. Followups are NOT drained — they wait for the turn boundary.
func (c *SteerCell) Take() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := c.steer
	c.steer = nil
	return out
}

// TakeFollowup pops the oldest followup, reporting whether one was
// queued. One per call: each turn boundary relays a single followup, so a
// queued batch becomes one turn per message instead of one merged turn.
func (c *SteerCell) TakeFollowup() (string, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.followups) == 0 {
		return "", false
	}
	out := c.followups[0]
	c.followups = c.followups[1:]
	return out, true
}

// Peek returns an ordered copy of the steer queue without draining it
// (Snapshot.PendingSteers).
func (c *SteerCell) Peek() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return slices.Clone(c.steer)
}

// PeekFollowups returns an ordered copy of the followup queue without
// draining it (Snapshot.PendingFollowups).
func (c *SteerCell) PeekFollowups() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return slices.Clone(c.followups)
}

// Len reports the pending steer count.
func (c *SteerCell) Len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.steer)
}

// FollowupLen reports the pending followup count.
func (c *SteerCell) FollowupLen() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.followups)
}
