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
)

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

func writeConfig(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
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
	path := writeConfig(t, "# nothing but a comment\n")

	// Offline commands tolerate an empty file.
	cfg, err := Load(path, LoadOptions{}, noEnv)
	if err != nil {
		t.Fatalf("Load(empty, offline) error = %v", err)
	}
	if len(cfg.Providers) != 0 {
		t.Fatalf("providers = %d, want 0", len(cfg.Providers))
	}

	// Agent entries still fail fast with the embedded example.
	if _, err := Load(path, LoadOptions{RequireProviders: true}, noEnv); err == nil ||
		!strings.Contains(err.Error(), "at least one provider") {
		t.Fatalf("Load(empty, agent) error = %v, want providers-required", err)
	}
}

func TestLoadMissingFile(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "config.yaml")

	// Agent entry points fail fast with a copy-pasteable example.
	_, err := Load(missing, LoadOptions{RequireProviders: true}, noEnv)
	if err == nil || !strings.Contains(err.Error(), "not found") {
		t.Fatalf("Load() error = %v, want not-found with example", err)
	}
	if !strings.Contains(err.Error(), "providers:") {
		t.Fatalf("error should embed the minimal example: %v", err)
	}

	// Offline commands tolerate a missing file and fall back to defaults.
	cfg, err := Load(missing, LoadOptions{}, noEnv)
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

func TestResolveApprovalMode(t *testing.T) {
	// Absent defaults to on-request (PERMISSION_DESIGN §4.3).
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if def.Approval.Mode != "on-request" {
		t.Fatalf("default approval mode = %q, want on-request", def.Approval.Mode)
	}

	cfg := loadFile(t, twoProviderYAML+`
approval:
  mode: unless-trusted
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Approval.Mode != "unless-trusted" {
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
	// Absent: enabled, no token cap override, follows the turn's model.
	def := loadFile(t, twoProviderYAML, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if !def.Subagent.Enabled || def.Subagent.MaxTokens != 0 || def.Subagent.Model != nil {
		t.Fatalf("default subagent = %+v, want enabled/inherit/follow", def.Subagent)
	}

	cfg := loadFile(t, twoProviderYAML+`
subagent:
  max_tokens: 50000
  model: openai/gpt-5
`, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if cfg.Subagent.MaxTokens != 50_000 {
		t.Fatalf("subagent max_tokens = %d, want 50000", cfg.Subagent.MaxTokens)
	}
	if cfg.Subagent.Model == nil || cfg.Subagent.Model.String() != "openai/gpt-5" {
		t.Fatalf("subagent model = %v, want openai/gpt-5", cfg.Subagent.Model)
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
  model: nope/ghost
`), LoadOptions{RequireProviders: true}, envWith(map[string]string{"OPENAI_API_KEY": "sk"}))
	if err == nil || !strings.Contains(err.Error(), "subagent.model") {
		t.Fatalf("err = %v, want subagent.model validation error", err)
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
	content := strings.Replace(string(raw), "api_key: sk-xxxxxxxx", "api_key: sk-real", 1)
	if _, err := Load(writeConfig(t, content), LoadOptions{RequireProviders: true}, noEnv); err != nil {
		t.Fatalf("generated template does not load: %v", err)
	}

	// Never overwrite an existing file.
	if err := WriteTemplate(path); err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("second WriteTemplate() error = %v, want already-exists", err)
	}
}
