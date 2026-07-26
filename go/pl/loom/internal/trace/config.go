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
	"log/slog"
	"os"
	"os/exec"
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
	// -token rates; when both are positive the recorder computes and
	// attaches cost_details to generations.
	CostInputPerMTok  float64
	CostOutputPerMTok float64
	// Logger receives exporter/score/prompt-client failures (OTLP error
	// handler, score flush, prompt fetch). Nil discards them — pass an
	// io.Discard logger in the TUI (stderr output would tear the rendering)
	// and slog.Default() in headless runs.
	Logger *slog.Logger
}

// DefaultUserID derives a stable user identity: the git author email when
// available, falling back to the OS account name. Setup applies it whenever
// the config leaves UserID empty.
func DefaultUserID() string {
	if out, err := exec.Command("git", "config", "--get", "user.email").Output(); err == nil {
		if email := strings.TrimSpace(string(out)); email != "" {
			return email
		}
	}
	return os.Getenv("USER")
}
