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

package config

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"gopkg.in/yaml.v3"
)

// TestMain guarantees a $HOME: hermetic runners (bazel) unset it, and
// DefaultHomeDir / "~" expansion rely on it.
func TestMain(m *testing.M) {
	if os.Getenv("HOME") == "" {
		_ = os.Setenv("HOME", os.TempDir())
	}
	os.Exit(m.Run())
}

// noEnv rejects every secret reference; tests that need secrets inject
// their own lookup.
func noEnv(string) (string, bool) { return "", false }

func envWith(pairs map[string]string) EnvLookup {
	return func(key string) (string, bool) {
		v, ok := pairs[key]
		return v, ok
	}
}

const twoProviderYAML = `
default: deepseek/deepseek-chat
providers:
  - name: deepseek
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: sk-test
    models:
      - name: deepseek-chat
        context_window: 65536
      - name: deepseek-reasoner
        context_window: 65536
        wire_api: responses
  - name: openai
    type: openai
    base_url: https://api.openai.com/v1
    api_key_env: OPENAI_API_KEY
    default_model: gpt-5
    models:
      - name: gpt-5
        context_window: 400000
`

// writeConfig writes content as config.yaml inside a fresh temp loom
// home and returns the home directory.
func writeConfig(t *testing.T, content string) string {
	t.Helper()
	home := t.TempDir()
	if err := os.WriteFile(filepath.Join(home, FileName), []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	return home
}

func loadFile(t *testing.T, content string, lookup EnvLookup) *ResolvedConfig {
	t.Helper()
	cfg, err := Load(writeConfig(t, content), LoadOptions{RequireProviders: true}, lookup)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	return cfg
}

func TestLoadResolvesProvidersAndDefault(t *testing.T) {
	cfg := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk-env"}))

	if len(cfg.Providers) != 2 {
		t.Fatalf("providers = %d, want 2", len(cfg.Providers))
	}
	if cfg.Default != (ProviderModelRef{Provider: "deepseek", Model: "deepseek-chat"}) {
		t.Fatalf("default = %+v", cfg.Default)
	}
	deepseek := cfg.ProviderByName("deepseek")
	if deepseek == nil || deepseek.Model == nil {
		t.Fatal("deepseek provider not assembled")
	}
	if deepseek.DefaultModel != "deepseek-chat" {
		t.Fatalf("deepseek default model = %q (implicit models[0])", deepseek.DefaultModel)
	}
	openai := cfg.ProviderByName("openai")
	if openai == nil || openai.DefaultModel != "gpt-5" {
		t.Fatalf("openai provider = %+v", openai)
	}
	meta, ok := cfg.ModelMeta(ProviderModelRef{Provider: "deepseek", Model: "deepseek-reasoner"})
	if !ok || meta.ContextWindow != 65536 || meta.WireAPI != "responses" {
		t.Fatalf("model meta = %+v, ok = %v", meta, ok)
	}
	// wire_api inheritance is expanded at resolve time: deepseek-chat has no
	// explicit value and takes the provider default (chat).
	meta, ok = cfg.ModelMeta(ProviderModelRef{Provider: "deepseek", Model: "deepseek-chat"})
	if !ok || meta.WireAPI != "chat" {
		t.Fatalf("inherited wire_api = %q, want chat", meta.WireAPI)
	}
}

func TestLoadImplicitDefaultIsFirstProvider(t *testing.T) {
	cfg := loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
      - name: m2
`, noEnv)
	if cfg.Default != (ProviderModelRef{Provider: "only", Model: "m1"}) {
		t.Fatalf("implicit default = %+v, want only/m1", cfg.Default)
	}
}

func TestStorageDerivesFromHome(t *testing.T) {
	// The loom home is the single data root — even for a missing config
	// file (offline commands run without providers).
	home := t.TempDir()
	cfg, err := Load(home, LoadOptions{}, noEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	want := home
	if cfg.Storage.BaseDir != want {
		t.Fatalf("BaseDir = %q, want %q", cfg.Storage.BaseDir, want)
	}
	derived := map[string]string{
		"SessionsDir":   cfg.Storage.SessionsDir(),
		"SessionDBPath": cfg.Storage.SessionDBPath(),
		"LogsDir":       cfg.Storage.LogsDir(),
		"CacheDir":      cfg.Storage.CacheDir(),
		"MemoriesDir":   cfg.Storage.MemoriesDir(),
		"RulesDir":      cfg.Storage.RulesDir(),
		"SkillsDir":     cfg.Storage.SkillsDir(),
		"LoomMDPath":    cfg.Storage.LoomMDPath(),
	}
	wantDerived := map[string]string{
		"SessionsDir":   filepath.Join(want, "sessions"),
		"SessionDBPath": filepath.Join(want, "sessions", "sessions.db"),
		"LogsDir":       filepath.Join(want, "logs"),
		"CacheDir":      filepath.Join(want, "cache"),
		"MemoriesDir":   filepath.Join(want, "memories"),
		"RulesDir":      filepath.Join(want, "rules"),
		"SkillsDir":     filepath.Join(want, "skills"),
		"LoomMDPath":    filepath.Join(want, "LOOM.md"),
	}
	for name, got := range derived {
		if got != wantDerived[name] {
			t.Errorf("%s = %q, want %q", name, got, wantDerived[name])
		}
	}
}

func TestStorageBaseDirIsHome(t *testing.T) {
	home := writeConfig(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
`)
	cfg, err := Load(home, LoadOptions{RequireProviders: true}, noEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.Storage.BaseDir != home {
		t.Fatalf("BaseDir = %q, want %q", cfg.Storage.BaseDir, home)
	}
	if got, want := cfg.Storage.SessionDBPath(), filepath.Join(home, "sessions", "sessions.db"); got != want {
		t.Fatalf("SessionDBPath() = %q, want %q", got, want)
	}
}

func TestLoadRelativeHomeResolvesAgainstCwd(t *testing.T) {
	t.Chdir(t.TempDir())
	cfg, err := Load(".", LoadOptions{}, noEnv)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Storage.BaseDir != cwd {
		t.Fatalf("BaseDir = %q, want %q (relative home resolves against cwd)", cfg.Storage.BaseDir, cwd)
	}
}

func TestResolveToolsPathExtra(t *testing.T) {
	// Absent: empty extras.
	cfg := loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
`, noEnv)
	if len(cfg.Tools.PathExtra) != 0 {
		t.Fatalf("tools.path_extra default = %v, want empty", cfg.Tools.PathExtra)
	}

	// Absolute entries pass through; "~/" expands.
	cfg = loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
tools:
  path_extra: ["/opt/corp/bin", "~/toolchain/bin"]
`, noEnv)
	if len(cfg.Tools.PathExtra) != 2 || cfg.Tools.PathExtra[0] != "/opt/corp/bin" {
		t.Fatalf("tools.path_extra = %v, want [/opt/corp/bin ~/toolchain/bin expanded]", cfg.Tools.PathExtra)
	}
	home, err := os.UserHomeDir()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Tools.PathExtra[1] != filepath.Join(home, "toolchain/bin") {
		t.Fatalf("tilde expansion = %q, want under %q", cfg.Tools.PathExtra[1], home)
	}

	// Empty and relative entries fail fast at load time.
	for _, extra := range []string{`[""]`, `["rel/bin"]`} {
		home := writeConfig(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
tools:
  path_extra: `+extra+`
`)
		if _, err := Load(home, LoadOptions{RequireProviders: true}, noEnv); err == nil {
			t.Fatalf("path_extra %s: want load error", extra)
		}
	}
}

func TestResolveSkillsExtraRoots(t *testing.T) {
	// Absent: no extra roots.
	cfg := loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
`, noEnv)
	if len(cfg.Skills.ExtraRoots) != 0 {
		t.Fatalf("skills.extra_roots default = %v, want empty", cfg.Skills.ExtraRoots)
	}

	// Absolute entries pass through; "~/" expands; blank entries drop.
	cfg = loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
skills:
  extra_roots: ["/opt/skills", "~/my-skills", ""]
`, noEnv)
	if len(cfg.Skills.ExtraRoots) != 2 || cfg.Skills.ExtraRoots[0] != "/opt/skills" {
		t.Fatalf("skills.extra_roots = %v, want [/opt/skills ~/my-skills expanded]", cfg.Skills.ExtraRoots)
	}
	home, err := os.UserHomeDir()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Skills.ExtraRoots[1] != filepath.Join(home, "my-skills") {
		t.Fatalf("tilde expansion = %q, want under %q", cfg.Skills.ExtraRoots[1], home)
	}
}

func TestResolveShare(t *testing.T) {
	// Defaults: disabled, fixed built-in bind address.
	cfg := loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
`, noEnv)
	if cfg.Share.Enabled || cfg.Share.Listen != DefaultShareListen {
		t.Fatalf("share defaults = %+v, want disabled with %q", cfg.Share, DefaultShareListen)
	}

	cfg = loadFile(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
share:
  enabled: true
  listen: 192.168.1.5:9000
`, noEnv)
	if !cfg.Share.Enabled || cfg.Share.Listen != "192.168.1.5:9000" {
		t.Fatalf("share = %+v, want enabled on 192.168.1.5:9000", cfg.Share)
	}

	// Invalid listen values fail fast: no host:port shape, port 0 (links
	// would not survive restarts), out-of-range ports.
	for _, listen := range []string{"not-an-addr", "0.0.0.0:0", "0.0.0.0:99999", "0.0.0.0:http"} {
		home := writeConfig(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
share:
  listen: '`+listen+`'
`)
		if _, err := Load(home, LoadOptions{RequireProviders: true}, noEnv); err == nil ||
			!strings.Contains(err.Error(), "share.listen") {
			t.Fatalf("share.listen %q: error = %v, want a share.listen error", listen, err)
		}
	}
}

