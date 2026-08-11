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
// Created: 2026/08/18

package permission

import (
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// ExtractNetworkHosts scans a command's argv for tokens that look like
// network targets — URLs (http/https) or bare hostnames — and returns the
// canonicalized hosts. This is used to apply domain rules to run_cmd calls
// that declared needs_network: the model said the command reaches out, so
// the policy layer can apply the same host allow/deny list that governs
// web_fetch.
//
// Extraction is best-effort and conservative:
//   - Only http/https URLs and bare hostnames are recognized. Schemes like
//     git+ssh, ftp, etc. are ignored (the actual network egress may or may
//     not happen — policy defers to the sandbox).
//   - A bare token is treated as a hostname only if it looks like one:
//     contains a dot, no path separators, no spaces, and survives
//     CanonicalHost validation.
//   - Shell-script argv: every subcommand's argv is scanned, so a composed
//     script's network targets are all covered.
//
// The result may be empty even when the command does network egress (e.g.
// curl without an explicit URL argument, using a config file). An empty
// result means "no domain rule applies" — the argv-prefix rules and the
// baseline still apply.
func ExtractNetworkHosts(info RunCmdCall) []string {
	argvs, ok := info.ShellCommandArgvs()
	if !ok {
		return nil
	}
	seen := map[string]struct{}{}
	var hosts []string
	for _, argv := range argvs {
		for _, tok := range argv {
			host, ok := extractHost(tok)
			if !ok {
				continue
			}
			if _, dup := seen[host]; dup {
				continue
			}
			seen[host] = struct{}{}
			hosts = append(hosts, host)
		}
	}
	return hosts
}

// evaluateNetworkHosts extracts network targets from the call's argv and
// evaluates them against the domain rule set. It returns the strictest
// matching domain verdict (deny > ask > allow), or nil when no domain rule
// matches any extracted host. A nil result means "no domain opinion" — the
// argv verdict stands, and the baseline decides the rest.
//
// Only called when info.NeedsNetwork is true: a command that did not
// declare network egress has no network target to filter.
func (d RuleDecider) evaluateNetworkHosts(info RunCmdCall) *domain.Verdict {
	hosts := ExtractNetworkHosts(info)
	if len(hosts) == 0 {
		return nil
	}
	var (
		best   domain.Decision
		bestR  DomainRule
		bestIn int
	)
	for _, host := range hosts {
		decision, rule := d.Rules.EvaluateDomain(host)
		if decision == "" {
			continue
		}
		if n := decisionStrictness(decision); n > bestIn {
			best, bestR, bestIn = decision, rule, n
		}
	}
	if best == "" {
		return nil
	}
	return &domain.Verdict{Decision: best, Source: SourceRule, Reason: bestR.Justification}
}

// extractHost tries to recover a canonical hostname from a single argv
// token. It first attempts URL parsing (http/https only); if that fails,
// it tries the token as a bare hostname — but only if it contains a dot
// (a bare word like "build" is not a hostname, while "api.example.com"
// is).
func extractHost(tok string) (string, bool) {
	// Fast path: if the token contains "://" it must parse as a URL.
	if strings.Contains(tok, "://") {
		return domain.HostFromURL(tok)
	}
	// Bare hostname path: only consider tokens that look like domains
	// (contain at least one dot, no path separators, no colons that
	// would indicate a port or scheme delimiter).
	if !strings.Contains(tok, ".") {
		return "", false
	}
	if strings.ContainsAny(tok, "/\\ ") {
		return "", false
	}
	// Strip a trailing port if present (e.g. "example.com:443").
	if idx := strings.Index(tok, ":"); idx >= 0 {
		tok = tok[:idx]
	}
	host, err := domain.CanonicalHost(tok)
	if err != nil {
		return "", false
	}
	return host, true
}
