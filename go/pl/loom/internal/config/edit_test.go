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
// Created: 2026/08/08

package config

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func ptr[T any](v T) *T { return &v }

// editTestFile builds a File exercising every secret-bearing section.
func editTestFile() *File {
	return editTestFileWithEnv()
}

// editTestFileWithEnv is editTestFile plus an MCP env secret, the stored
// counterpart for mask/restore round trips.
func editTestFileWithEnv() *File {
	return &File{
		Default: "deepseek/deepseek-chat",
		Providers: []Provider{{
			Name:       "deepseek",
			Type:       "openai",
			BaseURL:    "https://api.deepseek.com/v1",
			APIKey:     "sk-real-key",
			MaxRetries: ptr(3),
			Models: []Model{
				{Name: "deepseek-chat", ContextWindow: 65536},
				{Name: "deepseek-reasoner", ContextWindow: 65536, WireAPI: "responses"},
			},
		}},
		Limits:  Limits{MaxCostUSD: ptr(5.0)},
		Context: Context{CompactTriggerRatio: ptr(0.8)},
		Tracing: Tracing{Host: "https://langfuse.internal", PublicKey: "pk-lf-real", SecretKey: "sk-lf-real"},
		MCPServers: map[string]MCPServer{
			"remote": {
				URL: "https://mcp.example.com/mcp",
				Headers: map[string]string{
					"Authorization": "Bearer ${MCP_TOKEN}",
					"X-Api-Key":     "raw-token-value",
				},
			},
			"local": {
				Command: "npx",
				Args:    []string{"-y", "@mcp/server-fs"},
				Env:     map[string]string{"GITHUB_TOKEN": "ghp_secret"},
			},
		},
	}
}

func TestMaskSecrets(t *testing.T) {
	f := editTestFile()
	f.MaskSecrets()
	if f.Providers[0].APIKey != SecretMask {
		t.Fatalf("api_key = %q, want mask", f.Providers[0].APIKey)
	}
	if f.Tracing.PublicKey != SecretMask || f.Tracing.SecretKey != SecretMask {
		t.Fatalf("tracing keys not masked: %+v", f.Tracing)
	}
	headers := f.MCPServers["remote"].Headers
	if headers["X-Api-Key"] != SecretMask {
		t.Fatalf("raw header = %q, want mask", headers["X-Api-Key"])
	}
	// Environment references are not secrets and must stay readable.
	if headers["Authorization"] != "Bearer ${MCP_TOKEN}" {
		t.Fatalf("env-ref header = %q, want the original reference", headers["Authorization"])
	}
}

func TestMaskSecretsCoversMCPEnv(t *testing.T) {
	f := editTestFile()
	f.MCPServers["local"] = MCPServer{
		Command: "npx",
		Env:     map[string]string{"GITHUB_TOKEN": "ghp_secret", "PLAIN": ""},
	}
	f.MaskSecrets()
	env := f.MCPServers["local"].Env
	if env["GITHUB_TOKEN"] != SecretMask {
		t.Fatalf("env secret = %q, want mask", env["GITHUB_TOKEN"])
	}
	if env["PLAIN"] != "" {
		t.Fatalf("empty env value became %q", env["PLAIN"])
	}
	// Masked env values restore from the stored file; an unresolvable one
	// (server renamed) is a hard error.
	if err := f.RestoreSecretsFrom(editTestFileWithEnv()); err != nil {
		t.Fatalf("RestoreSecretsFrom: %v", err)
	}
	if f.MCPServers["local"].Env["GITHUB_TOKEN"] != "ghp_secret" {
		t.Fatalf("env not restored: %q", f.MCPServers["local"].Env["GITHUB_TOKEN"])
	}
}

func TestMaskSecretsLeavesEmptyFields(t *testing.T) {
	f := &File{Providers: []Provider{{Name: "p", APIKeyEnv: "P_API_KEY"}}}
	f.MaskSecrets()
	if f.Providers[0].APIKey != "" {
		t.Fatalf("empty api_key became %q", f.Providers[0].APIKey)
	}
	if f.Providers[0].APIKeyEnv != "P_API_KEY" {
		t.Fatalf("api_key_env = %q, want untouched", f.Providers[0].APIKeyEnv)
	}
}

