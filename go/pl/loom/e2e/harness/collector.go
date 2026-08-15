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

package harness

import (
	"context"
	"encoding/json"
	"sync"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Collector drains a per-session event subscription and auto-resolves
// approval/question requests so e2e turns never wedge. Approvals are
// answered from a scripted FIFO decision queue (snapshot scenarios drive
// it via their approve steps); an empty queue defaults to allow.
type Collector struct {
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu          sync.Mutex
	seen        map[runtimeevent.RuntimeEventKind]int
	turns       int
	lastTurnErr string
	decisions   []domain.Decision
}

// NewCollector creates a collector over the client's subscription.
// Call Run in a goroutine to start draining.
func NewCollector(c client.Client, ch <-chan runtimeevent.RuntimeEvent) *Collector {
	return &Collector{client: c, ch: ch}
}

// EnqueueDecision scripts the answer to the next approval request.
func (c *Collector) EnqueueDecision(d domain.Decision) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.decisions = append(c.decisions, d)
}

// Run drains the subscription until the channel closes.
func (c *Collector) Run() {
	c.mu.Lock()
	c.seen = make(map[runtimeevent.RuntimeEventKind]int)
	c.mu.Unlock()
	for evt := range c.ch {
		c.mu.Lock()
		c.seen[evt.Kind]++
		if evt.Kind == runtimeevent.KindTurnFinished {
			c.turns++
			var p runtimeevent.TurnFinishedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				c.lastTurnErr = p.Error
			}
		}
		c.mu.Unlock()
		switch evt.Kind {
		case runtimeevent.KindApprovalRequested:
			var p runtimeevent.ApprovalRequestedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.ResolveApproval(context.Background(), app.ApprovalBinding{
					ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
				}, c.nextDecision(), nil)
			}
		case runtimeevent.KindQuestionAsked:
			var p runtimeevent.QuestionAskedPayload
			if err := json.Unmarshal(evt.Payload, &p); err == nil {
				_, _ = c.client.AnswerQuestion(context.Background(), p.QuestionID, domain.QuestionAnswer{Skipped: true})
			}
		}
	}
}

func (c *Collector) nextDecision() domain.Decision {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.decisions) == 0 {
		return domain.DecisionAllow
	}
	d := c.decisions[0]
	c.decisions = c.decisions[1:]
	return d
}

// TurnsDone returns how many turns have finished.
func (c *Collector) TurnsDone() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.turns
}

// LastTurnError carries the Error field of the most recent
// turn.finished event ("" for a clean finish) — the first thing to
// inspect when a turn comes back empty.
func (c *Collector) LastTurnError() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.lastTurnErr
}

// WaitTurn blocks until n turns have finished or the timeout fires.
func (c *Collector) WaitTurn(t *testing.T, n int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if c.TurnsDone() >= n {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for turn %d to finish (done=%d)", n, c.TurnsDone())
}

// AssertSaw fails unless every kind was observed at least once.
func (c *Collector) AssertSaw(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] == 0 {
			t.Fatalf("never saw event %q", k)
		}
	}
}

// AssertSawAny fails unless at least one of the kinds was observed.
func (c *Collector) AssertSawAny(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] > 0 {
			return
		}
	}
	t.Fatalf("never saw any of %v", kinds)
}

// Count returns the total observed event count.
func (c *Collector) Count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, v := range c.seen {
		n += v
	}
	return n
}
