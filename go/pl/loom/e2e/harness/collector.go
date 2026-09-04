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
	"fmt"
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
//
// Resolves NEVER run on the drain loop itself: the session service drops
// any live subscriber whose queue fills (SessionService.dispatch), and a
// synchronous ResolveApproval — which itself publishes durable events —
// can stall the loop long enough for that to happen. A dropped
// subscription is closed silently, so the drain loop exits and every
// WaitTurn would otherwise burn its full timeout with no signal.
type Collector struct {
	// ctx bounds the auto-resolves (approval/question answers): when the
	// owning test is cancelled, they stop instead of retrying against a
	// torn-down server.
	ctx    context.Context
	client client.Client
	ch     <-chan runtimeevent.RuntimeEvent

	mu          sync.Mutex
	seen        map[runtimeevent.RuntimeEventKind]int
	turns       int
	lastTurnErr string
	firstErr    error // first internal failure (bad payload, resolve error)
	decisions   []domain.Decision
	defaults    int // approvals answered by the empty-queue fallback

	// turnSig receives a non-blocking poke on every turn.finished, so
	// WaitTurn wakes immediately instead of polling. done closes when Run
	// returns (subscription closed) — WaitTurn fails fast on it instead
	// of waiting out the timeout against a dead stream.
	turnSig chan struct{}
	done    chan struct{}
	runOnce sync.Once
	wg      sync.WaitGroup // in-flight auto-resolves
}

// NewCollector creates a collector over the client's subscription.
// Call Run in a goroutine to start draining.
func NewCollector(ctx context.Context, c client.Client, ch <-chan runtimeevent.RuntimeEvent) *Collector {
	return &Collector{
		ctx:     ctx,
		client:  c,
		ch:      ch,
		seen:    make(map[runtimeevent.RuntimeEventKind]int),
		turnSig: make(chan struct{}, 1),
		done:    make(chan struct{}),
	}
}

// EnqueueDecision scripts the answer to the next approval request.
func (c *Collector) EnqueueDecision(d domain.Decision) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.decisions = append(c.decisions, d)
}

// Run drains the subscription until the channel closes. In-flight
// auto-resolves are awaited before Run returns so teardown never races
// a dangling service call.
func (c *Collector) Run() {
	c.runOnce.Do(func() {
		defer close(c.done)
		defer c.wg.Wait()
		for evt := range c.ch {
			c.mu.Lock()
			c.seen[evt.Kind]++
			c.mu.Unlock()
			switch evt.Kind {
			case runtimeevent.KindTurnFinished:
				// turns++ is UNCONDITIONAL (baseline semantics): a cleanly
				// finished turn carries NO payload at all — the controller
				// only attaches TurnFinishedPayload on failure — so an empty
				// payload is the common case, not corruption.
				var p runtimeevent.TurnFinishedPayload
				if len(evt.Payload) > 0 && string(evt.Payload) != "null" {
					if err := json.Unmarshal(evt.Payload, &p); err != nil {
						c.recordErr(fmt.Errorf("decode turn.finished payload: %w", err))
					}
				}
				c.mu.Lock()
				c.turns++
				c.lastTurnErr = p.Error
				c.mu.Unlock()
				select {
				case c.turnSig <- struct{}{}:
				default:
				}
			case runtimeevent.KindApprovalRequested:
				var p runtimeevent.ApprovalRequestedPayload
				if err := json.Unmarshal(evt.Payload, &p); err != nil {
					c.recordErr(fmt.Errorf("decode approval.requested payload: %w", err))
					continue
				}
				// Pop the decision on the drain loop so the scripted FIFO
				// order stays bound to the event order; only the service
				// call goes async.
				decision := c.nextDecision()
				c.wg.Add(1)
				go func() {
					defer c.wg.Done()
					_, err := c.client.ResolveApproval(c.ctx, app.ApprovalBinding{
						ApprovalID: p.ApprovalID, CallID: p.CallID, ArgsHash: p.ArgsHash,
					}, decision, nil)
					if err != nil {
						c.recordErr(fmt.Errorf("resolve approval %s (%s): %w", p.ApprovalID, decision, err))
					}
				}()
			case runtimeevent.KindQuestionAsked:
				var p runtimeevent.QuestionAskedPayload
				if err := json.Unmarshal(evt.Payload, &p); err != nil {
					c.recordErr(fmt.Errorf("decode question.asked payload: %w", err))
					continue
				}
				c.wg.Add(1)
				go func() {
					defer c.wg.Done()
					_, err := c.client.AnswerQuestion(c.ctx, p.QuestionID, domain.QuestionAnswer{Skipped: true})
					if err != nil {
						c.recordErr(fmt.Errorf("answer question %s: %w", p.QuestionID, err))
					}
				}()
			}
		}
	})
}

