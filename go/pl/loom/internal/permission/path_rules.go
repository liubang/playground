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
// Created: 2026/08/12

// Writable-path rules are the file-write counterpart of domain rules:
// instead of approving every write outside the workspace roots (the
// confinement boundary), the user approves a directory once — "always
// allow writing under ~/work/notes" — and every other location still
// prompts. They live in the same rule files under a "paths" key:
//
//	{"rules": [...], "paths": [{
//	  "path": "~/work/notes",
//	  "decision": "allow",               // allow | ask | deny
//	  "justification": "personal notes vault"
//	}]}
//
// A rule path is a canonical absolute file or directory ("~" expands at
// load, symlinks resolve): it matches a write target equal to it or
// beneath it (subpath semantics). Rules targeting a sensitive location
// (workspace.IsSensitiveAbsolute) are rejected at load — a rule must
// never reopen what the sensitive list closes. Project layers may only
// tighten (allow entries are dropped), same as argv and domain rules.
package permission

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// PathRule is one writable-path rule for file-writing tools.
type PathRule struct {
	// Path is the canonical absolute file or directory the rule covers.
	Path string `json:"path"`
	// Decision is allow | ask | deny (domain.Decision values).
	Decision string `json:"decision"`
	// Justification is the human-readable rationale surfaced in approval
	// prompts and denial messages.
	Justification string `json:"justification,omitempty"`
	// Source is the file the rule came from (diagnostics only).
	Source string `json:"-"`
}

// normalizeRulePath canonicalizes a rule path: "~" expands, the result
// must be absolute, symlinks resolve (rule paths match canonical write
// targets), and sensitive locations are refused.
func normalizeRulePath(p string) (string, error) {
	expanded, err := expandHome(p)
	if err != nil {
		return "", err
	}
	if !filepath.IsAbs(expanded) {
		return "", fmt.Errorf("path rule %q is not absolute", p)
	}
	canonical := workspacepkg.Canonicalize(expanded)
	if filepath.Dir(canonical) == canonical {
		return "", fmt.Errorf("path rule %q must not be the filesystem root", p)
	}
	if workspacepkg.IsSensitiveAbsolute(canonical) {
		return "", fmt.Errorf("path rule %q targets a sensitive location", p)
	}
	return canonical, nil
}

// pathRuleMatches reports whether the canonical rule path covers the
// canonical write target (exact or subpath).
func pathRuleMatches(rulePath, target string) bool {
	if target == rulePath {
		return true
	}
	return strings.HasPrefix(target, rulePath+string(filepath.Separator))
}

// validatePathRule checks path shape and decision validity.
func validatePathRule(r *PathRule) error {
	canonical, err := normalizeRulePath(r.Path)
	if err != nil {
		return err
	}
	r.Path = canonical
	switch domain.Decision(r.Decision) {
	case domain.DecisionAllow, domain.DecisionAsk, domain.DecisionDeny:
	default:
		return fmt.Errorf("path rule %q: decision must be allow|ask|deny, got %q", r.Path, r.Decision)
	}
	return nil
}

// EvaluatePath returns the strictest decision among matching path rules
// (exact or subpath), or "" when nothing matches.
func (s *RuleSet) EvaluatePath(target string) (domain.Decision, PathRule) {
	if s == nil {
		return "", PathRule{}
	}
	target = workspacepkg.Canonicalize(target)
	var (
		best    domain.Decision
		bestR   PathRule
		bestInt int
	)
	for _, r := range s.paths {
		if !pathRuleMatches(r.Path, target) {
			continue
		}
		d := domain.Decision(r.Decision)
		if n := decisionStrictness(d); n > bestInt {
			best, bestR, bestInt = d, r, n
		}
	}
	return best, bestR
}

// Paths returns the loaded path rules (for `loom rules list` and tests).
func (s *RuleSet) Paths() []PathRule {
	if s == nil {
		return nil
	}
	return append([]PathRule(nil), s.paths...)
}

// WriteInfo carries the policy-relevant shape of a boundary-crossing
// file write. It is the write counterpart of URLInfo.
type WriteInfo struct {
	// Path is the canonical absolute write target outside the roots.
	Path string
}

// WriteInfoOf resolves the boundary-crossing write shape of a prepared
// call: (WriteInfo, true) only when the call writes OUTSIDE the
// workspace + scratch roots. The typed PreparedCall.WriteRequest (signed
// by the producing tool) is the ONLY source — its OutsideRoots flag
// keeps confined-but-absolute writes (the scratch dirs) out of the
// boundary path, and its absence means "not a boundary write": tools
// that can cross the boundary always declare it during Prepare, and raw
// argument guessing would misclassify READ tools (read_file/view_image
// take an absolute path too) as boundary writes.
func WriteInfoOf(call domain.PreparedCall) (WriteInfo, bool) {
	if call.WriteRequest == nil || !call.WriteRequest.OutsideRoots || call.WriteRequest.Path == "" {
		return WriteInfo{}, false
	}
	return WriteInfo{Path: call.WriteRequest.Path}, true
}

// WriteRequestFromRawArgs synthesizes the typed write contract for the
// approval-UI boundary, where only the canonical raw arguments survive
// (rule previews and "allow always" memory derivation —
// app.ApprovalRulePreview / RuleApprover.RememberCall). An absolute
// "path" argument implies a boundary write there: confined writes never
// reach an approval, and the canonical display form of a confined write
// is workspace-relative. Relative paths never synthesize a request.
func WriteRequestFromRawArgs(raw json.RawMessage) *domain.WriteRequest {
	var args struct {
		Path string `json:"path"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || !filepath.IsAbs(args.Path) {
		return nil
	}
	return &domain.WriteRequest{
		Path:         workspacepkg.Canonicalize(args.Path),
		OutsideRoots: true,
	}
}

// --- Session path memory ---

// RememberPath records an approved writable directory for the rest of the
// session. The directory is canonicalized; sensitive locations are
// refused (ok=false) so a session memory can never reopen them.
func (s *SessionRules) RememberPath(dir string) (string, bool) {
	canonical, err := normalizeRulePath(dir)
	if err != nil {
		return "", false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.paths == nil {
		s.paths = make(map[string]struct{})
	}
	s.paths[canonical] = struct{}{}
	return canonical, true
}

// MatchPath reports whether a remembered directory covers the write
// target (exact or subpath).
func (s *SessionRules) MatchPath(target string) bool {
	target = workspacepkg.Canonicalize(target)
	s.mu.RLock()
	defer s.mu.RUnlock()
	for dir := range s.paths {
		if pathRuleMatches(dir, target) {
			return true
		}
	}
	return false
}

// ForgetPath removes a session-remembered writable directory. ok=false
// means the directory was not in the session store.
func (s *SessionRules) ForgetPath(dir string) bool {
	canonical, err := normalizeRulePath(dir)
	if err != nil {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.paths[canonical]; !ok {
		return false
	}
	delete(s.paths, canonical)
	return true
}

// SessionPaths returns the remembered writable directories (status
// display, tests).
func (s *SessionRules) SessionPaths() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]string, 0, len(s.paths))
	for p := range s.paths {
		out = append(out, p)
	}
	return out
}
