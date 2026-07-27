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

package ui

import (
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// Regression: a rate-limit failure surfaces a giant single-line provider
// error JSON. Rendered unbounded, it floods the transcript, pushes the
// conversation out of view and prompts the user to scroll away from the
// tail — the first step towards "replies appear one message late". The
// notice must keep only the informative head; the full error stays in the
// event log.
func TestTurnFinishedErrorNoticeIsBounded(t *testing.T) {
	idx := NewBlockIndex()
	longError := `model stream consumption: {"error":{"message":"` + strings.Repeat("x", 2000) + `"}}`
	payload := mustPayload(t, runtimeevent.TurnFinishedPayload{Error: longError})

	id := ApplyRuntimeEvent(idx, runtimeevent.RuntimeEvent{Sequence: 9, Kind: runtimeevent.KindTurnFinished, Payload: payload})
	if id == "" {
		t.Fatal("error turn finished did not produce a notice block")
	}
	block, _ := idx.Get(id)
	const prefix = "Turn ended with error: "
	if !strings.HasPrefix(block.Content, prefix) {
		t.Fatalf("notice lost its prefix: %q", block.Content)
	}
	if got := len(block.Content) - len(prefix); got > turnErrorMaxCells+3 { // +3 for the ellipsis
		t.Fatalf("notice error text = %d chars, want <= %d: %q", got, turnErrorMaxCells+3, block.Content)
	}
	if !strings.HasSuffix(block.Content, "...") {
		t.Fatalf("truncated notice must end with an ellipsis: %q", block.Content)
	}
}

// Regression: with the viewport only a couple of lines above the tail, an
// incoming block must pull the view back to the bottom (near-bottom
// magnetism). Otherwise an accidental ↑/wheel nudge hides freshly streamed
// replies below the fold; the window then looks identical to "no reply yet"
// and the next prompt appears to answer the previous message.
func TestNewBlockNearBottomSnapsBackToTail(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 20
	m.layout()

	// Overflow the viewport so scrolling has somewhere to go.
	for i := 0; i < 30; i++ {
		payload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "line\n"})
		ApplyRuntimeEvent(m.blocks, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: payload})
	}
	m.syncTranscript()
	if m.viewport.TotalLineCount() <= m.viewport.Height {
		t.Fatalf("setup: content %d lines must overflow viewport height %d", m.viewport.TotalLineCount(), m.viewport.Height)
	}

	// The user nudges two lines up (accidental wheel/↑ at the composer edge).
	m.pauseFollowTail()
	m.viewport.LineUp(2)

	// A new streamed block arrives: the view must snap back to the tail
	// instead of counting an unseen event.
	updated, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{
		Turn:    2,
		Kind:    runtimeevent.KindModelTextDelta,
		Payload: mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "fresh"}),
	})
	m = updated
	if !m.followTail {
		t.Fatal("near-bottom new block must re-engage follow tail")
	}
	if m.newEvents != 0 {
		t.Fatalf("near-bottom new block must not count as unseen, got %d", m.newEvents)
	}
}

// Regression: a deliberate (far) scroll keeps the user's reading position,
// but the status bar must now say — unconditionally — that the view is
// above the tail. Previously the indicator existed only for events that
// arrived while already scrolled (newEvents>0), so scrolling up after a
// reply had landed left zero trace that more content sat below the fold.
func TestScrolledAwayFromTailShowsIndicator(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 120, 20
	m.layout()

	for i := 0; i < 30; i++ {
		payload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "line\n"})
		ApplyRuntimeEvent(m.blocks, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: payload})
	}
	m.syncTranscript()

	m.pauseFollowTail()
	m.viewport.LineUp(10)
	m.newEvents = 0 // no events arrived while scrolled

	bar := m.renderStatusBar()
	if !strings.Contains(bar, "ctrl+end for latest") {
		t.Fatalf("status bar must flag the scrolled-away state, got %q", bar)
	}

	// Back at the bottom the indicator disappears.
	m.resumeFollowTail()
	m.syncTranscript()
	bar = m.renderStatusBar()
	if strings.Contains(bar, "ctrl+end for latest") {
		t.Fatalf("status bar must clear the indicator at the tail, got %q", bar)
	}
}

// The far-scroll case: while deliberately scrolled up, a streamed reply
// lands below the visible window; submitting the next prompt snaps to the
// tail and reveals it. This is the exact perception reported as "I send a
// message, then I receive the previous message's reply": the data was never
// lost, only hidden below the fold with too weak an indicator.
func TestReplyBelowFoldBecomesVisibleOnNextSubmit(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 20
	m.layout()

	// Overflow the viewport, then scroll far up (deliberate read).
	for i := 0; i < 30; i++ {
		payload := mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "line\n"})
		ApplyRuntimeEvent(m.blocks, runtimeevent.RuntimeEvent{Turn: 1, Kind: runtimeevent.KindModelTextDelta, Payload: payload})
	}
	m.syncTranscript()
	m.pauseFollowTail()
	m.viewport.LineUp(15)

	// The reply streams in below the fold; it must not yank a deliberate
	// far scroll back, and it must be counted as unseen.
	updated, _ := m.handleRuntimeEvent(runtimeevent.RuntimeEvent{
		Turn:    2,
		Kind:    runtimeevent.KindModelTextDelta,
		Payload: mustPayload(t, runtimeevent.ModelTextDeltaPayload{Delta: "fresh reply"}),
	})
	m = updated
	if m.followTail {
		t.Fatal("far scroll must keep the reading position (no snap)")
	}
	if m.newEvents == 0 {
		t.Fatal("below-fold reply must count as a new unseen event")
	}
	m.syncTranscript()
	if strings.Contains(m.viewport.View(), "fresh reply") {
		t.Fatal("below-fold reply must stay out of the visible window")
	}

	// Submitting the next prompt re-engages follow tail: the reply appears.
	m.resumeFollowTail()
	m.syncTranscript()
	if !strings.Contains(m.viewport.View(), "fresh reply") {
		t.Fatal("next submit must reveal the reply that was below the fold")
	}
}
