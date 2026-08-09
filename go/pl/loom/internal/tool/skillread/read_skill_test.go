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
// Created: 2026/07/26

package skillread

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
)

func writeFile(t *testing.T, path, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("MkdirAll error = %v", err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatalf("WriteFile(%q) error = %v", path, err)
	}
}

func writeSkill(t *testing.T, root, dir, name, body string) string {
	t.Helper()
	skillDir := filepath.Join(root, ".loom", "skills", dir)
	writeFile(t, filepath.Join(skillDir, skill.FileName),
		"---\nname: "+name+"\ndescription: test skill\n---\n\n"+body)
	return skillDir
}

// loadCatalog discovers skills under root into a fresh AtomicCatalog.
func loadCatalog(t *testing.T, root string) *skill.AtomicCatalog {
	t.Helper()
	var atomic skill.AtomicCatalog
	atomic.Store(skill.NewLoader(root, nil, nil).Load(context.Background()))
	return &atomic
}

func newTool(t *testing.T, catalog *skill.AtomicCatalog) *ReadSkillTool {
	t.Helper()
	tool, err := NewReadSkillTool(catalog)
	if err != nil {
		t.Fatalf("NewReadSkillTool() error = %v", err)
	}
	return tool
}

func newCall(t *testing.T, args map[string]any) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_skill", Arguments: raw}
}

func prepare(t *testing.T, tool *ReadSkillTool, args map[string]any) domain.PreparedCall {
	t.Helper()
	prepared, err := tool.Prepare(context.Background(), newCall(t, args))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	return prepared
}

func executeOK(t *testing.T, tool *ReadSkillTool, prepared domain.PreparedCall) string {
	t.Helper()
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() status = %s, error = %+v", result.Status, result.Error)
	}
	if len(result.Content) != 1 || result.Content[0].Kind != domain.PartText {
		t.Fatalf("content = %+v, want a single text part", result.Content)
	}
	return result.Content[0].Text
}

func executeErr(t *testing.T, tool *ReadSkillTool, prepared domain.PreparedCall) *domain.ToolError {
	t.Helper()
	result := tool.Execute(context.Background(), prepared)
	if result.Error == nil {
		t.Fatalf("Execute() error = nil, want error")
	}
	return result.Error
}

func prepareErr(t *testing.T, tool *ReadSkillTool, args map[string]any) string {
	t.Helper()
	_, err := tool.Prepare(context.Background(), newCall(t, args))
	if err == nil {
		t.Fatal("Prepare() error = nil, want error")
	}
	return err.Error()
}

// A missing file inside the skill directory must be named in the error
// so the model can tell which path was wrong.
func TestReadSkillMissingPathNamesPath(t *testing.T) {
	root := t.TempDir()
	writeSkill(t, root, "weather", "weather", "body\n")
	tool := newTool(t, loadCatalog(t, root))

	msg := prepareErr(t, tool, map[string]any{"name": "weather", "path": "references/missing.md"})
	if !strings.Contains(msg, `path does not exist: "references/missing.md"`) {
		t.Fatalf("error = %q, want the offending path named", msg)
	}
}

func TestReadSkillDefinitionRisk(t *testing.T) {
	tool := newTool(t, &skill.AtomicCatalog{})
	if got := tool.Definition().Risk(); got != domain.R1 {
		t.Fatalf("Risk() = %v, want R1 (auto-approved)", got)
	}
	if tool.Definition().Name != "read_skill" {
		t.Fatalf("Name = %q", tool.Definition().Name)
	}
}

func TestReadSkillDefaultPath(t *testing.T) {
	root := t.TempDir()
	writeSkill(t, root, "weather", "weather", "line one\nline two\n")
	tool := newTool(t, loadCatalog(t, root))

	out := executeOK(t, tool, prepare(t, tool, map[string]any{"name": "weather"}))
	if !strings.HasPrefix(out, "skill: weather · path: "+skill.FileName+" · dir: ") {
		t.Fatalf("output header = %q", out)
	}
	if !strings.Contains(out, "· lines 1-7 of 7 ·") {
		t.Fatalf("output range = %q", out)
	}
	if strings.Contains(out, "[truncated:") {
		t.Fatalf("unexpected truncation: %q", out)
	}
	// Line 7 is the body line after frontmatter + blank lines.
	if !strings.Contains(out, "     7→line two") {
		t.Fatalf("numbered lines = %q", out)
	}
	if !strings.Contains(out, "· dir: /") || !strings.Contains(out, "weather · lines") {
		t.Fatalf("dir in header = %q, want absolute skill dir", out)
	}
}

