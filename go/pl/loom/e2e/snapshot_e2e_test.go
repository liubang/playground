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
// Created: 2026/08/15

package e2e

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/e2e/harness"
	"github.com/liubang/playground/go/pl/loom/internal/client"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// snapshot_e2e_test.go is the snapshot suite runner
// (docs/REPLAY_TESTING_DESIGN.md §5): each scenario under
// testdata/snapshots/<name>/ drives the full serve path through its
// input.json step script, and the run's normalized runtime event stream
// plus persisted transcripts are diffed against the committed golden
// files. Three modes:
//
//	replay  (default, keyless): the model replays calls.*.jsonl;
//	        goldens are compared.
//	record  (LOOM_SNAPSHOT=record, needs LOOM_E2E_LLM=1): real provider
//	        calls are recorded into calls.*.jsonl and the goldens are
//	        rewritten — recording IS the real-model acceptance run.
//	refresh (LOOM_SNAPSHOT=refresh): replay run, goldens rewritten —
//	        the fast path after a legitimate prompt-text tweak.

const snapshotsDir = "testdata/snapshots"

// inputScript is a scenario's input.json: the deterministic drive steps.
type inputScript struct {
	Steps []inputStep `json:"steps"`
}

type inputStep struct {
	Op       string `json:"op"`                 // prompt | wait_turn_end | approve | cancel | compact
	Text     string `json:"text,omitempty"`     // prompt
	Decision string `json:"decision,omitempty"` // approve: allow | deny
}

func TestSnapshotScenarios(t *testing.T) {
	mode := os.Getenv("LOOM_SNAPSHOT")
	switch mode {
	case "", "replay":
		mode = "replay"
	case "record", "refresh":
	default:
		t.Fatalf("LOOM_SNAPSHOT = %q, want record|refresh|replay", mode)
	}

	entries, err := os.ReadDir(snapshotsDir)
	if err != nil {
		if os.IsNotExist(err) {
			t.Skip("no snapshot scenarios recorded yet")
		}
		t.Fatalf("read snapshots dir: %v", err)
	}
	scenarios := 0
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		scenarios++
		name := entry.Name()
		t.Run(name, func(t *testing.T) {
			runSnapshotScenario(t, filepath.Join(snapshotsDir, name), mode)
		})
	}
	if scenarios == 0 {
		t.Skip("no snapshot scenarios recorded yet")
	}
}

func runSnapshotScenario(t *testing.T, dir string, mode string) {
	t.Helper()
	ctx := t.Context()

	raw, err := os.ReadFile(filepath.Join(dir, "input.json"))
	if err != nil {
		t.Fatalf("read input.json: %v", err)
	}
	var input inputScript
	if err := json.Unmarshal(raw, &input); err != nil {
		t.Fatalf("parse input.json: %v", err)
	}

	var env *harness.Env
	if mode == "record" {
		env = harness.NewEnv(t, harness.WithRecording(dir))
	} else {
		env = harness.NewEnv(t, harness.WithReplay(dir))
	}
	// Seed the workspace from the committed fixture, if any.
	seedDir := filepath.Join(dir, "workspace")
	if info, err := os.Stat(seedDir); err == nil && info.IsDir() {
		copyTree(t, seedDir, env.Workspace)
	}

	c := env.NewClient(t)
	collector := harness.NewCollector(c, env.Subscribe(t, c))
	go collector.Run()

	turns := 0
	for i, step := range input.Steps {
		switch step.Op {
		case "prompt":
			if _, err := c.SubmitPrompt(ctx, step.Text, nil); err != nil {
				t.Fatalf("step %d (prompt): %v", i, err)
			}
		case "wait_turn_end":
			turns++
			collector.WaitTurn(t, turns, 5*time.Minute)
		case "approve":
			decision, err := parseDecision(step.Decision)
			if err != nil {
				t.Fatalf("step %d: %v", i, err)
			}
			collector.EnqueueDecision(decision)
		case "cancel":
			if err := c.CancelTurn(ctx); err != nil {
				t.Fatalf("step %d (cancel): %v", i, err)
			}
		case "compact":
			if _, err := c.RequestCompaction(ctx); err != nil {
				t.Fatalf("step %d (compact): %v", i, err)
			}
		default:
			t.Fatalf("step %d: unknown op %q", i, step.Op)
		}
	}

	// Replay must have consumed exactly the recorded calls — no more
	// (script exhausted fails in-call), no less.
	if mode != "record" {
		if err := env.ReplayModel().AssertConsumed(); err != nil {
			t.Fatalf("replay consumption: %v", err)
		}
	}

	normCtx := harness.NormalizeContext{
		Workspace:   env.Workspace,
		ArtifactDir: env.ArtifactDir,
		Home:        env.Home,
	}
	eventsGolden := harness.NormalizeEvents(normCtx, env.TapEvents())
	transcriptGolden := harness.NormalizeTranscripts(normCtx, inspectAllSessions(t, env, c))

	assertGolden(t, dir, "events.expected.jsonl", eventsGolden, mode == "replay")
	assertGolden(t, dir, "transcript.expected.json", transcriptGolden, mode == "replay")
}

