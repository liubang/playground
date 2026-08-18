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
// <loom home>/config.yaml — the single configuration source (see
// docs/CONFIG_DESIGN.md). The loom home (data root) is located by
// LOOM_HOME, defaulting to ~/.loom. The only other env reads are
// secret references (api_key_env), which resolve secret *values*, not
// configuration, plus $HOME (via os.UserHomeDir) for the default home.
package config

import (
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/mcp"
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
	Default string `yaml:"default,omitempty"`

	// Providers is the only required section: at least one entry.
	Providers []Provider `yaml:"providers"`

	Limits   Limits   `yaml:"limits,omitempty"`
	Context  Context  `yaml:"context,omitempty"`
	Runaway  Runaway  `yaml:"runaway,omitempty"`
	Prompt   Prompt   `yaml:"prompt,omitempty"`
	Skills   Skills   `yaml:"skills,omitempty"`
	Tools    Tools    `yaml:"tools,omitempty"`
	Rules    Rules    `yaml:"rules,omitempty"`
	Approval Approval `yaml:"approval,omitempty"`
	Tracing  Tracing  `yaml:"tracing,omitempty"`
	Share    Share    `yaml:"share,omitempty"`
	Logging  Logging  `yaml:"logging,omitempty"`
	UI       UI       `yaml:"ui,omitempty"`
	Subagent Subagent `yaml:"subagent,omitempty"`
	Memory   Memory   `yaml:"memory,omitempty"`
	Image    Image    `yaml:"image,omitempty"`
	Browser  Browser  `yaml:"browser,omitempty"`
	// KnowledgeBase configures the minisearch-backed knowledge base tools
	// (kb_search/kb_read). The section is opt-in: absent means disabled,
	// and an unconfigured deployment never advertises the tools to the
	// model. Unlike provider keys, the credential lives inline (no env
	// indirection): a reader-role key is all that's needed, and the file
	// already holds the LLM provider keys.
	KnowledgeBase KnowledgeBase        `yaml:"knowledge_base,omitempty"`
	MCPServers    map[string]MCPServer `yaml:"mcp_servers,omitempty"`
	// Workspaces pre-registers project workspaces at startup
	// (docs/WORKSPACE_DESIGN.md §10). Optional; the startup directory is
	// always registered as the default workspace regardless of this list.
	Workspaces []WorkspaceSpec `yaml:"workspaces,omitempty"`
}

// WorkspaceSpec names one pre-registered workspace. Root supports a leading
// "~" for the user's home directory; existence is validated at registration.
type WorkspaceSpec struct {
	Name string `yaml:"name,omitempty"`
	Root string `yaml:"root"`
}

// Provider describes one model endpoint and its model catalog. Type selects
// the wire protocol family: "openai" (OpenAI-compatible chat/responses, the
// default) or "anthropic" (Messages API).
type Provider struct {
	Name    string `yaml:"name"`
	Type    string `yaml:"type,omitempty"`
	BaseURL string `yaml:"base_url"`
	// APIKey holds the secret inline; APIKeyEnv names an environment
	// variable to read it from. The two are mutually exclusive.
	APIKey    string `yaml:"api_key,omitempty"`
	APIKeyEnv string `yaml:"api_key_env,omitempty"`
	// APIVersion is the protocol version header for providers that version
	// their API out-of-band (Anthropic's anthropic-version); empty selects
	// the provider implementation's pinned default.
	APIVersion string `yaml:"api_version,omitempty"`
	// AuthType selects the credential header for providers with more than
	// one authentication convention: "x-api-key" (default) or "bearer".
	// Only meaningful for anthropic providers today.
	AuthType string `yaml:"auth_type,omitempty"`
	// WireAPI is the provider-level default ("chat" or "responses");
	// models may override it. Empty means "chat".
	WireAPI      string `yaml:"wire_api,omitempty"`
	MaxRetries   *int   `yaml:"max_retries,omitempty"`
	DefaultModel string `yaml:"default_model,omitempty"`
	// Reasoning is the provider-level default reasoning (thinking) intent;
	// models may override it.
	Reasoning Reasoning `yaml:"reasoning,omitempty"`
	Models    []Model   `yaml:"models"`
}

