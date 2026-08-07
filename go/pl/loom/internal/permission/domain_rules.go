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
// Created: 2026/07/30

// Domain rules are the web_fetch counterpart of argv-prefix rules: instead
// of approving every fetch (R3 network egress + untrusted content intake,
// the exfiltration leg of the lethal trifecta), the user approves ONE
// exact host — "always allow weather.com.cn" — and every other domain
// still prompts (docs/PERMISSION_DESIGN.md, M3-lite). They live in the
// same rule files under a "domains" key:
//
//	{"rules": [...], "domains": [{
//	  "host": "www.weather.com.cn",
//	  "decision": "allow",               // allow | ask | deny
//	  "justification": "weather lookups"
//	}]}
//
// Matching is EXACT host (case-insensitive, no port, no subdomains):
// approving www.weather.com.cn does not cover api.weather.com.cn. Project
// layers may only tighten (allow entries are dropped), same as argv rules.
package permission

import (
	"encoding/json"
	"fmt"
	"net/url"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// DomainRule is one host rule for web_fetch calls.
type DomainRule struct {
	// Host is the exact lowercase hostname (no scheme, path, or port).
	Host string `json:"host"`
	// Decision is allow | ask | deny (domain.Decision values).
	Decision string `json:"decision"`
	// Justification is the human-readable rationale surfaced in approval
	// prompts and denial messages.
	Justification string `json:"justification,omitempty"`
	// Source is the file the rule came from (diagnostics only).
	Source string `json:"-"`
}

// normalizeDomainHost validates and canonicalizes a rule host: lowercase,
// no scheme/userinfo/path/port, no leading dot.
func normalizeDomainHost(host string) (string, error) {
	h := strings.ToLower(strings.TrimSpace(host))
	h = strings.TrimSuffix(h, ".")
	if h == "" {
		return "", fmt.Errorf("host is empty")
	}
	if strings.ContainsAny(h, "/:@") {
		return "", fmt.Errorf("host %q must be a bare hostname (no scheme, path, port, or userinfo)", host)
	}
	if strings.HasPrefix(h, ".") || strings.HasSuffix(h, ".") || strings.Contains(h, "..") {
		return "", fmt.Errorf("host %q is not a valid hostname", host)
	}
	return h, nil
}

// validateDomainRule checks host shape and decision validity.
func validateDomainRule(r *DomainRule) error {
	host, err := normalizeDomainHost(r.Host)
	if err != nil {
		return err
	}
	r.Host = host
	switch domain.Decision(r.Decision) {
	case domain.DecisionAllow, domain.DecisionAsk, domain.DecisionDeny:
	default:
		return fmt.Errorf("domain rule %q: decision must be allow|ask|deny, got %q", r.Host, r.Decision)
	}
	return nil
}

// EvaluateDomain returns the strictest decision among matching domain
// rules (exact host), or "" when nothing matches.
func (s *RuleSet) EvaluateDomain(host string) (domain.Decision, DomainRule) {
	if s == nil {
		return "", DomainRule{}
	}
	host, err := normalizeDomainHost(host)
	if err != nil {
		return "", DomainRule{}
	}
	var (
		best    domain.Decision
		bestR   DomainRule
		bestInt int
	)
	for _, r := range s.domains {
		if r.Host != host {
			continue
		}
		d := domain.Decision(r.Decision)
		if n := decisionStrictness(d); n > bestInt {
			best, bestR, bestInt = d, r, n
		}
	}
	return best, bestR
}

// ParseWebFetchHost extracts the canonical hostname from web_fetch call
// arguments ({"url": "..."}). Only http/https URLs are eligible.
func ParseWebFetchHost(raw json.RawMessage) (string, bool) {
	var args struct {
		URL string `json:"url"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.URL == "" {
		return "", false
	}
	u, err := url.Parse(args.URL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") {
		return "", false
	}
	host, err := normalizeDomainHost(u.Hostname())
	if err != nil {
		return "", false
	}
	return host, true
}

// --- Session domain memory ---

// RememberDomain records an approved host for the rest of the session.
func (s *SessionRules) RememberDomain(host string) (string, bool) {
	host, err := normalizeDomainHost(host)
	if err != nil {
		return "", false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.domains == nil {
		s.domains = make(map[string]struct{})
	}
	s.domains[host] = struct{}{}
	return host, true
}

// MatchDomain reports whether the host was remembered this session.
func (s *SessionRules) MatchDomain(host string) bool {
	host, err := normalizeDomainHost(host)
	if err != nil {
		return false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.domains[host]
	return ok
}

// ForgetDomain removes a session-remembered host. ok=false means the host
// was not in the session store.
func (s *SessionRules) ForgetDomain(host string) bool {
	host, err := normalizeDomainHost(host)
	if err != nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.domains[host]; !ok {
		return false
	}
	delete(s.domains, host)
	return true
}

// Domains returns the remembered hosts (status display, tests).
func (s *SessionRules) Domains() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]string, 0, len(s.domains))
	for h := range s.domains {
		out = append(out, h)
	}
	return out
}