func (c *Collector) recordErr(err error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.firstErr == nil {
		c.firstErr = err
	}
}

func (c *Collector) nextDecision() domain.Decision {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.decisions) == 0 {
		c.defaults++
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

// DefaultsUsed returns how many approval requests were answered by the
// empty-queue fallback instead of a scripted decision. Snapshot
// scenarios expect zero: a scripted approve step must exist for every
// approval the run raises.
func (c *Collector) DefaultsUsed() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.defaults
}

// PendingDecisions returns how many scripted decisions were never
// consumed — an approve step too many in the input script.
func (c *Collector) PendingDecisions() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.decisions)
}

// AssertClean fails on any internal collector failure (bad payloads,
// resolve errors), on approvals answered by the default fallback, or on
// unconsumed scripted decisions — the three ways an input script can
// silently diverge from the run it drives.
func (c *Collector) AssertClean(t *testing.T) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.firstErr != nil {
		t.Fatalf("collector internal error: %v", c.firstErr)
	}
	if c.defaults > 0 {
		t.Fatalf("%d approval(s) answered by the default allow fallback; script the decisions explicitly", c.defaults)
	}
	if len(c.decisions) > 0 {
		t.Fatalf("%d scripted approve decision(s) never consumed", len(c.decisions))
	}
}

// diagnostics renders the state that matters when a wait fails: the
// last turn's own error and the collector's first internal failure.
func (c *Collector) diagnostics() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	s := ""
	if c.lastTurnErr != "" {
		s += fmt.Sprintf("; last turn error: %s", c.lastTurnErr)
	}
	if c.firstErr != nil {
		s += fmt.Sprintf("; collector error: %v", c.firstErr)
	}
	return s
}

// WaitTurn blocks until n turns have finished. It fails fast — with the
// run's diagnostics — if the event stream closes first (dropped
// subscription, shutdown) instead of waiting out the timeout against a
// channel that can never deliver again.
func (c *Collector) WaitTurn(t *testing.T, n int, timeout time.Duration) {
	t.Helper()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for c.TurnsDone() < n {
		select {
		case <-c.turnSig:
		case <-c.done:
			if c.TurnsDone() >= n {
				return
			}
			t.Fatalf("event stream closed waiting for turn %d to finish (done=%d)%s",
				n, c.TurnsDone(), c.diagnostics())
		case <-timer.C:
			t.Fatalf("timed out waiting for turn %d to finish (done=%d)%s",
				n, c.TurnsDone(), c.diagnostics())
		}
	}
}

// AssertSaw fails unless every kind was observed at least once.
func (c *Collector) AssertSaw(t *testing.T, kinds ...runtimeevent.RuntimeEventKind) {
	t.Helper()
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range kinds {
		if c.seen[k] == 0 {
			t.Fatalf("never saw event %q (saw %v)%s", k, c.seen, c.diagnosticsLocked())
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
	t.Fatalf("never saw any of %v (saw %v)%s", kinds, c.seen, c.diagnosticsLocked())
}

func (c *Collector) diagnosticsLocked() string {
	s := ""
	if c.lastTurnErr != "" {
		s += fmt.Sprintf("; last turn error: %s", c.lastTurnErr)
	}
	if c.firstErr != nil {
		s += fmt.Sprintf("; collector error: %v", c.firstErr)
	}
	return s
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