func TestRestoreSecretsFrom(t *testing.T) {
	cur := editTestFile()
	masked := editTestFile()
	masked.MaskSecrets()
	if err := masked.RestoreSecretsFrom(cur); err != nil {
		t.Fatalf("RestoreSecretsFrom: %v", err)
	}
	if !reflect.DeepEqual(masked, cur) {
		t.Fatalf("restored file differs:\n got %+v\nwant %+v", masked, cur)
	}
}

func TestRestoreSecretsFromUnresolvable(t *testing.T) {
	cur := editTestFile()
	masked := editTestFile()
	masked.MaskSecrets()
	// Renaming the provider in the same edit breaks the structural match.
	masked.Providers[0].Name = "renamed"
	err := masked.RestoreSecretsFrom(cur)
	if err == nil || !strings.Contains(err.Error(), "renamed") {
		t.Fatalf("err = %v, want an unresolved-mask error naming the provider", err)
	}
}

func TestToMapAndDecodeRoundTrip(t *testing.T) {
	f := editTestFile()
	m, err := f.ToMap()
	if err != nil {
		t.Fatalf("ToMap: %v", err)
	}
	// Unset sections stay implicit — the wire reflects exactly what a
	// file would carry.
	for _, absent := range []string{"memory", "subagent", "image", "ui", "workspaces"} {
		if _, ok := m[absent]; ok {
			t.Fatalf("unexpected key %q in rendered map", absent)
		}
	}
	raw, err := json.Marshal(m)
	if err != nil {
		t.Fatalf("marshal map: %v", err)
	}
	back, err := DecodeFileJSON(raw)
	if err != nil {
		t.Fatalf("DecodeFileJSON: %v", err)
	}
	if !reflect.DeepEqual(back, f) {
		t.Fatalf("round trip differs:\n got %+v\nwant %+v", back, f)
	}
}

func TestDecodeFileJSONRejectsUnknownKeys(t *testing.T) {
	_, err := DecodeFileJSON([]byte(`{"provideers": []}`))
	if err == nil {
		t.Fatal("unknown key accepted")
	}
}

// TestParseFileRejectsMultiDocument: a second YAML document would be
// silently dropped by the merge/save path — reject it loudly instead.
func TestParseFileRejectsMultiDocument(t *testing.T) {
	doc := "providers: []\n---\nlimits: {}\n"
	if _, err := ParseFile([]byte(doc)); err == nil {
		t.Fatal("multi-document config accepted")
	}
}

func TestResolve(t *testing.T) {
	lookup := envWith(map[string]string{"MCP_TOKEN": "tok"})
	resolved, err := editTestFile().Resolve(lookup)
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if len(resolved.Providers) != 1 || resolved.Default.String() != "deepseek/deepseek-chat" {
		t.Fatalf("resolved = %+v", resolved)
	}

	// Resolve must not mutate its input: the file is serialized to disk
	// afterwards, and inherited wire_api/reasoning defaults materialized
	// by resolve would leak into it.
	f := editTestFile()
	f.Providers[0].Models[0].WireAPI = ""
	if _, err := f.Resolve(lookup); err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if got := f.Providers[0].Models[0].WireAPI; got != "" {
		t.Fatalf("Resolve mutated the input file: WireAPI = %q", got)
	}

	bare := editTestFile()
	bare.Providers = nil
	if _, err := bare.Resolve(lookup); err == nil {
		t.Fatal("no providers accepted")
	}

	bad := editTestFile()
	bad.Approval.Mode = "whenever"
	if _, err := bad.Resolve(lookup); err == nil || !strings.Contains(err.Error(), "approval") {
		t.Fatalf("err = %v, want an approval-mode error", err)
	}

	missingEnv := editTestFile()
	if _, err := missingEnv.Resolve(noEnv); err == nil || !strings.Contains(err.Error(), "MCP_TOKEN") {
		t.Fatalf("err = %v, want a missing-env error for the ${MCP_TOKEN} header", err)
	}
}

