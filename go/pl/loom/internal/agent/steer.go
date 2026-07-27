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

// SteerCellCapacity bounds the pending-steer queue. It is a soft cap
// against runaway scripts; a human cannot out-type it.
const SteerCellCapacity = 8

// ErrSteerCellFull is returned by SteerCell.Put when the queue is at
// capacity; the UI restores the draft so nothing is lost.
var ErrSteerCellFull = errors.New("steer queue is full (8 pending); wait for injection or press Ctrl+C to flush")

// SteerCell is the mailbox between the controller (receiving user input
// while a turn is busy) and the loop (which owns the Run). It is a bounded
// FIFO: every queued message is preserved in submission order.
//
// The cell lives on the Bootstrap, so leftovers survive a turn boundary and
// become the next turn's prompt (docs/STEER_DESIGN.md §3.1). It is
// deliberately volatile — a message is durable only after the loop drains
// it (docs/STEER_DESIGN.md §3.3).
type SteerCell struct {
	mu    sync.Mutex
	queue []string
}

// NewSteerCell creates an empty cell.
func NewSteerCell() *SteerCell { return &SteerCell{} }

// Put appends a message to the queue. Blank text and a full queue are
// errors; the caller (controller) surfaces them so the UI can restore the
// composer's draft.
func (c *SteerCell) Put(text string) error {
	text = strings.TrimSpace(text)
	if text == "" {
		return errors.New("steer text must not be empty")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.queue) >= SteerCellCapacity {
		return ErrSteerCellFull
	}
	c.queue = append(c.queue, text)
	return nil
}

// Take drains the queue and returns all messages in submission order.
func (c *SteerCell) Take() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := c.queue
	c.queue = nil
	return out
}

// Peek returns an ordered copy of the queue without draining it
// (Snapshot.PendingSteers).
func (c *SteerCell) Peek() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return slices.Clone(c.queue)
}

// Len reports the pending count.
func (c *SteerCell) Len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.queue)
}