func TestReadSkillSubPathAndPagination(t *testing.T) {
	root := t.TempDir()
	dir := writeSkill(t, root, "weather", "weather", "")
	var ref []string
	for i := 1; i <= 10; i++ {
		ref = append(ref, "ref line")
	}
	writeFile(t, filepath.Join(dir, "references", "cli.md"), strings.Join(ref, "\n")+"\n")
	tool := newTool(t, loadCatalog(t, root))

	out := executeOK(t, tool, prepare(t, tool, map[string]any{
		"name": "weather", "path": "references/cli.md", "offset": 4, "limit": 3,
	}))
	if !strings.Contains(out, "· lines 4-6 of 10 ·") {
		t.Fatalf("pagination header = %q", out)
	}
	if !strings.Contains(out, "     4→ref line\n     5→ref line\n     6→ref line\n") {
		t.Fatalf("numbered lines = %q", out)
	}
	if !strings.Contains(out, "[truncated: 4 more lines; call read_skill with offset=7 to continue]") {
		t.Fatalf("truncation marker = %q", out)
	}

	out = executeOK(t, tool, prepare(t, tool, map[string]any{
		"name": "weather", "path": "references/cli.md", "offset": 8, "limit": 500,
	}))
	if !strings.Contains(out, "· lines 8-10 of 10 ·") || strings.Contains(out, "[truncated:") {
		t.Fatalf("tail page = %q", out)
	}
}

func TestReadSkillUnknownNameListsAvailable(t *testing.T) {
	root := t.TempDir()
	writeSkill(t, root, "weather", "weather", "")
	writeSkill(t, root, "review", "review", "")
	tool := newTool(t, loadCatalog(t, root))

	msg := prepareErr(t, tool, map[string]any{"name": "missing"})
	if !strings.Contains(msg, "weather") || !strings.Contains(msg, "review") {
		t.Fatalf("error = %q, want available skill names", msg)
	}

	empty := newTool(t, &skill.AtomicCatalog{})
	msg = prepareErr(t, empty, map[string]any{"name": "missing"})
	if !strings.Contains(msg, "no skills are available") {
		t.Fatalf("error = %q, want empty-catalog message", msg)
	}
}

func TestReadSkillRejectsEscapes(t *testing.T) {
	root := t.TempDir()
	dir := writeSkill(t, root, "weather", "weather", "")
	writeFile(t, filepath.Join(root, "secret.txt"), "top secret")
	tool := newTool(t, loadCatalog(t, root))

	cases := map[string]string{
		"dotdot escape": "../secret.txt",
		"absolute path": filepath.Join(root, "secret.txt"),
		"nested dotdot": "references/../../secret.txt",
	}
	for name, path := range cases {
		t.Run(name, func(t *testing.T) {
			prepareErr(t, tool, map[string]any{"name": "weather", "path": path})
		})
	}

	// Symlink inside the skill directory pointing outside must be rejected.
	writeFile(t, filepath.Join(dir, "references", "ok.md"), "ok")
	if err := os.Symlink(filepath.Join(root, "secret.txt"), filepath.Join(dir, "references", "evil.md")); err != nil {
		t.Fatal(err)
	}
	prepareErr(t, tool, map[string]any{"name": "weather", "path": "references/evil.md"})
}

func TestReadSkillRejectsSensitiveAndBinaryAndLarge(t *testing.T) {
	root := t.TempDir()
	dir := writeSkill(t, root, "weather", "weather", "")
	writeFile(t, filepath.Join(dir, "scripts", ".env"), "TOKEN=x")
	writeFile(t, filepath.Join(dir, "assets", "logo.bin"), string([]byte{0x89, 0x50, 0x00, 0x47}))
	writeFile(t, filepath.Join(dir, "big.txt"), strings.Repeat("x", 300<<10))
	tool := newTool(t, loadCatalog(t, root))

	if msg := prepareErr(t, tool, map[string]any{"name": "weather", "path": "scripts/.env"}); !strings.Contains(msg, "sensitive") {
		t.Fatalf(".env error = %q, want sensitive", msg)
	}
	if msg := executeErr(t, tool, prepare(t, tool, map[string]any{"name": "weather", "path": "assets/logo.bin"})).Message; !strings.Contains(msg, "binary") {
		t.Fatalf("binary error = %q", msg)
	}
	if msg := prepareErr(t, tool, map[string]any{"name": "weather", "path": "big.txt"}); !strings.Contains(msg, "256KB") {
		t.Fatalf("large error = %q, want 256KB limit", msg)
	}
}