// TestMergeIntoYAMLPreservesComments locks the core promise of the edit
// layer: saving through the API must not strip the annotations a user
// wrote into the file.
func TestMergeIntoYAMLPreservesComments(t *testing.T) {
	raw := `# loom configuration — hand-written

# Budget guardrails.
limits:
  max_cost_usd: 5.0 # monthly budget

# Model endpoints.
providers:
  - name: deepseek # primary gateway
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: sk-old
    models:
      - name: deepseek-chat
        context_window: 65536
`
	f := &File{
		Providers: []Provider{{
			Name:    "deepseek",
			Type:    "openai",
			BaseURL: "https://api.deepseek.com/v1",
			APIKey:  "sk-new",
			Models: []Model{
				{Name: "deepseek-chat", ContextWindow: 65536},
				{Name: "deepseek-reasoner", ContextWindow: 65536},
			},
		}},
		Limits: Limits{MaxCostUSD: ptr(9.5)},
	}
	out, err := MergeIntoYAML([]byte(raw), f)
	if err != nil {
		t.Fatalf("MergeIntoYAML: %v", err)
	}
	text := string(out)
	for _, want := range []string{
		"# loom configuration — hand-written",
		"# Budget guardrails.",
		"# monthly budget",
		"# Model endpoints.",
		"# primary gateway",
		"max_cost_usd: 9.5",
		"api_key: sk-new",
		"deepseek-reasoner",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("merged output missing %q:\n%s", want, text)
		}
	}
	// The merged document must remain a strictly valid config.
	if _, err := ParseFile(out); err != nil {
		t.Fatalf("merged output does not parse: %v\n%s", err, text)
	}
}

func TestMergeIntoYAMLDropsRemovedSections(t *testing.T) {
	raw := `limits:
  max_cost_usd: 5.0
providers:
  - name: deepseek
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: sk-old
    models:
      - name: deepseek-chat
        context_window: 65536
`
	f := editTestFile() // provider deepseek with two models
	f.Limits = Limits{} // the section was removed in the UI edit
	out, err := MergeIntoYAML([]byte(raw), f)
	if err != nil {
		t.Fatalf("MergeIntoYAML: %v", err)
	}
	text := string(out)
	if strings.Contains(text, "max_cost_usd") {
		t.Fatalf("removed limits section survived:\n%s", text)
	}
	if !strings.Contains(text, "deepseek-reasoner") {
		t.Fatalf("new model missing:\n%s", text)
	}
}

func TestMergeIntoYAMLFreshDocument(t *testing.T) {
	f := editTestFile()
	out, err := MergeIntoYAML(nil, f)
	if err != nil {
		t.Fatalf("MergeIntoYAML: %v", err)
	}
	back, err := ParseFile(out)
	if err != nil {
		t.Fatalf("fresh document does not parse: %v", err)
	}
	if !reflect.DeepEqual(back, f) {
		t.Fatalf("fresh document differs:\n got %+v\nwant %+v", back, f)
	}
}

func TestRevisionOf(t *testing.T) {
	a := RevisionOf([]byte("one"))
	if a == RevisionOf([]byte("two")) {
		t.Fatal("distinct contents share a revision")
	}
	if a != RevisionOf([]byte("one")) {
		t.Fatal("same content, different revisions")
	}
}

func TestWriteFileAtomic(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.yaml")
	if err := WriteFileAtomic(path, []byte("a: 1\n")); err != nil {
		t.Fatalf("WriteFileAtomic: %v", err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("new file mode = %o, want 600", info.Mode().Perm())
	}
	// An existing file keeps its permissions across saves.
	if err := os.Chmod(path, 0o640); err != nil {
		t.Fatalf("chmod: %v", err)
	}
	if err := WriteFileAtomic(path, []byte("a: 2\n")); err != nil {
		t.Fatalf("WriteFileAtomic: %v", err)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(raw) != "a: 2\n" {
		t.Fatalf("content = %q, want updated", raw)
	}
	if info, err := os.Stat(path); err != nil || info.Mode().Perm() != 0o640 {
		t.Fatalf("mode after rewrite = %v, want 640 preserved", info.Mode().Perm())
	}
}
