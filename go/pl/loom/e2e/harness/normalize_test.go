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
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
)

func testNormCtx() NormalizeContext {
	return NormalizeContext{
		Workspace:   "/tmp/loom-test/ws",
		ArtifactDir: "/tmp/loom-test/artifacts",
		Home:        "/tmp/loom-test",
	}
}

func TestNormalizeEventsVolatileFields(t *testing.T) {
	sessID := domain.NewSessionID()
	runID := domain.NewRunID()
	events := []runtimeevent.RuntimeEvent{
		{
			Version: 1, Sequence: 41, SessionID: sessID, RunID: runID, Turn: 1,
			Kind: runtimeevent.KindTurnStarted, Time: time.Date(2026, 8, 15, 1, 2, 3, 0, time.UTC),
			Durable: true,
			Payload: json.RawMessage(`{"prompt":"read /tmp/loom-test/ws/a.txt"}`),
		},
		{
			Version: 1, Sequence: 42, SessionID: sessID, RunID: runID, Turn: 1,
			Kind: runtimeevent.KindUsageUpdated, Time: time.Now(),
			Durable: true,
			Payload: json.RawMessage(`{"input_tokens":1234,"cached":1000}`),
		},
		{
			// Ephemeral deltas never enter the golden.
			Version: 1, Sequence: 43, SessionID: sessID, Kind: runtimeevent.KindModelTextDelta,
			Payload: json.RawMessage(`{"text":"hi"}`),
		},
	}
	out := NormalizeEvents(testNormCtx(), events)
	lines := strings.Split(strings.TrimSpace(out), "\n")
	if len(lines) != 2 {
		t.Fatalf("lines = %d, want 2:\n%s", len(lines), out)
	}
	// The same raw ID maps to the same token across both events. Within
	// one object the ordinals follow sorted key order (run_id < session_id).
	if !strings.Contains(lines[0], `"run_id":"{{id:1}}"`) || !strings.Contains(lines[0], `"session_id":"{{id:2}}"`) {
		t.Fatalf("IDs not tokenized in first-appearance order: %s", lines[0])
	}
	if !strings.Contains(lines[1], `"session_id":"{{id:2}}"`) {
		t.Fatalf("repeat occurrence must reuse the same token: %s", lines[1])
	}
	// Broker sequence + timestamps zero; usage numbers survive.
	if !strings.Contains(lines[0], `"sequence":0`) || !strings.Contains(lines[0], `"time":0`) {
		t.Fatalf("volatile scalars not zeroed: %s", lines[0])
	}
	if !strings.Contains(lines[1], `"input_tokens":1234`) {
		t.Fatalf("usage numbers must survive: %s", lines[1])
	}
	// Workspace path scrubbed inside a payload string.
	if !strings.Contains(lines[0], `{{cwd}}/a.txt`) {
		t.Fatalf("workspace path not scrubbed: %s", lines[0])
	}
}

func TestNormalizePathBoundaries(t *testing.T) {
	n := newNormalizer(NormalizeContext{Workspace: "/tmp/ws"})
	// A sibling directory sharing the prefix must NOT be rewritten.
	if got := n.scrubString("/tmp/ws2/file"); got != "/tmp/ws2/file" {
		t.Fatalf("boundary violation: %q", got)
	}
	if got := n.scrubString("cat /tmp/ws/file"); got != "cat {{cwd}}/file" {
		t.Fatalf("embedded path not scrubbed: %q", got)
	}
	if got := n.scrubString("/tmp/ws"); got != "{{cwd}}" {
		t.Fatalf("bare root not scrubbed: %q", got)
	}
}

func TestNormalizeLongestPathWins(t *testing.T) {
	n := newNormalizer(NormalizeContext{Workspace: "/tmp/h/ws", Home: "/tmp/h"})
	if got := n.scrubString("/tmp/h/ws/a"); got != "{{cwd}}/a" {
		t.Fatalf("nested root lost to its ancestor: %q", got)
	}
	if got := n.scrubString("/tmp/h/other"); got != "{{home}}/other" {
		t.Fatalf("home root not applied: %q", got)
	}
}

// Regression: the scrubString ID/hash fast paths must never skip
// replay.ScrubVolatileText — its date line is 28 chars (below the ID
// minimum) and carries no underscore (the embedded-ID gate).
func TestScrubStringKeepsVolatileTextScrubBelowIDMinLen(t *testing.T) {
	n := newNormalizer(NormalizeContext{})
	if got := n.scrubString("Current date: 2026-08-15 UTC"); got != "Current date: {{date}}" {
		t.Fatalf("volatile date line not scrubbed: %q", got)
	}
	if got := n.scrubString("Platform: darwin/arm64, Shell: /bin/zsh"); got != "Platform: {{platform}}, Shell: {{shell}}" {
		t.Fatalf("platform/shell line not scrubbed: %q", got)
	}
}

func TestNormalizeTranscriptsKeepsPerSessionSequence(t *testing.T) {
	insp := domain.SessionInspection{
		Session: domain.SessionSummary{ID: domain.NewSessionID()},
		Transcript: domain.SessionTranscript{
			SessionID: domain.NewSessionID(),
			Messages: []domain.Message{{
				ID: domain.NewMessageID(), Sequence: 3, Role: domain.RoleUser,
				Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: "hi"}},
				CreatedAt: time.Now(),
			}},
			LastEventSequence: 9,
		},
	}
	out := NormalizeTranscripts(testNormCtx(), []domain.SessionInspection{insp})
	if !strings.Contains(out, `"sequence": 3`) || !strings.Contains(out, `"last_event_sequence": 9`) {
		t.Fatalf("per-session sequences must survive:\n%s", out)
	}
	if !strings.Contains(out, `"created_at": 0`) {
		t.Fatalf("timestamps must zero:\n%s", out)
	}
	if strings.Contains(out, "msg_") || strings.Contains(out, "sess_") {
		t.Fatalf("raw IDs leaked:\n%s", out)
	}
}