func TestStorageSectionIsRejected(t *testing.T) {
	// storage.base_dir is gone: the loom home comes from LOOM_HOME (or
	// the default), and unknown keys fail fast so a stale config says so.
	home := writeConfig(t, `
providers:
  - name: only
    base_url: https://example.com/v1
    api_key: sk
    models:
      - name: m1
storage:
  base_dir: /tmp/elsewhere
`)
	_, err := Load(home, LoadOptions{RequireProviders: true}, noEnv)
	if err == nil || !strings.Contains(err.Error(), "storage") {
		t.Fatalf("Load() error = %v, want an unknown-key error naming storage", err)
	}
}

func TestLoadAnthropicProvider(t *testing.T) {
	cfg := loadFile(t, `
providers:
  - name: claude
    type: anthropic
    base_url: https://api.anthropic.com
    api_key_env: ANTHROPIC_API_KEY
    reasoning: {effort: medium}
    models:
      - name: claude-sonnet-4-6
        context_window: 200000
        max_output_tokens: 64000
      - name: claude-haiku-4-5
        reasoning: {effort: "off"}
`, envWith(map[string]string{"ANTHROPIC_API_KEY": "sk-ant"}))

	provider := cfg.ProviderByName("claude")
	if provider == nil || provider.Model == nil {
		t.Fatal("anthropic provider not assembled")
	}

	// wire_api defaults to the protocol family's only choice, expanded into
	// model metadata.
	meta, ok := cfg.ModelMeta(ProviderModelRef{Provider: "claude", Model: "claude-sonnet-4-6"})
	if !ok || meta.WireAPI != "messages" {
		t.Fatalf("model meta = %+v, ok = %v", meta, ok)
	}
	// Reasoning inheritance: the model without an opinion takes the
	// provider-level default.
	spec := meta.Reasoning.DomainSpec()
	if spec.Effort != "medium" || spec.BudgetTokens != 0 {
		t.Fatalf("inherited reasoning = %+v, want medium", spec)
	}

	// Explicit model-level reasoning wins over the provider default.
	meta, ok = cfg.ModelMeta(ProviderModelRef{Provider: "claude", Model: "claude-haiku-4-5"})
	if !ok {
		t.Fatal("haiku meta missing")
	}
	if spec := meta.Reasoning.DomainSpec(); spec.Effort != "off" || spec.Enabled() {
		t.Fatalf("model reasoning = %+v, want off", spec)
	}
}

