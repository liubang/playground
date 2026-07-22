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
// Created: 2026/07/22 21:10

package domain

import (
	"strings"
	"testing"
)

func TestNewSessionID(t *testing.T) {
	id := NewSessionID()
	if id.IsZero() {
		t.Fatal("expected non-zero session ID")
	}
	if !strings.HasPrefix(id.String(), "sess_") {
		t.Fatalf("expected prefix sess_, got %s", id.String())
	}
}

func TestNewRunID(t *testing.T) {
	id := NewRunID()
	if id.IsZero() {
		t.Fatal("expected non-zero run ID")
	}
	if !strings.HasPrefix(id.String(), "run_") {
		t.Fatalf("expected prefix run_, got %s", id.String())
	}
}

func TestNewEventID(t *testing.T) {
	id := NewEventID()
	if id.IsZero() {
		t.Fatal("expected non-zero event ID")
	}
	if !strings.HasPrefix(id.String(), "evt_") {
		t.Fatalf("expected prefix evt_, got %s", id.String())
	}
}

func TestParseIDRoundTrip(t *testing.T) {
	orig := NewSessionID()
	s := orig.String()
	parsed, err := ParseSessionID(s)
	if err != nil {
		t.Fatalf("ParseID error: %v", err)
	}
	if parsed.String() != s {
		t.Fatalf("round-trip mismatch: %s vs %s", s, parsed.String())
	}
}

func TestParseIDEmpty(t *testing.T) {
	_, err := ParseSessionID("")
	if err == nil {
		t.Fatal("expected error for empty ID")
	}
}

func TestIDUniqueness(t *testing.T) {
	ids := make(map[string]bool)
	for range 100 {
		id := NewEventID()
		if ids[id.String()] {
			t.Fatalf("duplicate ID: %s", id.String())
		}
		ids[id.String()] = true
	}
}

func TestAllIDPrefixes(t *testing.T) {
	cases := []struct {
		name   string
		id     string
		prefix string
	}{
		{"session", NewSessionID().String(), "sess_"},
		{"run", NewRunID().String(), "run_"},
		{"turn", NewTurnID().String(), "turn_"},
		{"message", NewMessageID().String(), "msg_"},
		{"tool_call", NewToolCallID().String(), "tc_"},
		{"event", NewEventID().String(), "evt_"},
		{"artifact", NewArtifactID().String(), "art_"},
		{"checkpoint", NewCheckpointID().String(), "ckpt_"},
	}
	for _, c := range cases {
		if !strings.HasPrefix(c.id, c.prefix) {
			t.Errorf("%s: expected prefix %s, got %s", c.name, c.prefix, c.id)
		}
	}
}
