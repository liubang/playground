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
// Created: 2026/09/30

package agent

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/memory"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/exsession"
	"github.com/liubang/playground/go/pl/loom/internal/tool/skillread"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// The canonical contract every tool must satisfy, pinned by three
// production failures that were all the same bug in different clothes:
//
//  1. The signed canonical arguments' top-level fields must be a subset of
//     the model-visible InputSchema properties (plus an explicit per-tool
//     allowlist of internal fields). The transcript rewrite projects the
//     canonical form onto the schema (modelFacingCanonicalArgs); anything
//     outside it is silently dropped there —
//     sess_f59104313f866e60289409f10a4af59a (write's created/old_hash),
//     sess_eb40ddfc64b734371efe224695f6beeb (update_task's ignored_fields).
//
//  2. Re-Preparing the projected form must reproduce the signed canonical
//     form byte-for-byte. verifyPreparedFreshness replays exactly that
//     shape; a divergence fails the call closed as a security error —
//     sess_2e5c16c02e285ee4e608d0b136e7d4e9 (run_cmd's max_output_bytes
//     folded at Prepare, lost by the projection), the memory add_note
//     timestamp minted at Prepare (never survived an approval that
//     crossed a second boundary).
//
//  3. Re-Preparing the signed canonical form itself must succeed and
//     reproduce it: the agent loop's repaired-arguments path replays
//     exactly that (run.go freshnessOriginal = prepared.Call), so a tool
//     whose strict decode rejects its own canonical fields (write's
//     created/old_hash before the tolerate-on-decode fix) bounces every
//     repaired call.
//
// Tools whose Prepare binds environment state into the canonical form on
// purpose (write's created/old_hash, read_skill's resolved_path) list
// those fields in extraProps; their idempotence comes from re-deriving
// the values from the unchanged environment at replay.
//
// Not covered here, with reasons: kb_search/kb_read (dynamic schema;
// binding the resolved collection into the canonical form is a
// deliberate config-drift guard, idempotent under unchanged config),
// browser/MCP/imagegen/webfetch/websearch (heavy external
// dependencies), subagent tools (need a full manager),
// view_image/present_image (pure path normalization, no transforms).
func TestToolCanonicalContract(t *testing.T) {
	root := t.TempDir()
	validator, err := workspacepkg.NewPathValidator(root)
	if err != nil {
		t.Fatalf("NewPathValidator: %v", err)
	}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox:  process.ExplicitTestSandbox{},
		LookPath: exec.LookPath,
	})
	if err != nil {
		t.Fatalf("NewRunner: %v", err)
	}
	book := workspacepkg.NewFileStateBook()
	memStore, err := memory.OpenStore(filepath.Join(t.TempDir(), "memories"))
	if err != nil {
		t.Fatalf("memory.OpenStore: %v", err)
	}
	sessionMgr, err := exsession.NewManager(runner, nil, time.Minute)
	if err != nil {
		t.Fatalf("exsession.NewManager: %v", err)
	}
	t.Cleanup(sessionMgr.Close)

	mustContractFile(t, root, "note.txt", "hello world\n")
	snapshot, err := validator.Snapshot(filepath.Join(root, "note.txt"))
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	book.Record(filepath.Join(root, "note.txt"), snapshot.SHA256)

	contractSkillDir := filepath.Join(root, ".loom", "skills", "contract-skill")
	if err := os.MkdirAll(contractSkillDir, 0o755); err != nil {
		t.Fatalf("mkdir skill: %v", err)
	}
	if err := os.WriteFile(filepath.Join(contractSkillDir, skill.FileName),
		[]byte("---\nname: contract-skill\ndescription: contract test skill\n---\n\nbody\n"), 0o644); err != nil {
		t.Fatalf("write SKILL.md: %v", err)
	}
	var catalog skill.AtomicCatalog
	catalog.Store(skill.NewLoader(root, nil, nil).Load(context.Background()))
	readSkill, err := skillread.NewReadSkillTool(&catalog)
	if err != nil {
		t.Fatalf("NewReadSkillTool: %v", err)
	}

	readFile, err := builtin.NewReadFileTool(validator, book)
	if err != nil {
		t.Fatalf("NewReadFileTool: %v", err)
	}
	grepTool, err := builtin.NewSearchTool(validator, nil)
	if err != nil {
		t.Fatalf("NewSearchTool: %v", err)
	}
	globTool, err := builtin.NewGlobTool(validator, nil)
	if err != nil {
		t.Fatalf("NewGlobTool: %v", err)
	}
	writeTool, err := edit.NewWriteTool(validator)
	if err != nil {
		t.Fatalf("NewWriteTool: %v", err)
	}
	editTool, err := edit.NewEditTool(validator, book)
	if err != nil {
		t.Fatalf("NewEditTool: %v", err)
	}
	runCmd, err := command.NewRunCmdTool(validator, runner)
	if err != nil {
		t.Fatalf("NewRunCmdTool: %v", err)
	}
	execSess, err := exsession.NewExecSessionTool(validator, sessionMgr)
	if err != nil {
		t.Fatalf("NewExecSessionTool: %v", err)
	}
	updateTask, err := NewUpdateTaskTool(NewGoalCell(), NewPlanCell())
	if err != nil {
		t.Fatalf("NewUpdateTaskTool: %v", err)
	}
	memTool, err := memory.NewMemoryTool(memStore)
	if err != nil {
		t.Fatalf("NewMemoryTool: %v", err)
	}
	askUser, err := NewAskUserTool(domain.AutonomousQuestioner{})
	if err != nil {
		t.Fatalf("NewAskUserTool: %v", err)
	}

	cases := []struct {
		name string
		tool domain.Tool
		// extraProps allowlists internal canonical fields outside the
		// schema — each entry must be justified by a re-derivation
		// mechanism at replay (see the contract comment above).
		extraProps []string
		args       []string
	}{
		{"read_file", readFile, nil, []string{
			`{"path":"note.txt"}`,
			`{"path":"./note.txt"}`,
		}},
		{"grep", grepTool, nil, []string{
			`{"pattern":"hello"}`,
			`{"pattern":"hello","path":".","fixed_strings":true}`,
		}},
		{"glob", globTool, nil, []string{
			`{"pattern":"*.txt"}`,
			`{"pattern":"**/*.txt","path":"."}`,
		}},
		{"write", writeTool, []string{"created", "old_hash"}, []string{
			`{"path":"contract-new.txt","content":"brand new\n"}`,
			`{"path":"note.txt","content":"overwrite\n"}`,
		}},
		{"edit", editTool, nil, []string{
			`{"path":"note.txt","old_string":"world","new_string":"loom"}`,
			`{"path":"note.txt","edits":[{"old_string":"world","new_string":"loom"}],"replace_all":null}`,
		}},
		{"run_cmd", runCmd, nil, []string{
			`{"command":"echo hi"}`,
			`{"command":"echo hi","working_dir":null,"env":null,"timeout_ms":null,"max_output_tokens":256}`,
		}},
		{"exec_session", execSess, nil, []string{
			`{"action":"start","command":"echo hi"}`,
			`{"action":"start","command":"echo hi","yield_time_ms":null,"max_output_tokens":512}`,
		}},
		{"update_task", updateTask, nil, []string{
			// Cross-action mirror (sess_eb40ddfc) and item-level hoist
			// (sess_5fb89a53): stripped fields ride the prepared call,
			// never the canonical arguments.
			`{"action":"plan","objective":"","evidence":null,"title":"t",` +
				`"plan":[{"goal":"a","status":"in_progress","evidence":null},{"goal":"b","status":"pending"}]}`,
			`{"action":"goal","objective":"ship it","title":"stray title"}`,
			`{"action":"plan","title":"t2",` +
				`"plan":[{"goal":"a","status":"done"},{"goal":"b","status":"in-progress"}]}`,
		}},
		{"memory", memTool, nil, []string{
			`{"action":"add_note","filename":"prefer-go.md","note":"User prefers Go"}`,
			`{"action":"add_note","note":"no filename at all"}`,
			`{"action":"list","path":null,"max_results":null}`,
			`{"action":"read","path":"MEMORY.md"}`,
		}},
		{"ask_user", askUser, nil, []string{
			`{"question":"继续吗？","options":[{"label":"是"},{"label":"否"}],"allow_multiple":null}`,
		}},
		{"read_skill", readSkill, []string{"resolved_path"}, []string{
			`{"name":"contract-skill"}`,
			`{"name":"contract-skill","path":null,"offset":null,"limit":null}`,
		}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var schema struct {
				Properties map[string]json.RawMessage `json:"properties"`
			}
			if err := json.Unmarshal(tc.tool.Definition().InputSchema, &schema); err != nil {
				t.Fatalf("InputSchema undecodable: %v", err)
			}
			allowed := map[string]bool{}
			for key := range schema.Properties {
				allowed[key] = true
			}
			for _, key := range tc.extraProps {
				allowed[key] = true
			}

			for _, args := range tc.args {
				prepared, err := tc.tool.Prepare(context.Background(), domain.ToolCall{
					ID:        domain.NewToolCallID(),
					Name:      tc.tool.Definition().Name,
					Arguments: json.RawMessage(args),
				})
				if err != nil {
					t.Fatalf("Prepare(%s): %v", args, err)
				}

				// Invariant 1: canonical top-level fields ⊆ schema
				// properties + allowlisted internal fields.
				var fields map[string]json.RawMessage
				if err := json.Unmarshal(prepared.Call.Arguments, &fields); err != nil {
					t.Fatalf("Prepare(%s): canonical undecodable: %v", args, err)
				}
				for key := range fields {
					if !allowed[key] {
						t.Errorf("Prepare(%s): canonical carries non-schema field %q: %s",
							args, key, prepared.Call.Arguments)
					}
				}

				// Invariant 2: the schema-projected replay (what the
				// transcript rewrite leaves behind, and what
				// verifyPreparedFreshness re-Prepares) reproduces the
				// signed canonical form.
				projected := modelFacingCanonicalArgs(prepared)
				fresh, err := tc.tool.Prepare(context.Background(), domain.ToolCall{
					ID:        prepared.Call.ID,
					Name:      tc.tool.Definition().Name,
					Arguments: projected,
				})
				if err != nil {
					t.Fatalf("Prepare(%s): freshness re-Prepare of %s: %v", args, projected, err)
				}
				matched, err := canonicalJSONEqual(fresh.Call.Arguments, prepared.Call.Arguments)
				if err != nil {
					t.Fatalf("Prepare(%s): canonical comparison: %v", args, err)
				}
				if !matched {
					t.Errorf("Prepare(%s): freshness replay diverges:\nfresh:  %s\nsigned: %s",
						args, fresh.Call.Arguments, prepared.Call.Arguments)
				}

				// Invariant 3: the repaired-arguments path replays the signed
				// canonical form itself (run.go: freshnessOriginal =
				// prepared.Call). The strict decode must tolerate every field
				// the canonical form carries, and the re-Prepare must
				// reproduce it.
				replayed, err := tc.tool.Prepare(context.Background(), domain.ToolCall{
					ID:        prepared.Call.ID,
					Name:      tc.tool.Definition().Name,
					Arguments: prepared.Call.Arguments,
				})
				if err != nil {
					t.Errorf("Prepare(%s): canonical self-replay rejected: %v", args, err)
					continue
				}
				matched, err = canonicalJSONEqual(replayed.Call.Arguments, prepared.Call.Arguments)
				if err != nil {
					t.Fatalf("Prepare(%s): canonical comparison: %v", args, err)
				}
				if !matched {
					t.Errorf("Prepare(%s): canonical self-replay diverges:\nreplayed: %s\nsigned:   %s",
						args, replayed.Call.Arguments, prepared.Call.Arguments)
				}
			}
		})
	}
}

func mustContractFile(t *testing.T, root, name, content string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(root, name), []byte(content), 0o644); err != nil {
		t.Fatalf("write %s: %v", name, err)
	}
}
