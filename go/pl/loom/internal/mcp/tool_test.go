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

package mcp

import (
	"encoding/json"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func decodeSpec(t *testing.T, raw string) ToolSpec {
	t.Helper()
	var spec ToolSpec
	if err := json.Unmarshal([]byte(raw), &spec); err != nil {
		t.Fatalf("decode ToolSpec: %v", err)
	}
	return spec
}

// readOnlyHint promises "no side effects" but not "no network": a read-only
// search/fetch tool can still exfiltrate content to an external endpoint, so
// it must NOT land on the auto-approved R1 tier (REVIEW H12).
func TestCapabilitiesForSpecReadOnlyIsNotAutoApprovable(t *testing.T) {
	spec := decodeSpec(t, `{"name":"search","annotations":{"readOnlyHint":true}}`)
	caps := capabilitiesForSpec(spec)

	hasNet := false
	for _, c := range caps {
		if c == domain.CapNetworkConnect {
			hasNet = true
		}
	}
	if !hasNet {
		t.Fatalf("readOnly capabilities = %v, want network.connect included", caps)
	}
	def := domain.ToolDefinition{Name: "mcp__srv__search", Capabilities: caps, Source: domain.ToolSourceMCP}
	if got := def.Risk(); got != domain.R3 {
		t.Fatalf("readOnly risk = %d, want R3 (approval required)", got)
	}
}

func TestCapabilitiesForSpecRemainingMappings(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		risk domain.RiskLevel
	}{
		{"destructive", `{"name":"drop","annotations":{"destructiveHint":true}}`, domain.R3},
		{"open world", `{"name":"fetch","annotations":{"openWorldHint":true}}`, domain.R3},
		{"no annotations", `{"name":"plain"}`, domain.R2},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			def := domain.ToolDefinition{Name: "mcp__srv__x", Capabilities: capabilitiesForSpec(decodeSpec(t, tc.raw)), Source: domain.ToolSourceMCP}
			if got := def.Risk(); got != tc.risk {
				t.Fatalf("risk = %d, want %d", got, tc.risk)
			}
		})
	}
}