// Model is one selectable model with its metadata.
type Model struct {
	Name          string `yaml:"name"`
	ContextWindow int64  `yaml:"context_window,omitempty"`
	// Modalities declares the model's input modalities (e.g. ["text",
	// "image"]). Empty means text-only: submitting image attachments to
	// such a model is rejected, and image references in history are
	// replaced by text gaps (media.StripImages) instead of inline images.
	Modalities []string `yaml:"modalities,omitempty"`
	// WindowUtilization overrides context.utilization for this model (e.g.
	// when a gateway overstates the window); nil inherits the global value.
	WindowUtilization *float64 `yaml:"window_utilization,omitempty"`
	// MaxOutputTokens caps the request parameter (model capability); the
	// agent budget guardrail lives in limits.max_output_tokens.
	MaxOutputTokens int64     `yaml:"max_output_tokens,omitempty"`
	WireAPI         string    `yaml:"wire_api,omitempty"`
	Reasoning       Reasoning `yaml:"reasoning,omitempty"`
}

// SupportsImages reports whether the model declares image input.
func (m Model) SupportsImages() bool {
	for _, modality := range m.Modalities {
		if modality == "image" {
			return true
		}
	}
	return false
}

// Reasoning configures the model's reasoning (thinking) intent in
// vendor-neutral terms; each provider maps it onto its wire representation
// (Anthropic thinking.budget_tokens, OpenAI reasoning_effort).
type Reasoning struct {
	// Effort is "off", "low", "medium", or "high"; empty means the
	// provider decides.
	Effort string `yaml:"effort,omitempty"`
	// BudgetTokens is an explicit reasoning token budget; it wins over the
	// effort-derived budget where the wire API supports one.
	BudgetTokens int64 `yaml:"budget_tokens,omitempty"`
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
	MaxInputTokens  *int64   `yaml:"max_input_tokens,omitempty"`
	MaxOutputTokens *int64   `yaml:"max_output_tokens,omitempty"`
	MaxCostUSD      *float64 `yaml:"max_cost_usd,omitempty"`
	// MaxTokens budgets session-cumulative metered tokens; nil keeps the
	// default (0 = unlimited, opt-in).
	MaxTokens          *int64 `yaml:"max_tokens,omitempty"`
	MaxToolOutputBytes *int64 `yaml:"max_tool_output_bytes,omitempty"`
	MaxArtifactBytes   *int64 `yaml:"max_artifact_bytes,omitempty"`
}

// Context mirrors domain.ContextConfig; nil fields keep the built-in
// default. All values are ratios of the model's effective context window.
type Context struct {
	Utilization         *float64  `yaml:"utilization,omitempty"`
	CompactTriggerRatio *float64  `yaml:"compact_trigger_ratio,omitempty"`
	CompactTargetRatio  *float64  `yaml:"compact_target_ratio,omitempty"`
	NoticeLevels        []float64 `yaml:"notice_levels,omitempty"`
}

// Runaway mirrors domain.RunawayConfig; nil fields keep the built-in
// default.
type Runaway struct {
	MaxRepeatedCalls       *int `yaml:"max_repeated_calls,omitempty"`
	MaxConsecutiveFailures *int `yaml:"max_consecutive_failures,omitempty"`
	StallWarnTurns         *int `yaml:"stall_warn_turns,omitempty"`
	// StallTimeout uses Go duration syntax ("15m", "1h"); empty keeps the
	// default, "0" disables the stall watchdog.
	StallTimeout string `yaml:"stall_timeout,omitempty"`
}

// Prompt configures the system prompt.
type Prompt struct {
	Extra          string        `yaml:"extra,omitempty"`
	DisableBuiltin bool          `yaml:"disable_builtin,omitempty"`
	Managed        ManagedPrompt `yaml:"managed,omitempty"`
}

