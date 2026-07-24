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

	tea "github.com/charmbracelet/bubbletea"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

// fillTranscript pushes enough turns into the model to overflow the viewport.
func fillTranscript(t *testing.T, m Model, turns int) Model {
	t.Helper()
	for i := 0; i < turns; i++ {
		payload := mustPayload(t, runtimeevent.TurnStartedPayload{TurnIndex: i, Prompt: strings.Repeat("line ", 8)})
		updated, _ := m.Update(runtimeEventMsg(runtimeevent.RuntimeEvent{Sequence: uint64(i + 1), Kind: runtimeevent.KindTurnStarted, Payload: payload}))
		m = updated.(Model)
	}
	return m
}

// The Update-side viewport must hold real content; otherwise every scroll
// operation (keyboard or mouse wheel) is a no-op on an empty viewport.
func TestScrollingWorksAfterLayout(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 20
	m = fillTranscript(t, m, 50)

	if !m.followTail {
		t.Fatal("expected follow-tail initially")
	}
	if m.viewport.YOffset <= 0 {
		t.Fatalf("follow-tail did not pin viewport to bottom: YOffset=%d", m.viewport.YOffset)
	}
}

func TestKeyboardScrollingMovesViewport(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 20
	m = fillTranscript(t, m, 50)
	bottom := m.viewport.YOffset

	updated, _ := m.Update(tea.KeyMsg{Type: tea.KeyPgUp})
	m = updated.(Model)
	if m.followTail {
		t.Fatal("PgUp did not pause follow-tail")
	}
	if m.viewport.YOffset >= bottom {
		t.Fatalf("PgUp did not scroll up: YOffset=%d (bottom=%d)", m.viewport.YOffset, bottom)
	}

	updated, _ = m.Update(tea.KeyMsg{Type: tea.KeyCtrlEnd})
	m = updated.(Model)
	if !m.followTail || m.viewport.YOffset != bottom {
		t.Fatalf("Ctrl+End did not return to bottom: YOffset=%d follow=%v", m.viewport.YOffset, m.followTail)
	}
}

func TestMouseWheelScrollsTranscript(t *testing.T) {
	ctrl := newTestController(t)
	m := NewModel(ctrl, "model", "/ws")
	m.width, m.height = 80, 20
	m = fillTranscript(t, m, 50)
	bottom := m.viewport.YOffset

	// Wheel up pauses follow-tail and actually moves the viewport.
	updated, _ := m.Update(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonWheelUp})
	m = updated.(Model)
	if m.followTail {
		t.Fatal("wheel up did not pause follow-tail")
	}
	if m.viewport.YOffset >= bottom {
		t.Fatalf("wheel up did not scroll: YOffset=%d (bottom=%d)", m.viewport.YOffset, bottom)
	}

	// Wheeling back to the bottom resumes follow-tail.
	for i := 0; i < 20; i++ {
		updated, _ = m.Update(tea.MouseMsg{Action: tea.MouseActionPress, Button: tea.MouseButtonWheelDown})
		m = updated.(Model)
	}
	if !m.followTail || m.viewport.YOffset != bottom {
		t.Fatalf("wheel down to bottom did not resume follow-tail: YOffset=%d follow=%v", m.viewport.YOffset, m.followTail)
	}
}
