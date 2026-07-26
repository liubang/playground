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

// File is the raw YAML schema of ~/.loom/config.yaml. Pointer fields
// distinguish "unset" (nil → built-in default) from an explicit value —
// e.g. limits.max_turns: 0 disables the turn budget and must not be
// mistaken for "not present".
type File struct {
	// Default selects the startup model: "provider/model", a bare model
	// name (must be unique across providers), or a bare provider name (its
	// default_model). Empty means providers[0]'s default model.
	Default string `yaml:"default"`

	// Providers is the only required section: at least one entry.
	Providers []Provider `yaml:"providers"`

	Limits  Limits  `yaml:"limits"`
	Prompt  Prompt  `yaml:"prompt"`
	Skills  Skills  `yaml:"skills"`
	Rules   Rules   `yaml:"rules"`
	Tracing Tracing `yaml:"tracing"`
	Storage Storage `yaml:"storage"`
	UI      UI      `yaml:"ui"`
}

// Provider describes one OpenAI-compatible endpoint and its model catalog.
type Provider struct {
	Name    string `yaml:"name"`
	Type    string `yaml:"type"`
	BaseURL string `yaml:"base_url"`
	// APIKey holds the secret inline; APIKeyEnv names an environment
	// variable to read it from. The two are mutually exclusive.
	APIKey    string `yaml:"api_key"`
	APIKeyEnv string `yaml:"api_key_env"`
	// WireAPI is the provider-level default ("chat" or "responses");
	// models may override it. Empty means "chat".
	WireAPI      string `yaml:"wire_api"`
	MaxRetries   *int   `yaml:"max_retries"`
	DefaultModel string `yaml:"default_model"`
	Models       []Model `yaml:"models"`
}

// Model is one selectable model with its metadata.
type Model struct {
	Name          string `yaml:"name"`
	ContextWindow int64  `yaml:"context_window"`
	// MaxOutputTokens caps the request parameter (model capability); the
	// agent budget guardrail lives in limits.max_output_tokens.
	MaxOutputTokens int64  `yaml:"max_output_tokens"`
	WireAPI         string `yaml:"wire_api"`
}

// Limits mirrors domain.Limits; nil fields keep the built-in default.
type Limits struct {
	MaxTurns           *int    `yaml:"max_turns"`
	MaxToolCalls       *int    `yaml:"max_tool_calls"`
	MaxInputTokens     *int64  `yaml:"max_input_tokens"`
	MaxOutputTokens    *int64  `yaml:"max_output_tokens"`
	MaxCostUSD         *float64 `yaml:"max_cost_usd"`
	// MaxWallTime uses Go duration syntax ("30m", "1h").
	MaxWallTime        string  `yaml:"max_wall_time"`
	MaxToolOutputBytes *int64  `yaml:"max_tool_output_bytes"`
	MaxArtifactBytes   *int64  `yaml:"max_artifact_bytes"`
	MaxRepeatedActions *int    `yaml:"max_repeated_actions"`
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
}