// ManagedPrompt selects a Langfuse-managed prompt (requires tracing).
type ManagedPrompt struct {
	Name  string `yaml:"name,omitempty"`
	Label string `yaml:"label,omitempty"`
}

// Skills configures skill discovery. Enabled is nil-typed so "absent"
// defaults to true while an explicit false disables. Disabled lists skill
// names to suppress at load time (by name, across every scope): a disabled
// skill stays on disk but never enters the catalog the model sees.
type Skills struct {
	Enabled    *bool    `yaml:"enabled,omitempty"`
	ExtraRoots []string `yaml:"extra_roots,omitempty"`
	Disabled   []string `yaml:"disabled,omitempty"`
}

// Tools configures how loom locates the local development toolchain.
type Tools struct {
	// PathExtra lists additional directories prepended to the process PATH
	// ahead of every built-in candidate (explicit configuration wins).
	// Entries support a leading "~/"; they must resolve to absolute paths.
	PathExtra []string `yaml:"path_extra,omitempty"`
}

// Rules configures the declarative permission rule layers. All bools are
// nil-typed: absent keeps the built-in default (documented per field in
// resolve.go).
type Rules struct {
	Enabled           *bool `yaml:"enabled,omitempty"`
	Builtin           *bool `yaml:"builtin,omitempty"`
	Project           *bool `yaml:"project,omitempty"`
	ProjectAllow      *bool `yaml:"project_allow,omitempty"`
	PersistRemembered *bool `yaml:"persist_remembered,omitempty"`
}

// Approval configures the baseline approval strategy
// (docs/PERMISSION_DESIGN.md §4.3). Mode selects how calls with no rule
// or session memory are decided: "on-request" (default: everything the
// sandbox or path validator confines runs without prompting; escalation,
// network widening, and danger-listed commands prompt),
// "unless-dangerous" (blacklist mode: additionally grants declared
// network needs silently; only the danger screen and escalations
// prompt), or "never" (unattended: sandboxed calls run, escalations and
// dangerous commands are denied outright so a run can never hang on a
// prompt).
type Approval struct {
	Mode string `yaml:"mode,omitempty"`
	// TrustUserURLs auto-allows fetching a host the user mentioned in the
	// conversation (web_fetch, browser navigate): the user handed the
	// agent the URL, so asking again is pure friction. Rule-layer denies
	// still win, and never mode ignores it. Nil means enabled.
	TrustUserURLs *bool `yaml:"trust_user_urls,omitempty"`
}

// Tracing configures Langfuse observability. Keys follow the same
// inline-or-env-reference rule as provider API keys.
type Tracing struct {
	Host           string `yaml:"host,omitempty"`
	PublicKey      string `yaml:"public_key,omitempty"`
	PublicKeyEnv   string `yaml:"public_key_env,omitempty"`
	SecretKey      string `yaml:"secret_key,omitempty"`
	SecretKeyEnv   string `yaml:"secret_key_env,omitempty"`
	Environment    string `yaml:"environment,omitempty"`
	IncludeContent *bool  `yaml:"include_content,omitempty"`
	User           string `yaml:"user,omitempty"`
	// Cost rates are USD per million tokens; non-positive disables cost
	// attribution.
	CostInputPerMTok  float64 `yaml:"cost_input_usd_per_mtok,omitempty"`
	CostOutputPerMTok float64 `yaml:"cost_output_usd_per_mtok,omitempty"`
}

// Share is the persistent preference for the optional LAN share
// listener (loom-desktop, docs/DESKTOP_DESIGN.md §5): whether it starts
// at launch, and its fixed bind address. A stable port is what lets
// share links survive restarts — share tokens are persisted in the
// session store. Toggling at runtime (POST /v1/share/endpoint) writes
// through to enabled and hot-applies, so the on/off state never
// diverges from this file.
type Share struct {
	Enabled *bool  `yaml:"enabled,omitempty"`
	Listen  string `yaml:"listen,omitempty"`
}