// inspectAllSessions collects the persisted inspection of every session
// the run produced, root first then sub-agents by creation order, so the
// transcript golden covers delegated sessions too.
func inspectAllSessions(t *testing.T, env *harness.Env, c client.Client) []domain.SessionInspection {
	t.Helper()
	summaries, err := c.ListSessions(env.Ctx, 100)
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	sort.SliceStable(summaries, func(i, j int) bool {
		iRoot := summaries[i].ParentSessionID == ""
		jRoot := summaries[j].ParentSessionID == ""
		if iRoot != jRoot {
			return iRoot
		}
		return summaries[i].CreatedAt.Before(summaries[j].CreatedAt)
	})
	store := env.OpenStoreReadOnly(t)
	out := make([]domain.SessionInspection, 0, len(summaries))
	for _, summary := range summaries {
		insp, err := store.InspectSession(env.Ctx, summary.ID)
		if err != nil {
			t.Fatalf("InspectSession(%s): %v", summary.ID, err)
		}
		out = append(out, insp)
	}
	return out
}

func parseDecision(s string) (domain.Decision, error) {
	switch domain.Decision(s) {
	case domain.DecisionAllow:
		return domain.DecisionAllow, nil
	case domain.DecisionDeny:
		return domain.DecisionDeny, nil
	default:
		return "", fmt.Errorf("unknown approve decision %q (want allow|deny)", s)
	}
}

// assertGolden compares (replay) or rewrites (record/refresh) one golden
// file.
func assertGolden(t *testing.T, dir, name, actual string, compare bool) {
	t.Helper()
	path := filepath.Join(dir, name)
	if !compare {
		if err := os.WriteFile(path, []byte(actual), 0o600); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
		t.Logf("%s: golden rewritten (%d bytes)", name, len(actual))
		return
	}
	expected, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s (run LOOM_SNAPSHOT=record first): %v", name, err)
	}
	if string(expected) != actual {
		t.Fatalf("%s mismatch (LOOM_SNAPSHOT=refresh to accept):\n%s", name, unifiedDiff(string(expected), actual))
	}
}

// unifiedDiff renders a minimal line diff for golden mismatches.
func unifiedDiff(expected, actual string) string {
	expLines := strings.Split(expected, "\n")
	actLines := strings.Split(actual, "\n")
	var sb strings.Builder
	max := len(expLines)
	if len(actLines) > max {
		max = len(actLines)
	}
	shown := 0
	for i := 0; i < max && shown < 40; i++ {
		var exp, act string
		if i < len(expLines) {
			exp = expLines[i]
		}
		if i < len(actLines) {
			act = actLines[i]
		}
		if exp != act {
			sb.WriteString(fmt.Sprintf("line %d:\n- %s\n+ %s\n", i+1, truncate(exp, 400), truncate(act, 400)))
			shown++
		}
	}
	if shown == 0 {
		sb.WriteString("(files differ only in trailing whitespace)\n")
	}
	return sb.String()
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

// copyTree recursively copies dir contents into dest.
func copyTree(t *testing.T, src, dest string) {
	t.Helper()
	entries, err := os.ReadDir(src)
	if err != nil {
		t.Fatalf("read seed dir: %v", err)
	}
	for _, entry := range entries {
		from := filepath.Join(src, entry.Name())
		to := filepath.Join(dest, entry.Name())
		if entry.IsDir() {
			if err := os.MkdirAll(to, 0o755); err != nil {
				t.Fatalf("mkdir %s: %v", to, err)
			}
			copyTree(t, from, to)
			continue
		}
		data, err := os.ReadFile(from)
		if err != nil {
			t.Fatalf("read %s: %v", from, err)
		}
		if err := os.WriteFile(to, data, 0o600); err != nil {
			t.Fatalf("write %s: %v", to, err)
		}
	}
}