func TestReadSkillPrepareIsDeterministicForFreshness(t *testing.T) {
	root := t.TempDir()
	writeSkill(t, root, "weather", "weather", "body")
	tool := newTool(t, loadCatalog(t, root))
	call := newCall(t, map[string]any{"name": "weather"})

	first, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	second, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	// The loop's verifyPreparedFreshness re-Prepares and compares canonical
	// arguments: they must be byte-identical for an unchanged world.
	if string(first.Call.Arguments) != string(second.Call.Arguments) {
		t.Fatalf("canonical arguments differ:\n%s\n%s", first.Call.Arguments, second.Call.Arguments)
	}
	if first.ArgsHash != second.ArgsHash {
		t.Fatal("ArgsHash differs across identical Prepare calls")
	}
	if !strings.Contains(string(first.Call.Arguments), "resolved_path") {
		t.Fatal("canonical arguments must carry the signed resolved_path")
	}
}

func TestReadSkillExecuteFailsClosedOnCatalogDrift(t *testing.T) {
	rootA := t.TempDir()
	writeSkill(t, rootA, "weather", "weather", "body A")
	atomic := loadCatalog(t, rootA)
	tool := newTool(t, atomic)
	prepared := prepare(t, tool, map[string]any{"name": "weather"})

	// Skill removed between Prepare and Execute.
	atomic.Store(nil)
	if err := executeErr(t, tool, prepared); err.Code != string(domain.ErrSecurity) {
		t.Fatalf("removed skill: code = %s, want security", err.Code)
	}

	// Same skill name re-appears but from a different directory: the
	// re-resolved path no longer matches the signed one.
	atomic = loadCatalog(t, rootA)
	tool = newTool(t, atomic)
	prepared = prepare(t, tool, map[string]any{"name": "weather"})
	rootB := t.TempDir()
	writeSkill(t, rootB, "weather", "weather", "body B")
	atomic.Store(skill.NewLoader(rootB, nil, nil).Load(context.Background()))
	if err := executeErr(t, tool, prepared); err.Code != string(domain.ErrSecurity) || !strings.Contains(err.Message, "binding mismatch") {
		t.Fatalf("moved skill: error = %+v, want security binding mismatch", err)
	}
}

func TestReadSkillMultibyteCharacterAtSampleBoundary(t *testing.T) {
	// Regression: the 8KB binary-detection sample must back off to a rune
	// boundary. A CJK character straddling byte 8192 previously made a
	// plain-text SKILL.md (e.g. bi-query-sql) be rejected as binary.
	root := t.TempDir()
	wrapper := "---\nname: cjk-skill\ndescription: test skill\n---\n\n"
	// Place the lead byte of "完" (3 bytes) at offset 8190, so the 8KB
	// sample cut splits it mid-rune (8190..8192, cut keeps 0..8191).
	body := strings.Repeat("a", 8190-len(wrapper)) + "完整" + strings.Repeat("b", 100)
	writeFile(t, filepath.Join(root, ".loom", "skills", "cjk-skill", skill.FileName), wrapper+body)
	tool := newTool(t, loadCatalog(t, root))

	out := executeOK(t, tool, prepare(t, tool, map[string]any{"name": "cjk-skill"}))
	if !strings.Contains(out, "· 8.") && !strings.Contains(out, "· 9.") {
		t.Fatalf("output header = %q, want the >8KB CJK file read successfully", out)
	}
	if !strings.Contains(out, "→"+strings.Repeat("a", 100)[:50]) {
		t.Fatalf("numbered body missing: %.200q", out)
	}
}

func TestReadSkillExecuteRejectsForgedCalls(t *testing.T) {
	root := t.TempDir()
	writeSkill(t, root, "weather", "weather", "body")
	tool := newTool(t, loadCatalog(t, root))
	prepared := prepare(t, tool, map[string]any{"name": "weather"})

	// Tampered arguments break the HMAC signature.
	var args map[string]any
	if err := json.Unmarshal(prepared.Call.Arguments, &args); err != nil {
		t.Fatal(err)
	}
	args["path"] = "other.md"
	raw, _ := json.Marshal(args)
	forged := prepared
	forged.Call.Arguments = raw
	if err := executeErr(t, tool, forged); err.Code != string(domain.ErrSecurity) {
		t.Fatalf("forged call: code = %s, want security", err.Code)
	}

	// A model-supplied resolved_path is overwritten by Prepare.
	call := newCall(t, map[string]any{"name": "weather", "resolved_path": "/etc/passwd"})
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	var canonical readSkillArgs
	if err := json.Unmarshal(prepared.Call.Arguments, &canonical); err != nil {
		t.Fatal(err)
	}
	if canonical.ResolvedPath == "/etc/passwd" {
		t.Fatal("model-supplied resolved_path must be overwritten")
	}
}