// Logging configures file logging quotas (glog-style daily files under
// <loom home>/logs). Zero values keep the built-in defaults; negative
// values disable the corresponding limit.
type Logging struct {
	// MaxFileMB caps one log file in MiB; past the cap the writer rolls to
	// a same-day sequence file. 0 = default (2048).
	MaxFileMB int `yaml:"max_file_mb,omitempty"`
	// MaxTotalMB caps the logs directory total in MiB; the oldest files are
	// garbage-collected past the cap. 0 = default (10240).
	MaxTotalMB int `yaml:"max_total_mb,omitempty"`
}

// UI configures the terminal frontend.
type UI struct {
	Icons     string `yaml:"icons,omitempty"`
	AltScreen bool   `yaml:"alt_screen,omitempty"`
	// Keymap overrides default key bindings (docs/VIM_UI_DESIGN.md §5.2):
	// context → action → replacement key, e.g.
	// {chat: {search_transcript: "ctrl+s"}}. Unknown contexts/actions
	// and conflicting keys are ignored with a status-bar warning.
	Keymap map[string]map[string]string `yaml:"keymap,omitempty"`
}

// Subagent configures the delegate_task sub-agent
// (docs/SUBAGENT_DESIGN.md §7). Enabled is nil-typed so "absent"
// defaults to true while an explicit false removes the tool from the
// registry entirely.
type Subagent struct {
	Enabled *bool `yaml:"enabled,omitempty"`
	// MaxTokens caps the child run's session-cumulative metered tokens;
	// nil/0 inherits limits.max_tokens. The child's consumption is also
	// folded back into the parent run's budget, so delegation is never a
	// budget loophole.
	MaxTokens *int64 `yaml:"max_tokens,omitempty"`
	// MaxOutputTokens caps each child model RESPONSE. Sub-agent answers
	// should be concise; the per-request cap is also a latency knob — a
	// truncation is only discovered after the whole output budget has
	// streamed. nil = 8192 (reasoning headroom included); explicit 0
	// inherits limits.max_output_tokens.
	MaxOutputTokens *int64 `yaml:"max_output_tokens,omitempty"`
	// Model pins the sub-agent to a specific "provider/model" (or a bare
	// model/provider name); empty follows the current turn's model.
	Model string `yaml:"model,omitempty"`
}

// Image configures text-to-image generation (the generate_image tool).
// Generation reuses the named provider's base_url/api_key, so only
// openai-type providers qualify; the tool is registered only when the
// section is enabled. Enabled is nil-typed: absent means "enabled when
// provider and model are both set", an explicit false always disables.
type Image struct {
	Enabled  *bool  `yaml:"enabled,omitempty"`
	Provider string `yaml:"provider,omitempty"`
	Model    string `yaml:"model,omitempty"`
	// Size/Quality are the generation defaults (OpenAI Images API
	// vocabulary: "auto", "1024x1024", ... / "low", "medium", "high");
	// empty means "auto".
	Size    string `yaml:"size,omitempty"`
	Quality string `yaml:"quality,omitempty"`
}

// Memory configures the long-term memory system (docs/MEMORY_DESIGN.md).
// Enabled is nil-typed so "absent" defaults to true while an explicit
// false disables extraction, consolidation, and tool registration.
//
// The extraction/consolidation pipeline runs in the background at process
// startup (and every run_interval afterwards), draining a persistent job
// queue — never on the shutdown path.
type Memory struct {
	Enabled *bool `yaml:"enabled,omitempty"`
	// ExtractModel pins the Phase 1 extraction model ("provider/model" or
	// a bare model/provider name); empty follows the default model. A
	// cheap/fast model is recommended.
	ExtractModel string `yaml:"extract_model,omitempty"`
	// ConsolidationModel pins the Phase 2 consolidation model; empty
	// follows the default model.
	ConsolidationModel string `yaml:"consolidation_model,omitempty"`
	// MaxJobsPerRun bounds extraction jobs claimed per pipeline pass;
	// nil keeps the default (8).
	MaxJobsPerRun *int `yaml:"max_jobs_per_run,omitempty"`
	// RunInterval re-runs the pipeline periodically (Go duration syntax,
	// e.g. "30m"); empty keeps the default ("30m"), "0" runs the pipeline
	// once at startup only.
	RunInterval string `yaml:"run_interval,omitempty"`
	// MinSessionIdle skips sessions touched more recently than this (Go
	// duration); empty keeps the default ("1h"). Prevents extracting
	// sessions that may still be active.
	MinSessionIdle string `yaml:"min_session_idle,omitempty"`
	// MaxSessionAge skips sessions last touched longer ago than this (Go
	// duration); empty keeps the default ("720h", 30 days).
	MaxSessionAge string `yaml:"max_session_age,omitempty"`
}