func TestResolveRef(t *testing.T) {
	cfg := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk-env"}))

	cases := []struct {
		ref  string
		want ProviderModelRef
	}{
		{"deepseek/deepseek-reasoner", ProviderModelRef{"deepseek", "deepseek-reasoner"}},
		{"gpt-5", ProviderModelRef{"openai", "gpt-5"}},  // bare unique model
		{"openai", ProviderModelRef{"openai", "gpt-5"}}, // bare provider → its default
		{" deepseek/deepseek-chat ", ProviderModelRef{"deepseek", "deepseek-chat"}},
	}
	for _, tc := range cases {
		got, err := cfg.ResolveRef(tc.ref)
		if err != nil {
			t.Errorf("ResolveRef(%q) error = %v", tc.ref, err)
			continue
		}
		if got != tc.want {
			t.Errorf("ResolveRef(%q) = %+v, want %+v", tc.ref, got, tc.want)
		}
	}

	for _, ref := range []string{"", "nosuch/model", "deepseek/nosuch", "nosuch-model"} {
		if _, err := cfg.ResolveRef(ref); err == nil {
			t.Errorf("ResolveRef(%q) should fail", ref)
		}
	}
}

func TestResolveRefAmbiguous(t *testing.T) {
	cfg := loadFile(t, `
providers:
  - name: a
    base_url: https://a.example.com/v1
    api_key: sk
    models: [{name: shared}]
  - name: b
    base_url: https://b.example.com/v1
    api_key: sk
    models: [{name: shared}]
`, noEnv)
	_, err := cfg.ResolveRef("shared")
	if err == nil || !strings.Contains(err.Error(), "ambiguous") {
		t.Fatalf("ResolveRef(shared) error = %v, want ambiguity with candidates", err)
	}
	if !strings.Contains(err.Error(), "a/shared") || !strings.Contains(err.Error(), "b/shared") {
		t.Fatalf("ambiguity error should list candidates: %v", err)
	}
}

