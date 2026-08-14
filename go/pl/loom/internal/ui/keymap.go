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
	"fmt"
	"sort"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
)

// KeyContext is the UI context a key binding is active in. Contexts are
// coarser than Mode: the three picker modes share ContextPicker.
// See docs/VIM_UI_DESIGN.md §5.
type KeyContext string

const (
	ContextChat   KeyContext = "chat"
	ContextPicker KeyContext = "picker"
)

// Action is a bindable abstract action. Handlers own the action's
// semantics; the binding table only decides which key triggers it.
type Action string

const (
	// ContextChat: global view actions. Structural keys interleaved with
	// composer editing (Enter, Esc, arrows, Tab, Ctrl+C/D) stay hardcoded
	// by design (docs/VIM_UI_DESIGN.md §5.4).
	ActionToggleReasoning  Action = "toggle_reasoning"
	ActionToggleToolOutput Action = "toggle_tool_output"
	ActionToggleAllTools   Action = "toggle_all_tools"
	ActionTogglePlan       Action = "toggle_plan"
	ActionSearchTranscript Action = "search_transcript"
	ActionViewSubagent     Action = "view_subagent"
	ActionCopyLastReply    Action = "copy_last_reply"
	ActionJumpToBottom     Action = "jump_to_bottom"
	ActionPasteImage       Action = "paste_image"
	// ActionQueueFollowup submits the composer draft into the next-turn
	// queue: it runs as its own turn after the busy one, instead of
	// steering into it (deepseek-harness followup semantics).
	ActionQueueFollowup Action = "queue_followup"

	// ContextPicker: keys available in both finder modes (insert and
	// normal). Mode-specific runes (j/k/g/G/q/i in normal mode) are
	// hardcoded vim semantics and stay out of the table.
	ActionCursorUp   Action = "cursor_up"
	ActionCursorDown Action = "cursor_down"
	ActionConfirm    Action = "confirm"
	ActionClose      Action = "close"
)

// defaultBindings lists every bindable action with its default keys, in
// display order. It is the single source of truth for both the default
// keymap and override validation: an action absent here is unknown to
// the configuration layer.
var defaultBindings = map[KeyContext][]struct {
	action Action
	keys   []string
}{
	ContextChat: {
		{ActionToggleReasoning, []string{"ctrl+r"}},
		{ActionToggleToolOutput, []string{"ctrl+e"}},
		{ActionToggleAllTools, []string{"ctrl+o"}},
		{ActionTogglePlan, []string{"ctrl+t"}},
		{ActionSearchTranscript, []string{"ctrl+f"}},
		{ActionViewSubagent, []string{"ctrl+g"}},
		{ActionCopyLastReply, []string{"ctrl+y"}},
		{ActionJumpToBottom, []string{"ctrl+end"}},
		{ActionPasteImage, []string{"ctrl+v"}},
		{ActionQueueFollowup, []string{"ctrl+n"}},
	},
	ContextPicker: {
		{ActionCursorUp, []string{"up", "ctrl+k"}},
		{ActionCursorDown, []string{"down", "ctrl+j"}},
		{ActionConfirm, []string{"enter"}},
		{ActionClose, []string{"esc"}},
	},
}

// Keymap resolves keys to actions per context. The zero value is empty;
// use DefaultKeymap for the built-in bindings.
type Keymap struct {
	// bindings[context][keyString] = action; keyString is bubbletea's
	// canonical key name (tea.KeyMsg.String()).
	bindings map[KeyContext]map[string]Action
}

// DefaultKeymap returns the built-in bindings, which match the
// pre-keymap hardcoded behavior exactly.
func DefaultKeymap() Keymap {
	bindings := make(map[KeyContext]map[string]Action, len(defaultBindings))
	for ctx, entries := range defaultBindings {
		bindings[ctx] = make(map[string]Action)
		for _, e := range entries {
			for _, key := range e.keys {
				bindings[ctx][normalizeKeyName(key)] = e.action
			}
		}
	}
	return Keymap{bindings: bindings}
}

// Lookup reports the action bound to msg in ctx, if any.
func (k Keymap) Lookup(ctx KeyContext, msg tea.KeyMsg) (Action, bool) {
	keys, ok := k.bindings[ctx]
	if !ok {
		return "", false
	}
	action, ok := keys[normalizeKeyName(msg.String())]
	return action, ok
}

// WithOverrides returns a copy of the keymap with user overrides applied
// (docs/VIM_UI_DESIGN.md §5.2). overrides is keyed by context, then
// action, with the replacement key as value. Unknown contexts, unknown
// actions, empty keys, and conflicts with an already-bound key are
// ignored with a warning each; processing is deterministic (sorted by
// context and action) so the same input always yields the same result.
func (k Keymap) WithOverrides(overrides map[string]map[string]string) (Keymap, []string) {
	out := Keymap{bindings: make(map[KeyContext]map[string]Action, len(k.bindings))}
	for ctx, keys := range k.bindings {
		out.bindings[ctx] = make(map[string]Action, len(keys))
		for key, action := range keys {
			out.bindings[ctx][key] = action
		}
	}

	var warnings []string
	for _, ctxName := range sortedKeys(overrides) {
		ctx := KeyContext(ctxName)
		entries, known := defaultBindings[ctx]
		if !known {
			warnings = append(warnings, fmt.Sprintf("unknown keymap context %q (ignored)", ctxName))
			continue
		}
		actions := overrides[ctxName]
		for _, actionName := range sortedKeys(actions) {
			action := Action(actionName)
			if !actionKnown(entries, action) {
				warnings = append(warnings, fmt.Sprintf("unknown keymap action %q in context %q (ignored)", actionName, ctxName))
				continue
			}
			key := normalizeKeyName(actions[actionName])
			if key == "" {
				warnings = append(warnings, fmt.Sprintf("empty key for action %q in context %q (ignored)", actionName, ctxName))
				continue
			}
			table := out.bindings[ctx]
			// A key owned by another action stays with the incumbent; the
			// override is dropped and the action keeps its old binding.
			if owner, taken := table[key]; taken && owner != action {
				warnings = append(warnings, fmt.Sprintf(
					"key %q in context %q already bound to %q; override for %q ignored",
					key, ctxName, owner, actionName,
				))
				continue
			}
			// Detach the action from its previous key(s), then bind the
			// new one.
			for k2, a2 := range table {
				if a2 == action {
					delete(table, k2)
				}
			}
			table[key] = action
		}
	}
	return out, warnings
}

// normalizeKeyName canonicalizes a key name for table lookup. Modifier
// combinations (containing "+") are lowercased so "Ctrl+R" and "ctrl+r"
// are the same binding; single characters keep their case because "Q"
// and "q" are distinct keys.
func normalizeKeyName(key string) string {
	key = strings.TrimSpace(key)
	if strings.Contains(key, "+") {
		return strings.ToLower(key)
	}
	return key
}

func actionKnown(entries []struct {
	action Action
	keys   []string
}, action Action,
) bool {
	for _, e := range entries {
		if e.action == action {
			return true
		}
	}
	return false
}

func sortedKeys[V any](m map[string]V) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