// Browser configures the headless Chrome browser tool. Enabled is
// nil-typed: absent means "enabled" (the tool registers with defaults),
// an explicit false removes the browser tool from the registry.
type Browser struct {
	Enabled    *bool  `yaml:"enabled,omitempty"`
	ChromePath string `yaml:"chrome_path,omitempty"`
	// CdpURL is a remote Chrome DevTools Protocol endpoint (ws:// or
	// http://). When set, the browser Manager connects to this remote
	// Chrome instead of launching a local process — letting users point
	// loom at an externally managed Chrome (e.g. one with a real profile
	// or anti-detection extensions). Empty means "launch locally".
	CdpURL         string `yaml:"cdp_url,omitempty"`
	IdleTTL        string `yaml:"idle_ttl,omitempty"`
	NavTimeoutMs   int    `yaml:"nav_timeout_ms,omitempty"`
	ScreenshotQ    int    `yaml:"screenshot_quality,omitempty"`
	ViewportWidth  int    `yaml:"viewport_width,omitempty"`
	ViewportHeight int    `yaml:"viewport_height,omitempty"`
}

// KnowledgeBase configures the minisearch-backed knowledge base tools
// (kb_search/kb_read). Enabled is nil-typed: absent means "enabled when
// base_url is set" (like image), an explicit false always disables.
// The credential lives inline: a reader-role key is sufficient for the
// read-only tools, and env indirection is intentionally not offered (see
// Provider.APIKey for the inline convention).
type KnowledgeBase struct {
	Enabled *bool  `yaml:"enabled,omitempty"`
	BaseURL string `yaml:"base_url,omitempty"`
	// APIKey is the minisearch bearer token (msk_…). Optional: a
	// minisearch deployment running with --auth=off needs none.
	APIKey string `yaml:"api_key,omitempty"`
	// TimeoutMs bounds each request; 0 keeps the default (10000).
	TimeoutMs int `yaml:"timeout_ms,omitempty"`
	// DefaultTopK is the default result count; 0 keeps the default (5).
	DefaultTopK int `yaml:"default_top_k,omitempty"`
	// Collections lists the searchable collections. At least one is
	// required when enabled; each carries a description surfaced to the
	// model so it can route by topic without a discovery round-trip.
	Collections []KBCollection `yaml:"collections,omitempty"`
	// DefaultCollection selects the default; empty means the first entry.
	DefaultCollection string `yaml:"default_collection,omitempty"`
}

// KBCollection names one searchable minisearch collection.
type KBCollection struct {
	Name        string `yaml:"name"`
	Description string `yaml:"description,omitempty"`
}

// MCPServer configures one MCP server connection. The key in MCPServers
// is the server name used for log attribution and tool name
// qualification (mcp__{server}__{tool}). Two transports are supported,
// selected by exactly one of Command or URL:
//   - command: spawn a subprocess and speak MCP over its stdio pipes;
//   - url:     POST JSON-RPC to a remote streamable HTTP endpoint.
//
// It aliases mcp.ServerConfig so the resolved configuration flows into
// the MCP manager without a field-by-field copy (REVIEW R12); the ${VAR}
// header reference resolution documented there happens at load time here.
type MCPServer = mcp.ServerConfig
