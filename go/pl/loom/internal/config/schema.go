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

// Package config loads loom's configuration from the YAML file at
// ~/.loom/config.yaml — the single configuration source (see
// docs/CONFIG_DESIGN.md). The only env reads are secret references
// (api_key_env), which resolve secret *values*, not configuration.
package config

import (
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// File is the raw YAML schema of ~/.loom/config.yaml. Pointer fields
// distinguish "unset" (nil → built-in default) from an explicit value —
// e.g. limits.max_turns: 0 disables the turn budget and must not be
// mistaken for "not present".
//
// The schema depends on domain only for the vendor-neutral reasoning spec
// conversion (Reasoning.DomainSpec); nothing here performs I/O.
type File struct {
	// Default selects the startup model: "provider/model", a bare model
	// name (must be unique across providers), or a bare provider name (its
	// default_model). Empty means providers[0]'s default model.
	Default string `yaml:"default"`

	// Providers is the only required section: at least one entry.
	Providers []Provider `yaml:"providers"`

	Limits   Limits   `yaml:"limits"`
	Context  Context  `yaml:"context"`
	Runaway  Runaway  `yaml:"runaway"`
	Prompt   Prompt   `yaml:"prompt"`
	Skills   Skills   `yaml:"skills"`
	Rules    Rules    `yaml:"rules"`
	Approval Approval `yaml:"approval"`
	Tracing  Tracing  `yaml:"tracing"`
	Storage    Storage              `yaml:"storage"`
	UI         UI                   `yaml:"ui"`
	Subagent   Subagent             `yaml:"subagent"`
	Memory     Memory               `yaml:"memory"`
	MCPServers map[string]MCPServer `yaml:"mcp_servers"`
}

// Provider describes one model endpoint and its model catalog. Type selects
// the wire protocol family: "openai" (OpenAI-compatible chat/responses, the
// default) or "anthropic" (Messages API).
type Provider struct {
	Name    string `yaml:"name"`
	Type    string `yaml:"type"`
	BaseURL string `yaml:"base_url"`
	// APIKey holds the secret inline; APIKeyEnv names an environment
	// variable to read it from. The two are mutually exclusive.
	APIKey    string `yaml:"api_key"`
	APIKeyEnv string `yaml:"api_key_env"`
	// APIVersion is the protocol version header for providers that version
	// their API out-of-band (Anthropic's anthropic-version); empty selects
	// the provider implementation's pinned default.
	APIVersion string `yaml:"api_version"`
	// AuthType selects the credential header for providers with more than
	// one authentication convention: "x-api-key" (default) or "bearer".
	// Only meaningful for anthropic providers today.
	AuthType string `yaml:"auth_type"`
	// WireAPI is the provider-level default ("chat" or "responses");
	// models may override it. Empty means "chat".
	WireAPI      string `yaml:"wire_api"`
	MaxRetries   *int   `yaml:"max_retries"`
	DefaultModel string `yaml:"default_model"`
	// Reasoning is the provider-level default reasoning (thinking) intent;
	// models may override it.
	Reasoning Reasoning `yaml:"reasoning"`
	Models    []Model   `yaml:"models"`
}

// Model is one selectable model with its metadata.
type Model struct {
	Name          string `yaml:"name"`
	ContextWindow int64  `yaml:"context_window"`
	// WindowUtilization overrides context.utilization for this model (e.g.
	// when a gateway overstates the window); nil inherits the global value.
	WindowUtilization *float64 `yaml:"window_utilization"`
	// MaxOutputTokens caps the request parameter (model capability); the
	// agent budget guardrail lives in limits.max_output_tokens.
	MaxOutputTokens int64     `yaml:"max_output_tokens"`
	WireAPI         string    `yaml:"wire_api"`
	Reasoning       Reasoning `yaml:"reasoning"`
}

// Reasoning configures the model's reasoning (thinking) intent in
// vendor-neutral terms; each provider maps it onto its wire representation
// (Anthropic thinking.budget_tokens, OpenAI reasoning_effort).
type Reasoning struct {
	// Effort is "off", "low", "medium", or "high"; empty means the
	// provider decides.
	Effort string `yaml:"effort"`
	// BudgetTokens is an explicit reasoning token budget; it wins over the
	// effort-derived budget where the wire API supports one.
	BudgetTokens int64 `yaml:"budget_tokens"`
}

// DomainSpec converts the configuration into the vendor-neutral domain spec.
func (r Reasoning) DomainSpec() domain.ReasoningSpec {
	return domain.ReasoningSpec{
		Effort:       domain.ReasoningEffort(strings.TrimSpace(r.Effort)),
		BudgetTokens: r.BudgetTokens,
	}
}

// Limits mirrors domain.Limits; nil fields keep the built-in default.
// Only scarce resources are budgeted (session-cumulative tokens, cost) —
// turn/tool-call count quotas and the per-prompt wall-clock cap were
// removed by design (docs/CONTEXT_DESIGN.md §4.4.3) and are rejected as
// unknown fields at load.
type Limits struct {
	MaxInputTokens  *int64   `yaml:"max_input_tokens"`
	MaxOutputTokens *int64   `yaml:"max_output_tokens"`
	MaxCostUSD      *float64 `yaml:"max_cost_usd"`
	// MaxTokens budgets session-cumulative metered tokens; nil keeps the
	// default (0 = unlimited, opt-in).
	MaxTokens          *int64 `yaml:"max_tokens"`
	MaxToolOutputBytes *int64 `yaml:"max_tool_output_bytes"`
	MaxArtifactBytes   *int64 `yaml:"max_artifact_bytes"`
}

// Context mirrors domain.ContextConfig; nil fields keep the built-in
// default. All values are ratios of the model's effective context window.
type Context struct {
	Utilization         *float64  `yaml:"utilization"`
	CompactTriggerRatio *float64  `yaml:"compact_trigger_ratio"`
	CompactTargetRatio  *float64  `yaml:"compact_target_ratio"`
	NoticeLevels        []float64 `yaml:"notice_levels"`
}

// Runaway mirrors domain.RunawayConfig; nil fields keep the built-in
// default.
type Runaway struct {
	MaxRepeatedCalls       *int `yaml:"max_repeated_calls"`
	MaxConsecutiveFailures *int `yaml:"max_consecutive_failures"`
	StallWarnTurns         *int `yaml:"stall_warn_turns"`
	// StallTimeout uses Go duration syntax ("15m", "1h"); empty keeps the
	// default, "0" disables the stall watchdog.
	StallTimeout string `yaml:"stall_timeout"`
}

// Prompt configures the system prompt.
type Prompt struct {
	Extra          string        `yaml:"extra"`
	DisableBuiltin bool          `yaml:"disable_builtin"`
	Managed        ManagedPrompt `yaml:"managed"`
}

// ManagedPrompt selects a Langfuse-managed prompt (requires tracing).
type ManagedPrompt struct {
	Name  string `yaml:"name"`
	Label string `yaml:"label"`
}

// Skills configures skill discovery. Enabled is nil-typed so "absent"
// defaults to true while an explicit false disables.
type Skills struct {
	Enabled    *bool    `yaml:"enabled"`
	ExtraRoots []string `yaml:"extra_roots"`
}

// Rules configures the declarative permission rule layers. All bools are
// nil-typed: absent keeps the built-in default (documented per field in
// resolve.go).
type Rules struct {
	Enabled           *bool `yaml:"enabled"`
	Builtin           *bool `yaml:"builtin"`
	Project           *bool `yaml:"project"`
	ProjectAllow      *bool `yaml:"project_allow"`
	PersistRemembered *bool `yaml:"persist_remembered"`
}

// Approval configures the baseline approval strategy
// (docs/PERMISSION_DESIGN.md §4.3). Mode selects how calls with no rule
// or session memory are decided: "on-request" (default: sandboxed,
// non-dangerous commands run without prompting), "unless-dangerous"
// (blacklist mode: everything the sandbox confines — sandboxed commands
// with network grants, workspace writes — runs without prompting; only
// danger-listed commands, complex shells, and escalations prompt),
// "unless-trusted" (legacy: every unmatched R2+ call prompts), or
// "never" (unattended: sandboxed calls run, escalations are denied).
type Approval struct {
	Mode string `yaml:"mode"`
}

// Tracing configures Langfuse observability. Keys follow the same
// inline-or-env-reference rule as provider API keys.
type Tracing struct {
	Host           string `yaml:"host"`
	PublicKey      string `yaml:"public_key"`
	PublicKeyEnv   string `yaml:"public_key_env"`
	SecretKey      string `yaml:"secret_key"`
	SecretKeyEnv   string `yaml:"secret_key_env"`
	Environment    string `yaml:"environment"`
	IncludeContent *bool  `yaml:"include_content"`
	User           string `yaml:"user"`
	// Cost rates are USD per million tokens; non-positive disables cost
	// attribution.
	CostInputPerMTok  float64 `yaml:"cost_input_usd_per_mtok"`
	CostOutputPerMTok float64 `yaml:"cost_output_usd_per_mtok"`
}

// Storage configures on-disk locations.
type Storage struct {
	SessionDB string `yaml:"session_db"`
}

// UI configures the terminal frontend.
type UI struct {
	Icons     string `yaml:"icons"`
	AltScreen bool   `yaml:"alt_screen"`
	// Keymap overrides default key bindings (docs/VIM_UI_DESIGN.md §5.2):
	// context → action → replacement key, e.g.
	// {chat: {search_transcript: "ctrl+s"}}. Unknown contexts/actions
	// and conflicting keys are ignored with a status-bar warning.
	Keymap map[string]map[string]string `yaml:"keymap"`
}

// Subagent configures the delegate_task sub-agent
// (docs/SUBAGENT_DESIGN.md §7). Enabled is nil-typed so "absent"
// defaults to true while an explicit false removes the tool from the
// registry entirely.
type Subagent struct {
	Enabled *bool `yaml:"enabled"`
	// MaxTokens caps the child run's session-cumulative metered tokens;
	// nil/0 inherits limits.max_tokens. The child's consumption is also
	// folded back into the parent run's budget, so delegation is never a
	// budget loophole.
	MaxTokens *int64 `yaml:"max_tokens"`
	// MaxOutputTokens caps each child model RESPONSE. Sub-agent answers
	// should be concise; the per-request cap is also a latency knob — a
	// truncation is only discovered after the whole output budget has
	// streamed. nil = 8192 (reasoning headroom included); explicit 0
	// inherits limits.max_output_tokens.
	MaxOutputTokens *int64 `yaml:"max_output_tokens"`
	// Model pins the sub-agent to a specific "provider/model" (or a bare
	// model/provider name); empty follows the current turn's model.
	Model string `yaml:"model"`
}

// Memory configures the long-term memory system (docs/MEMORY_DESIGN.md).
// Enabled is nil-typed so "absent" defaults to true while an explicit
// false disables extraction, consolidation, and tool registration.
type Memory struct {
	Enabled *bool `yaml:"enabled"`
}

// MCPServer configures one MCP server subprocess connected over the stdio
// transport. The key in MCPServers is the server name used for
// log attribution and tool name qualification (mcp__{server}__{tool}).
// Only the stdio transport is supported (SSE/streamable HTTP may follow).
type MCPServer struct {
	Command string            `yaml:"command"`
	Args    []string          `yaml:"args"`
	Env     map[string]string `yaml:"env"`
	Cwd     string            `yaml:"cwd"`
	// StartupTimeoutSec bounds spawn+initialize (default 30s);
	// ToolTimeoutSec bounds one tools/call (default 300s).
	StartupTimeoutSec float64 `yaml:"startup_timeout_sec"`
	ToolTimeoutSec    float64 `yaml:"tool_timeout_sec"`
	// EnabledTools/DisabledTools filter the discovered catalog by the
	// server-local tool names. EnabledTools nil means "all".
	EnabledTools  []string `yaml:"enabled_tools"`
	DisabledTools []string `yaml:"disabled_tools"`
}
