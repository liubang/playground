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
// Created: 2026/08/02

package subagent

import (
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestParseRole(t *testing.T) {
	for _, tc := range []struct {
		input string
		want  Role
		err   bool
	}{
		{"", RoleResearcher, false},
		{"researcher", RoleResearcher, false},
		{"coder", RoleCoder, false},
		{"RESEARCHER", "", true}, // case-sensitive
		{"admin", "", true},
		{"planner", "", true},
	} {
		got, err := ParseRole(tc.input)
		if tc.err {
			if err == nil {
				t.Fatalf("ParseRole(%q): expected error, got %q", tc.input, got)
			}
			continue
		}
		if err != nil {
			t.Fatalf("ParseRole(%q): unexpected error: %v", tc.input, err)
		}
		if got != tc.want {
			t.Fatalf("ParseRole(%q) = %q, want %q", tc.input, got, tc.want)
		}
	}
}

func TestRiskOf(t *testing.T) {
	if riskOf(RoleResearcher) != domain.R1 {
		t.Fatalf("researcher risk = %d, want R1", riskOf(RoleResearcher))
	}
	if riskOf(RoleCoder) != domain.R3 {
		t.Fatalf("coder risk = %d, want R3", riskOf(RoleCoder))
	}
	// Unknown role defaults to R1 (the safest tier).
	if riskOf("unknown") != domain.R1 {
		t.Fatalf("unknown role risk = %d, want R1", riskOf("unknown"))
	}
}

func TestRoleInstructions(t *testing.T) {
	if RoleResearcher.Instructions() != ResearcherInstructions {
		t.Fatalf("researcher instructions mismatch")
	}
	if RoleCoder.Instructions() != CoderInstructions {
		t.Fatalf("coder instructions mismatch")
	}
	// Unknown role falls back to researcher.
	r := Role("unknown")
	if r.Instructions() != ResearcherInstructions {
		t.Fatalf("unknown role instructions should default to researcher")
	}
}

func TestRoleOf(t *testing.T) {
	// No events → empty role.
	if roleOf(nil) != "" {
		t.Fatalf("roleOf(nil) = %q, want empty", roleOf(nil))
	}
	if roleOf([]domain.Event{}) != "" {
		t.Fatalf("roleOf([]) = %q, want empty", roleOf([]domain.Event{}))
	}

	// Non-delegation event → empty.
	events := []domain.Event{
		{Type: domain.EventRunCreated, Payload: []byte(`{"delegated":false}`)},
	}
	if roleOf(events) != "" {
		t.Fatalf("roleOf(non-delegated) = %q, want empty", roleOf(events))
	}

	// Delegated event with role → that role.
	events = []domain.Event{
		{Type: domain.EventRunCreated, Payload: []byte(`{"delegated":true,"role":"coder"}`)},
	}
	if got := roleOf(events); got != RoleCoder {
		t.Fatalf("roleOf(delegated+coder) = %q, want coder", got)
	}

	// Delegated event without role → empty (V1 session).
	events = []domain.Event{
		{Type: domain.EventRunCreated, Payload: []byte(`{"delegated":true}`)},
	}
	if got := roleOf(events); got != "" {
		t.Fatalf("roleOf(delegated-no-role) = %q, want empty", got)
	}

	// Invalid JSON → skipped, empty role.
	events = []domain.Event{
		{Type: domain.EventRunCreated, Payload: []byte(`not-json`)},
	}
	if got := roleOf(events); got != "" {
		t.Fatalf("roleOf(invalid-json) = %q, want empty", got)
	}
}
