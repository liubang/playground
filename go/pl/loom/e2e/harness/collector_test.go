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
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Regression: a cleanly finished turn carries NO payload (the controller
// only attaches TurnFinishedPayload on failure), so turn counting must
// not depend on the payload decoding.
func TestCollectorCountsEmptyPayloadTurnFinished(t *testing.T) {
	ch := make(chan runtimeevent.RuntimeEvent, 3)
	ch <- runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnFinished}
	ch <- runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnFinished, Payload: json.RawMessage(`null`)}
	ch <- runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnFinished, Payload: json.RawMessage(`{"error":"boom"}`)}
	close(ch)

	c := NewCollector(context.Background(), nil, ch)
	c.Run() // drains synchronously; the closed channel terminates the loop

	if got := c.TurnsDone(); got != 3 {
		t.Fatalf("TurnsDone = %d, want 3", got)
	}
	if got := c.LastTurnError(); got != "boom" {
		t.Fatalf("LastTurnError = %q, want %q", got, "boom")
	}
	c.AssertClean(t) // empty/null payloads are the clean case, not errors
}

// WaitTurn must wake on the turn signal rather than burn its timeout.
func TestCollectorWaitTurnWakesOnTurn(t *testing.T) {
	ch := make(chan runtimeevent.RuntimeEvent, 1)
	ch <- runtimeevent.RuntimeEvent{Kind: runtimeevent.KindTurnFinished}
	c := NewCollector(context.Background(), nil, ch)
	go c.Run()
	start := time.Now()
	c.WaitTurn(t, 1, 30*time.Second)
	if elapsed := time.Since(start); elapsed > 5*time.Second {
		t.Fatalf("WaitTurn took %v; it should wake on the turn signal", elapsed)
	}
}
