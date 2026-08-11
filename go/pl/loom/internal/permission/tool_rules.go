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

// Tool rules are the third rule kind: instead of an argv prefix (run_cmd)
// or an exact host (web_fetch), the user approves ONE tool by name —
// "always allow generate_image" — and every later call to that tool is
// auto-allowed without inspecting its arguments. That is only honest for
// tools whose blast radius is FIXED BY CONSTRUCTION: generate_image always
// calls the pinned image API with a prompt (network egress + provider
// cost), never a caller-chosen host, path, or program. Tools whose
// arguments select the target (edit/write paths, URLs, commands) must
// stay per-call or use their dedicated rule shape — see ToolMemoryEligible.
//
// Tool rules never appear in declarative rule files; they are created by
// interactive "allow always" decisions and persist in the remembered
// store (remembered_tools table), loading into the policy RuleSet like
// the other kinds.
package permission

import (
	"fmt"
	"regexp"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ToolRule is one tool-name rule for non-exec, non-web_fetch calls.
type ToolRule struct {
	// Name is the exact lowercase tool name (e.g. "generate_image").
	Name string `json:"name"`
	// Decision is allow | ask | deny (domain.Decision values). Only allow
	// is produced today (interactive approvals), but the evaluation keeps
	// strictest-wins semantics so a deny could be layered in later.
	Decision string `json:"decision"`
	// Justification is the human-readable rationale surfaced in approval
	// prompts and denial messages.
	Justification string `json:"justification,omitempty"`
	// Source is the store the rule came from (diagnostics only).
	Source string `json:"-"`
}

// toolNameToken is the canonical tool-name shape (snake_case, as declared
// in domain.ToolDefinition.Name).
var toolNameToken = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// normalizeToolName validates and canonicalizes a tool name.
func normalizeToolName(name string) (string, error) {
	n := strings.ToLower(strings.TrimSpace(name))
	if !toolNameToken.MatchString(n) {
		return "", fmt.Errorf("tool name %q is not a valid snake_case tool name", name)
	}
	return n, nil
}

// toolMemoryEligible lists the tools whose approvals may be remembered
// categorically BY NAME. The bar: the tool's risk profile must not depend
// on its arguments — a fixed remote endpoint and no filesystem effect —
// so one interactive approval covers every future invocation.
// run_cmd/exec_session (argv rules) and web_fetch (host rules) have their
// own, strictly narrower memory shapes and are deliberately absent.
var toolMemoryEligible = map[string]struct{}{
	// Pinned model + provider endpoint; arguments are prompt/size/quality
	// only. Each call still costs provider quota, which the standing
	// approval explicitly accepts.
	"generate_image": {},
	// The search backend endpoint is pinned at process start by deployment
	// configuration (BRAVE_SEARCH_API_KEY / TAVILY_API_KEY /
	// LOOM_WEB_SEARCH_PROVIDER → hardcoded provider URLs); arguments are
	// query/count/timeout only and the SSRF dial guard keeps the DNS answer
	// honest, so the egress target can never be argument-shaped. No
	// filesystem effect.
	"web_search": {},
}

// mcpToolPrefix is the qualified-name prefix of MCP-sourced tools
// (mcp__{server}__{tool}). MCP tools are third-party code, so the
// baseline keeps them per-call — but the trust decision for one is
// inherently about the server endpoint, not the arguments, so a name
// level "allow always" is the honest memory shape for them.
const mcpToolPrefix = "mcp__"

// ToolMemoryEligible reports whether a tool's "allow always" approval may
// be remembered by tool name, and returns the canonical (normalized) name
// so callers never re-normalize on their own.
func ToolMemoryEligible(name string) (canonical string, ok bool) {
	n, err := normalizeToolName(name)
	if err != nil {
		return "", false
	}
	if _, ok := toolMemoryEligible[n]; ok {
		return n, true
	}
	if strings.HasPrefix(n, mcpToolPrefix) {
		return n, true
	}
	return "", false
}

// EvaluateTool returns the strictest decision among matching tool rules
// (exact name), or "" when nothing matches.
func (s *RuleSet) EvaluateTool(name string) (domain.Decision, ToolRule) {
	if s == nil {
		return "", ToolRule{}
	}
	name, err := normalizeToolName(name)
	if err != nil {
		return "", ToolRule{}
	}
	var (
		best    domain.Decision
		bestR   ToolRule
		bestInt int
	)
	for _, r := range s.tools {
		if r.Name != name {
			continue
		}
		d := domain.Decision(r.Decision)
		if n := decisionStrictness(d); n > bestInt {
			best, bestR, bestInt = d, r, n
		}
	}
	return best, bestR
}

// --- Session tool memory ---

// RememberTool records an approved tool for the rest of the session.
// ok=false means the tool is not eligible for name-level memory
// (ToolMemoryEligible) — callers must keep such approvals per-call.
func (s *SessionRules) RememberTool(name string) (string, bool) {
	name, ok := ToolMemoryEligible(name)
	if !ok {
		return "", false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.tools == nil {
		s.tools = make(map[string]struct{})
	}
	s.tools[name] = struct{}{}
	return name, true
}

// MatchTool reports whether the tool was remembered this session.
func (s *SessionRules) MatchTool(name string) bool {
	name, err := normalizeToolName(name)
	if err != nil {
		return false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.tools[name]
	return ok
}

// ForgetTool removes a session-remembered tool. ok=false means the tool
// was not in the session store.
func (s *SessionRules) ForgetTool(name string) bool {
	name, err := normalizeToolName(name)
	if err != nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.tools[name]; !ok {
		return false
	}
	delete(s.tools, name)
	return true
}

// Tools returns the remembered tool names (status display, tests).
func (s *SessionRules) Tools() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]string, 0, len(s.tools))
	for n := range s.tools {
		out = append(out, n)
	}
	return out
}
