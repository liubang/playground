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
// Created: 2026/07/25

// Package trace provides Langfuse-backed observability for agent runs.
// Spans are exported over OTLP/HTTP with OpenTelemetry GenAI semantic
// conventions plus Langfuse's own observation attributes, so traces render
// as first-class generations in the Langfuse UI. A zero Config disables
// tracing entirely (no-op, near-zero overhead).
package trace

import (
	"os"
	"os/exec"
	"strconv"
	"strings"
)

// Config configures the Langfuse OTLP exporter.
type Config struct {
	// Enabled reports whether tracing should run at all (derived: host and
	// both keys must be present).
	Enabled bool
	// Host is the Langfuse base URL, e.g. "http://localhost:3100". The OTLP
	// path (/api/public/otel/v1/traces) is appended by Setup.
	Host string
	// PublicKey and SecretKey are Langfuse project credentials, sent as HTTP
	// Basic auth on the OTLP endpoint.
	PublicKey string
	SecretKey string
	// Environment is reported as langfuse.environment (e.g. "dev", "ci").
	Environment string
	// IncludeContent controls fidelity: when false, message/tool content is
	// replaced by structural summaries (roles, kinds, byte sizes) so no
	// conversation text leaves the process.
	IncludeContent bool
	// UserID attributes traces to an end user (langfuse.trace.user_id).
	UserID string
	// Release is the loom version attributed to traces
	// (langfuse.trace.release), enabling per-version comparisons.
	Release string
	// CostInputPerMTok and CostOutputPerMTok are optional USD-per-million-
	//-token rates; when both are positive the recorder computes and
	// attaches cost_details to generations.
	CostInputPerMTok  float64
	CostOutputPerMTok float64
}

// ConfigFromEnv reads tracing configuration from the environment.
// LOOM_LANGFUSE_* takes precedence; the community-standard LANGFUSE_* names
// are honored as fallbacks so existing Langfuse setups work unchanged.
//
//	LOOM_LANGFUSE_HOST / LANGFUSE_HOST / LANGFUSE_BASE_URL — base URL (required)
//	LOOM_LANGFUSE_PUBLIC_KEY / LANGFUSE_PUBLIC_KEY (required)
//	LOOM_LANGFUSE_SECRET_KEY / LANGFUSE_SECRET_KEY (required)
//	LOOM_LANGFUSE_ENVIRONMENT               — default "dev"
//	LOOM_TRACE_CONTENT                      — "0" redacts message content
//	LOOM_TRACE_USER                         — user_id (fallback: git email, $USER)
//	LOOM_VERSION                            — release tag on traces
//	LOOM_COST_INPUT_USD_PER_MTOK            — input cost rate per 1M tokens
//	LOOM_COST_OUTPUT_USD_PER_MTOK           — output cost rate per 1M tokens
func ConfigFromEnv() Config {
	get := func(names ...string) string {
		for _, name := range names {
			if v := os.Getenv(name); v != "" {
				return v
			}
		}
		return ""
	}
	cfg := Config{
		Host:           strings.TrimRight(get("LOOM_LANGFUSE_HOST", "LANGFUSE_HOST", "LANGFUSE_BASE_URL"), "/"),
		PublicKey:      get("LOOM_LANGFUSE_PUBLIC_KEY", "LANGFUSE_PUBLIC_KEY"),
		SecretKey:      get("LOOM_LANGFUSE_SECRET_KEY", "LANGFUSE_SECRET_KEY"),
		Environment:    os.Getenv("LOOM_LANGFUSE_ENVIRONMENT"),
		IncludeContent: os.Getenv("LOOM_TRACE_CONTENT") != "0",
		UserID:         get("LOOM_TRACE_USER"),
		Release:        get("LOOM_VERSION"),
	}
	if cfg.UserID == "" {
		cfg.UserID = defaultUserID()
	}
	cfg.CostInputPerMTok = parseFloatEnv("LOOM_COST_INPUT_USD_PER_MTOK")
	cfg.CostOutputPerMTok = parseFloatEnv("LOOM_COST_OUTPUT_USD_PER_MTOK")
	if cfg.Environment == "" {
		cfg.Environment = "dev"
	}
	cfg.Enabled = cfg.Host != "" && cfg.PublicKey != "" && cfg.SecretKey != ""
	return cfg
}

// defaultUserID derives a stable user identity: the git author email when
// available, falling back to the OS account name.
func defaultUserID() string {
	if out, err := exec.Command("git", "config", "--get", "user.email").Output(); err == nil {
		if email := strings.TrimSpace(string(out)); email != "" {
			return email
		}
	}
	return os.Getenv("USER")
}

// parseFloatEnv parses a positive float environment value; malformed or
// non-positive values yield zero (feature off).
func parseFloatEnv(name string) float64 {
	v, err := strconv.ParseFloat(os.Getenv(name), 64)
	if err != nil || v <= 0 {
		return 0
	}
	return v
}