func TestLoadValidationErrors(t *testing.T) {
	cases := []struct {
		name    string
		yaml    string
		wantErr string
	}{
		{"unknown field", "providers: []\nnosuchkey: 1", "nosuchkey"},
		{"duplicate provider", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\n  - {name: x, base_url: 'https://b.com', api_key: k, models: [{name: m}]}", "duplicate provider"},
		{"bad type", "providers:\n  - {name: x, type: gemini, base_url: 'https://a.com', api_key: k, models: [{name: m}]}", "unsupported type"},
		{"bad anthropic wire_api", "providers:\n  - {name: x, type: anthropic, base_url: 'https://a.com', api_key: k, wire_api: chat, models: [{name: m}]}", "messages"},
		{"bad anthropic model wire_api", "providers:\n  - {name: x, type: anthropic, base_url: 'https://a.com', api_key: k, models: [{name: m, wire_api: responses}]}", "messages"},
		{"bad reasoning effort", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m, reasoning: {effort: extreme}}]}", "reasoning.effort"},
		{"bad provider reasoning effort", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, reasoning: {effort: max}, models: [{name: m}]}", "reasoning.effort"},
		{"negative reasoning budget", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m, reasoning: {budget_tokens: -1}}]}", "budget_tokens"},
		{"bad auth_type", "providers:\n  - {name: x, type: anthropic, base_url: 'https://a.com', api_key: k, auth_type: digest, models: [{name: m}]}", "auth_type"},
		{"auth_type on openai", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, auth_type: bearer, models: [{name: m}]}", "only meaningful for anthropic"},
		{"missing base_url", "providers:\n  - {name: x, api_key: k, models: [{name: m}]}", "base_url is required"},
		{"bad base_url", "providers:\n  - {name: x, base_url: '::bad', api_key: k, models: [{name: m}]}", ""},
		{"empty models", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: []}", "at least one model"},
		{"duplicate model", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}, {name: m}]}", "duplicate model"},
		{"bad default_model", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, default_model: nope, models: [{name: m}]}", "default_model"},
		{"bad wire_api", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, wire_api: grpc, models: [{name: m}]}", "wire_api"},
		{"bad model wire_api", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m, wire_api: grpc}]}", "wire_api"},
		{"key conflict", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, api_key_env: E, models: [{name: m}]}", "mutually exclusive"},
		{"env not set", "providers:\n  - {name: x, base_url: 'https://a.com', api_key_env: MISSING_VAR, models: [{name: m}]}", "not set"},
		{"bad default ref", "default: nope\nproviders:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}", "default"},
		{"negative limit", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\nlimits:\n  max_cost_usd: -1", "max_cost_usd"},
		{"removed limit max_turns", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\nlimits:\n  max_turns: 50", "max_turns"},
		{"notice level above trigger", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\ncontext:\n  notice_levels: [0.6, 0.85]", "notice_levels"},
		{"target above trigger", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\ncontext:\n  compact_trigger_ratio: 0.7\n  compact_target_ratio: 0.8", "compact_target_ratio"},
		{"bad model window_utilization", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m, window_utilization: 1.5}]}", "window_utilization"},
		{"negative runaway threshold", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\nrunaway:\n  max_repeated_calls: -1", "runaway"},
		{"removed limit max_wall_time", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\nlimits:\n  max_wall_time: 30m", "max_wall_time"},
		{"bad stall duration", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m}]}\nrunaway:\n  stall_timeout: soon", "stall_timeout"},
		{"negative context window", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: m, context_window: -1}]}", ">= 0"},
		{"slash in provider name", "providers:\n  - {name: a/b, base_url: 'https://a.com', api_key: k, models: [{name: m}]}", "must not contain '/'"},
		{"slash in model name", "providers:\n  - {name: x, base_url: 'https://a.com', api_key: k, models: [{name: a/b}]}", "must not contain '/'"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := Load(writeConfig(t, tc.yaml), LoadOptions{RequireProviders: true}, noEnv)
			if err == nil {
				t.Fatalf("Load() should fail")
			}
			if tc.wantErr != "" && !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("Load() error = %v, want substring %q", err, tc.wantErr)
			}
		})
	}
}

func TestLoadEmptyFile(t *testing.T) {
	home := writeConfig(t, "# nothing but a comment\n")

	// Offline commands tolerate an empty file.
	cfg, err := Load(home, LoadOptions{}, noEnv)
	if err != nil {
		t.Fatalf("Load(empty, offline) error = %v", err)
	}
	if len(cfg.Providers) != 0 {
		t.Fatalf("providers = %d, want 0", len(cfg.Providers))
	}

	// Agent entries still fail fast with the embedded example.
	if _, err := Load(home, LoadOptions{RequireProviders: true}, noEnv); err == nil ||
		!strings.Contains(err.Error(), "at least one provider") {
		t.Fatalf("Load(empty, agent) error = %v, want providers-required", err)
	}
}

func TestLoadMissingFile(t *testing.T) {
	home := t.TempDir() // no config.yaml inside

	// Agent entry points fail fast with a copy-pasteable example.
	_, err := Load(home, LoadOptions{RequireProviders: true}, noEnv)
	if err == nil || !strings.Contains(err.Error(), "not found") {
		t.Fatalf("Load() error = %v, want not-found with example", err)
	}
	if !strings.Contains(err.Error(), "providers:") {
		t.Fatalf("error should embed the minimal example: %v", err)
	}

	// Offline commands tolerate a missing file and fall back to defaults.
	cfg, err := Load(home, LoadOptions{}, noEnv)
	if err != nil {
		t.Fatalf("Load(offline) error = %v", err)
	}
	if len(cfg.Providers) != 0 {
		t.Fatalf("offline providers = %d, want 0", len(cfg.Providers))
	}
}

