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
// Created: 2026/07/31

package ui

import (
	"testing"

	tea "github.com/charmbracelet/bubbletea"
)

func TestDefaultKeymapLookup(t *testing.T) {
	km := DefaultKeymap()
	cases := []struct {
		ctx  KeyContext
		key  tea.KeyMsg
		want Action
	}{
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlR}, ActionToggleReasoning},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlE}, ActionToggleToolOutput},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlO}, ActionToggleAllTools},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlT}, ActionTogglePlan},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlF}, ActionSearchTranscript},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlG}, ActionViewSubagent},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlY}, ActionCopyLastReply},
		{ContextChat, tea.KeyMsg{Type: tea.KeyCtrlEnd}, ActionJumpToBottom},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyUp}, ActionCursorUp},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyDown}, ActionCursorDown},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyCtrlK}, ActionCursorUp},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyCtrlJ}, ActionCursorDown},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyEnter}, ActionConfirm},
		{ContextPicker, tea.KeyMsg{Type: tea.KeyEsc}, ActionClose},
	}
	for _, c := range cases {
		got, ok := km.Lookup(c.ctx, c.key)
		if !ok || got != c.want {
			t.Errorf("Lookup(%s, %q) = (%q, %v), want (%q, true)", c.ctx, c.key.String(), got, ok, c.want)
		}
	}
}

func TestDefaultKeymapLeavesStructuralKeysUnbound(t *testing.T) {
	km := DefaultKeymap()
	// Structural keys (composer editing, cancel/quit) must stay out of
	// the bindable table (docs/VIM_UI_DESIGN.md §5.4).
	for _, key := range []tea.KeyMsg{
		{Type: tea.KeyCtrlC},
		{Type: tea.KeyCtrlD},
		{Type: tea.KeyEnter},
		{Type: tea.KeyEsc},
		{Type: tea.KeyTab},
		{Type: tea.KeyRunes, Runes: []rune{'j'}},
	} {
		if action, ok := km.Lookup(ContextChat, key); ok {
			t.Errorf("Lookup(chat, %q) = %q, want unbound", key.String(), action)
		}
	}
}

func TestKeymapOverrideRebinds(t *testing.T) {
	km := DefaultKeymap()
	km, warnings := km.WithOverrides(map[string]map[string]string{
		"chat": {"search_transcript": "ctrl+s"},
	})
	if len(warnings) != 0 {
		t.Fatalf("warnings = %v, want none", warnings)
	}
	// The new key fires the action; the old key is detached.
	if got, ok := km.Lookup(ContextChat, tea.KeyMsg{Type: tea.KeyCtrlS}); !ok || got != ActionSearchTranscript {
		t.Fatalf("Lookup(chat, ctrl+s) = (%q, %v), want search_transcript", got, ok)
	}
	if _, ok := km.Lookup(ContextChat, tea.KeyMsg{Type: tea.KeyCtrlF}); ok {
		t.Fatal("ctrl+f should be detached after the override")
	}
}

func TestKeymapOverrideUnknownEntries(t *testing.T) {
	km := DefaultKeymap()
	km2, warnings := km.WithOverrides(map[string]map[string]string{
		"nosuch": {"anything": "ctrl+x"},
		"chat":   {"nosuch_action": "ctrl+x", "toggle_plan": ""},
	})
	if len(warnings) != 3 {
		t.Fatalf("warnings = %v, want 3", warnings)
	}
	// Every invalid entry was ignored: bindings are unchanged.
	for _, c := range []struct {
		key  tea.KeyMsg
		want Action
	}{
		{tea.KeyMsg{Type: tea.KeyCtrlF}, ActionSearchTranscript},
		{tea.KeyMsg{Type: tea.KeyCtrlT}, ActionTogglePlan},
	} {
		if got, ok := km2.Lookup(ContextChat, c.key); !ok || got != c.want {
			t.Errorf("Lookup(chat, %q) = (%q, %v), want (%q, true)", c.key.String(), got, ok, c.want)
		}
	}
}

func TestKeymapOverrideConflict(t *testing.T) {
	km := DefaultKeymap()
	km, warnings := km.WithOverrides(map[string]map[string]string{
		"chat": {"toggle_plan": "ctrl+r"}, // owned by toggle_reasoning
	})
	if len(warnings) != 1 {
		t.Fatalf("warnings = %v, want 1 conflict", warnings)
	}
	// The incumbent keeps the key; the loser keeps its old binding.
	if got, _ := km.Lookup(ContextChat, tea.KeyMsg{Type: tea.KeyCtrlR}); got != ActionToggleReasoning {
		t.Fatalf("ctrl+r = %q, want toggle_reasoning", got)
	}
	if got, _ := km.Lookup(ContextChat, tea.KeyMsg{Type: tea.KeyCtrlT}); got != ActionTogglePlan {
		t.Fatalf("ctrl+t = %q, want toggle_plan", got)
	}
}

func TestKeymapOverrideDeterministic(t *testing.T) {
	overrides := map[string]map[string]string{
		"chat": {
			"toggle_plan":       "ctrl+r", // conflicts with toggle_reasoning
			"toggle_reasoning":  "ctrl+x",
			"search_transcript": "ctrl+s",
		},
	}
	first, w1 := DefaultKeymap().WithOverrides(overrides)
	for i := 0; i < 10; i++ {
		again, w2 := DefaultKeymap().WithOverrides(overrides)
		if len(w1) != len(w2) {
			t.Fatalf("warning count differs across runs: %d vs %d", len(w1), len(w2))
		}
		for ctx, keys := range first.bindings {
			for key, action := range keys {
				if again.bindings[ctx][key] != action {
					t.Fatalf("binding %s/%q differs across runs: %q vs %q",
						ctx, key, action, again.bindings[ctx][key])
				}
			}
		}
	}
	// Sorted processing: toggle_plan loses ctrl+r (conflict, toggle_plan
	// sorts before toggle_reasoning), then toggle_reasoning moves to
	// ctrl+x — the exact outcome is deterministic either way.
	if got, _ := first.Lookup(ContextChat, tea.KeyMsg{Type: tea.KeyCtrlX}); got != ActionToggleReasoning {
		t.Fatalf("ctrl+x = %q, want toggle_reasoning", got)
	}
}

func TestNormalizeKeyName(t *testing.T) {
	cases := map[string]string{
		"Ctrl+R":    "ctrl+r",
		" ctrl+f ":  "ctrl+f",
		"Shift+Tab": "shift+tab",
		"Q":         "Q", // single characters keep case: Q and q differ
		"q":         "q",
	}
	for in, want := range cases {
		if got := normalizeKeyName(in); got != want {
			t.Errorf("normalizeKeyName(%q) = %q, want %q", in, got, want)
		}
	}
}