// Regression (REVIEW H6): a model-level wire_api override must produce its
// own model instance. Before the fix, every model silently spoke the
// provider-level wire API — deepseek-reasoner configured "responses" still
// hit /chat/completions with no warning.
func TestResolveModelWireAPIOverrideBuildsDistinctInstance(t *testing.T) {
	cfg := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk-env"}))
	deepseek := cfg.ProviderByName("deepseek")
	if deepseek == nil {
		t.Fatal("deepseek provider missing")
	}
	chat := deepseek.ModelFor("deepseek-chat")
	responses := deepseek.ModelFor("deepseek-reasoner")
	if chat == nil || responses == nil {
		t.Fatal("ModelFor returned nil")
	}
	if chat == responses {
		t.Fatal("models with different wire_api must get distinct model instances")
	}
	// Unknown names degrade to the provider default instance.
	if deepseek.ModelFor("unknown-model") != deepseek.Model {
		t.Fatal("unknown model should degrade to the provider default instance")
	}
}

func TestResolveLimitsOverlay(t *testing.T) {
	cfg := loadFile(t, twoProviderYAML+`
limits:
  max_cost_usd: 1.5
  max_tokens: 1000000
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))

	if cfg.Limits.MaxEstimatedCostUSD != 1.5 {
		t.Errorf("MaxEstimatedCostUSD = %v", cfg.Limits.MaxEstimatedCostUSD)
	}
	if cfg.Limits.MaxTokens != 1_000_000 {
		t.Errorf("MaxTokens = %v, want 1000000", cfg.Limits.MaxTokens)
	}
	if cfg.Limits.MaxToolOutputBytes != 48*1024 {
		t.Errorf("MaxToolOutputBytes = %d, want built-in default 48KB", cfg.Limits.MaxToolOutputBytes)
	}
}

func TestResolveContextOverlay(t *testing.T) {
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if !reflect.DeepEqual(def.Context, domain.DefaultContextConfig()) {
		t.Fatalf("default context = %+v, want %+v", def.Context, domain.DefaultContextConfig())
	}

	cfg := loadFile(t, twoProviderYAML+`
context:
  utilization: 0.9
  compact_trigger_ratio: 0.7
  compact_target_ratio: 0.4
  notice_levels: [0.5, 0.65]
runaway:
  max_repeated_calls: 4
  stall_warn_turns: 0
  stall_timeout: 5m
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	wantContext := domain.ContextConfig{
		Utilization: 0.9, CompactTriggerRatio: 0.7, CompactTargetRatio: 0.4,
		NoticeLevels: []float64{0.5, 0.65},
	}
	if !reflect.DeepEqual(cfg.Context, wantContext) {
		t.Fatalf("context = %+v, want %+v", cfg.Context, wantContext)
	}
	wantRunaway := domain.RunawayConfig{MaxRepeatedCalls: 4, MaxConsecutiveFailures: 5, StallWarnTurns: 0, StallTimeout: 5 * time.Minute}
	if cfg.Runaway != wantRunaway {
		t.Fatalf("runaway = %+v, want %+v", cfg.Runaway, wantRunaway)
	}
}

func TestResolveRulesDefaultsAndOverrides(t *testing.T) {
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	want := ResolvedRules{Enabled: true, Builtin: true, Project: true, ProjectAllow: false, PersistRemembered: true}
	if def.Rules != want {
		t.Fatalf("default rules = %+v, want %+v", def.Rules, want)
	}

	cfg := loadFile(t, twoProviderYAML+`
rules:
  project: false
  project_allow: true
  persist_remembered: false
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Rules.Project || !cfg.Rules.ProjectAllow || cfg.Rules.PersistRemembered {
		t.Fatalf("rules = %+v", cfg.Rules)
	}
	if !cfg.Rules.Enabled || !cfg.Rules.Builtin {
		t.Fatalf("untouched rule defaults flipped: %+v", cfg.Rules)
	}
}

func TestResolveLoggingDefaultsAndOverrides(t *testing.T) {
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Logging.MaxFileBytes != logging.DefaultMaxFileBytes ||
		def.Logging.MaxTotalBytes != logging.DefaultMaxTotalBytes {
		t.Fatalf("default logging quotas = %+v", def.Logging)
	}

	cfg := loadFile(t, twoProviderYAML+`
logging:
  max_file_mb: 100
  max_total_mb: 500
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Logging.MaxFileBytes != 100<<20 || cfg.Logging.MaxTotalBytes != 500<<20 {
		t.Fatalf("logging quotas = %+v, want 100MiB/500MiB in bytes", cfg.Logging)
	}

	// Negative disables the limit (passed through as negative bytes).
	cfg = loadFile(t, twoProviderYAML+`
logging:
  max_file_mb: -1
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Logging.MaxFileBytes >= 0 {
		t.Fatalf("negative max_file_mb must pass through as unlimited: %+v", cfg.Logging)
	}
}

func TestResolveApprovalMode(t *testing.T) {
	// Absent defaults to on-request (PERMISSION_DESIGN §4.3).
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Approval.Mode != "on-request" {
		t.Fatalf("default approval mode = %q, want on-request", def.Approval.Mode)
	}

	cfg := loadFile(t, twoProviderYAML+`
approval:
  mode: danger-only
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Approval.Mode != "danger-only" {
		t.Fatalf("approval mode = %q", cfg.Approval.Mode)
	}

	cfg = loadFile(t, twoProviderYAML+`
approval:
  mode: never
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Approval.Mode != "never" {
		t.Fatalf("approval mode = %q", cfg.Approval.Mode)
	}
}

func TestResolveApprovalModeRejectsUnknown(t *testing.T) {
	_, err := Load(writeConfig(t, twoProviderYAML+`
approval:
  mode: yolo
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "approval.mode") {
		t.Fatalf("err = %v, want approval.mode validation error", err)
	}
}

func TestResolveSubagentDefaultsAndOverrides(t *testing.T) {
	// Absent: enabled, no token cap override, 8192 output cap, follows the
	// turn's model.
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if !def.Subagent.Enabled || def.Subagent.MaxTokens != 0 || def.Subagent.Model != nil {
		t.Fatalf("default subagent = %+v, want enabled/inherit/follow", def.Subagent)
	}
	if def.Subagent.MaxOutputTokens != 8192 {
		t.Fatalf("default subagent max_output_tokens = %d, want 8192", def.Subagent.MaxOutputTokens)
	}

	cfg := loadFile(t, twoProviderYAML+`
subagent:
  max_tokens: 50000
  max_output_tokens: 4000
  model: openai/gpt-5
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Subagent.MaxTokens != 50_000 {
		t.Fatalf("subagent max_tokens = %d, want 50000", cfg.Subagent.MaxTokens)
	}
	if cfg.Subagent.MaxOutputTokens != 4000 {
		t.Fatalf("subagent max_output_tokens = %d, want 4000", cfg.Subagent.MaxOutputTokens)
	}
	if cfg.Subagent.Model == nil || cfg.Subagent.Model.String() != "openai/gpt-5" {
		t.Fatalf("subagent model = %v, want openai/gpt-5", cfg.Subagent.Model)
	}

	// Explicit 0 inherits the parent limits.
	inherit := loadFile(t, twoProviderYAML+`
subagent:
  max_output_tokens: 0
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if inherit.Subagent.MaxOutputTokens != 0 {
		t.Fatalf("subagent max_output_tokens = %d, want 0 (inherit)", inherit.Subagent.MaxOutputTokens)
	}

	disabled := loadFile(t, twoProviderYAML+`
subagent:
  enabled: false
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if disabled.Subagent.Enabled {
		t.Fatal("subagent.enabled = false must resolve to disabled")
	}

	_, err := Load(writeConfig(t, twoProviderYAML+`
subagent:
  max_tokens: -1
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "subagent.max_tokens") {
		t.Fatalf("err = %v, want subagent.max_tokens validation error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
subagent:
  max_output_tokens: -1
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "subagent.max_output_tokens") {
		t.Fatalf("err = %v, want subagent.max_output_tokens validation error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
subagent:
  model: nope/ghost
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "subagent.model") {
		t.Fatalf("err = %v, want subagent.model validation error", err)
	}
}

func TestResolveSessionsAutoArchive(t *testing.T) {
	// Absent: the archiver is disabled.
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Sessions.AutoArchiveAfter != 0 {
		t.Fatalf("auto_archive_after default = %v, want 0 (disabled)", def.Sessions.AutoArchiveAfter)
	}

	cfg := loadFile(t, twoProviderYAML+`
sessions:
  auto_archive_after: "720h"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Sessions.AutoArchiveAfter != 30*24*time.Hour {
		t.Fatalf("auto_archive_after = %v, want 720h", cfg.Sessions.AutoArchiveAfter)
	}

	// "0" explicitly disables the archiver.
	zero := loadFile(t, twoProviderYAML+`
sessions:
  auto_archive_after: "0"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if zero.Sessions.AutoArchiveAfter != 0 {
		t.Fatalf("auto_archive_after = %v, want 0 (disabled)", zero.Sessions.AutoArchiveAfter)
	}

	_, err := Load(writeConfig(t, twoProviderYAML+`
sessions:
  auto_archive_after: "thirty days"
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "sessions.auto_archive_after") {
		t.Fatalf("err = %v, want sessions.auto_archive_after parse error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
sessions:
  auto_archive_after: "-1h"
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "sessions.auto_archive_after") {
		t.Fatalf("err = %v, want sessions.auto_archive_after validation error", err)
	}
}

func TestResolveSessionsGCArchivedAfter(t *testing.T) {
	// Absent: the purge is disabled.
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Sessions.GCArchivedAfter != 0 {
		t.Fatalf("gc_archived_after default = %v, want 0 (disabled)", def.Sessions.GCArchivedAfter)
	}

	cfg := loadFile(t, twoProviderYAML+`
sessions:
  gc_archived_after: "720h"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Sessions.GCArchivedAfter != 30*24*time.Hour {
		t.Fatalf("gc_archived_after = %v, want 720h", cfg.Sessions.GCArchivedAfter)
	}

	// "0" explicitly disables the purge.
	zero := loadFile(t, twoProviderYAML+`
sessions:
  gc_archived_after: "0"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if zero.Sessions.GCArchivedAfter != 0 {
		t.Fatalf("gc_archived_after = %v, want 0 (disabled)", zero.Sessions.GCArchivedAfter)
	}

	_, err := Load(writeConfig(t, twoProviderYAML+`
sessions:
  gc_archived_after: "thirty days"
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "sessions.gc_archived_after") {
		t.Fatalf("err = %v, want sessions.gc_archived_after parse error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
sessions:
  gc_archived_after: "-1h"
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "sessions.gc_archived_after") {
		t.Fatalf("err = %v, want sessions.gc_archived_after validation error", err)
	}
}

func TestResolveMemoryDefaultsAndOverrides(t *testing.T) {
	// Absent: enabled, follows the default model, built-in pipeline tuning.
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if !def.Memory.Enabled {
		t.Fatal("memory must default to enabled")
	}
	if def.Memory.ExtractModel != nil || def.Memory.ConsolidationModel != nil {
		t.Fatalf("memory models = %+v/%+v, want nil (follow default)", def.Memory.ExtractModel, def.Memory.ConsolidationModel)
	}
	if def.Memory.MaxJobsPerRun != 8 {
		t.Fatalf("max_jobs_per_run = %d, want 8", def.Memory.MaxJobsPerRun)
	}
	if def.Memory.RunInterval != 30*time.Minute {
		t.Fatalf("run_interval = %v, want 30m", def.Memory.RunInterval)
	}
	if def.Memory.MinSessionIdle != time.Hour {
		t.Fatalf("min_session_idle = %v, want 1h", def.Memory.MinSessionIdle)
	}
	if def.Memory.MaxSessionAge != 30*24*time.Hour {
		t.Fatalf("max_session_age = %v, want 720h", def.Memory.MaxSessionAge)
	}

	cfg := loadFile(t, twoProviderYAML+`
memory:
  extract_model: openai/gpt-5
  consolidation_model: deepseek/deepseek-chat
  max_jobs_per_run: 4
  run_interval: "10m"
  min_session_idle: "2h"
  max_session_age: "168h"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Memory.ExtractModel == nil || cfg.Memory.ExtractModel.String() != "openai/gpt-5" {
		t.Fatalf("extract_model = %v, want openai/gpt-5", cfg.Memory.ExtractModel)
	}
	if cfg.Memory.ConsolidationModel == nil || cfg.Memory.ConsolidationModel.String() != "deepseek/deepseek-chat" {
		t.Fatalf("consolidation_model = %v, want deepseek/deepseek-chat", cfg.Memory.ConsolidationModel)
	}
	if cfg.Memory.MaxJobsPerRun != 4 {
		t.Fatalf("max_jobs_per_run = %d, want 4", cfg.Memory.MaxJobsPerRun)
	}
	if cfg.Memory.RunInterval != 10*time.Minute {
		t.Fatalf("run_interval = %v, want 10m", cfg.Memory.RunInterval)
	}
	if cfg.Memory.MinSessionIdle != 2*time.Hour {
		t.Fatalf("min_session_idle = %v, want 2h", cfg.Memory.MinSessionIdle)
	}
	if cfg.Memory.MaxSessionAge != 7*24*time.Hour {
		t.Fatalf("max_session_age = %v, want 168h", cfg.Memory.MaxSessionAge)
	}

	// run_interval "0" runs the pipeline once at startup only.
	once := loadFile(t, twoProviderYAML+`
memory:
  run_interval: "0"
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if once.Memory.RunInterval != 0 {
		t.Fatalf("run_interval = %v, want 0 (startup only)", once.Memory.RunInterval)
	}

	disabled := loadFile(t, twoProviderYAML+`
memory:
  enabled: false
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if disabled.Memory.Enabled {
		t.Fatal("memory.enabled = false must resolve to disabled")
	}

	_, err := Load(writeConfig(t, twoProviderYAML+`
memory:
  extract_model: nope/ghost
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "memory.extract_model") {
		t.Fatalf("err = %v, want memory.extract_model validation error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
memory:
  max_jobs_per_run: 0
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "memory.max_jobs_per_run") {
		t.Fatalf("err = %v, want memory.max_jobs_per_run validation error", err)
	}

	_, err = Load(writeConfig(t, twoProviderYAML+`
memory:
  run_interval: "soon"
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "memory.run_interval") {
		t.Fatalf("err = %v, want memory.run_interval validation error", err)
	}
}

func TestResolveTracing(t *testing.T) {
	// Disabled by default (no host/keys).
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Tracing.Enabled {
		t.Fatal("tracing should be disabled without host and keys")
	}

	cfg := loadFile(t, twoProviderYAML+`
tracing:
  host: https://langfuse.example.com/
  public_key: pk
  secret_key_env: LF_SECRET
  cost_input_usd_per_mtok: 0.15
`, envWith(map[string]string{"OPENAI_API_KEY": "sk", "LF_SECRET": "sk-lf"}))
	tr := cfg.Tracing
	if !tr.Enabled || tr.Host != "https://langfuse.example.com" || tr.SecretKey != "sk-lf" {
		t.Fatalf("tracing = %+v", tr)
	}
	if tr.Environment != "dev" || !tr.IncludeContent {
		t.Fatalf("tracing defaults = %+v", tr)
	}
	if tr.CostInputPerMTok != 0.15 {
		t.Fatalf("cost rate = %v", tr.CostInputPerMTok)
	}
}

func TestWriteTemplate(t *testing.T) {
	path := filepath.Join(t.TempDir(), "loom", "custom-name.yaml")
	if err := WriteTemplate(path); err != nil {
		t.Fatalf("WriteTemplate() error = %v", err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("template mode = %o, want 600", info.Mode().Perm())
	}

	// The generated template must itself load (after the user fills a key).
	raw, _ := os.ReadFile(path)
	content := strings.Replace(string(raw), "#   api_key: <your-api-key>", "api_key: sk-real", 1)
	if _, err := Load(writeConfig(t, content), LoadOptions{RequireProviders: true}, noEnv); err != nil {
		t.Fatalf("generated template does not load: %v", err)
	}

	// Never overwrite an existing file.
	if err := WriteTemplate(path); err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("second WriteTemplate() error = %v, want already-exists", err)
	}
}

// TestTemplateCoversSchemaSections is the M35 regression lock: the init
// template doubles as the user-facing configuration reference, so every
// top-level schema section must appear in it (memory/image/logging/
// workspaces and subagent.max_output_tokens had drifted out). It also
// verifies the template's concrete values resolve to the defaults its
// comments document.
func TestTemplateCoversSchemaSections(t *testing.T) {
	var raw map[string]any
	if err := yaml.Unmarshal([]byte(template), &raw); err != nil {
		t.Fatalf("template is not valid YAML: %v", err)
	}
	// Mirror of File's top-level yaml keys (schema.go).
	for _, section := range []string{
		"default", "providers", "limits", "context", "runaway", "prompt",
		"skills", "rules", "approval", "tracing", "share", "logging",
		"ui", "subagent", "memory", "image", "mcp_servers", "workspaces",
	} {
		if _, ok := raw[section]; !ok {
			t.Fatalf("template missing top-level section %q", section)
		}
	}
	sub, _ := raw["subagent"].(map[string]any)
	if _, ok := sub["max_output_tokens"]; !ok {
		t.Fatal("template subagent section missing max_output_tokens")
	}

	// The template's concrete values must resolve to the documented
	// defaults.
	content := strings.Replace(template, "#   api_key: <your-api-key>", "api_key: sk-real", 1)
	cfg, err := Load(writeConfig(t, content), LoadOptions{RequireProviders: true}, noEnv)
	if err != nil {
		t.Fatalf("template does not load: %v", err)
	}
	if !cfg.Memory.Enabled || cfg.Memory.MaxJobsPerRun != 8 ||
		cfg.Memory.RunInterval != 30*time.Minute ||
		cfg.Memory.MinSessionIdle != time.Hour ||
		cfg.Memory.MaxSessionAge != 30*24*time.Hour {
		t.Fatalf("memory defaults = %+v", cfg.Memory)
	}
	if cfg.Logging.MaxFileBytes != logging.DefaultMaxFileBytes ||
		cfg.Logging.MaxTotalBytes != logging.DefaultMaxTotalBytes {
		t.Fatalf("logging defaults = %+v", cfg.Logging)
	}
	if cfg.Subagent.MaxOutputTokens != 8192 {
		t.Fatalf("subagent.max_output_tokens = %d, want 8192", cfg.Subagent.MaxOutputTokens)
	}
	if cfg.Image.Enabled {
		t.Fatalf("image must stay disabled with empty provider/model: %+v", cfg.Image)
	}
	if len(cfg.Workspaces) != 0 {
		t.Fatalf("workspaces = %+v, want empty", cfg.Workspaces)
	}
}

func TestEnsureFirstRunConfig(t *testing.T) {
	fakeHome := t.TempDir()
	t.Setenv("HOME", fakeHome)
	def, err := DefaultHomeDir()
	if err != nil {
		t.Fatal(err)
	}
	defaultPath := ConfigPathForHome(def)

	// A missing default home gets a fresh template (0600) and reports it.
	created, err := EnsureFirstRunConfig(def)
	if err != nil {
		t.Fatalf("EnsureFirstRunConfig() error = %v", err)
	}
	if !created {
		t.Fatal("EnsureFirstRunConfig() created = false, want true")
	}
	info, err := os.Stat(defaultPath)
	if err != nil {
		t.Fatalf("stat created config: %v", err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("created config mode = %o, want 600", info.Mode().Perm())
	}

	// An existing file is never touched: no overwrite, no error.
	created, err = EnsureFirstRunConfig(def)
	if err != nil || created {
		t.Fatalf("EnsureFirstRunConfig(existing) = (%v, %v), want (false, nil)", created, err)
	}

	// An explicit non-default home is never auto-created.
	explicit := filepath.Join(fakeHome, "custom")
	created, err = EnsureFirstRunConfig(explicit)
	if err != nil || created {
		t.Fatalf("EnsureFirstRunConfig(explicit) = (%v, %v), want (false, nil)", created, err)
	}
	if _, err := os.Stat(ConfigPathForHome(explicit)); !os.IsNotExist(err) {
		t.Fatalf("explicit home's config was created unexpectedly: %v", err)
	}
}
