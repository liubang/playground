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

// Rule files are the loom equivalent of codex's execpolicy: declarative
// argv-prefix rules for run_cmd calls, loaded from layered directories and
// self-tested at load time. A rule file is JSON:
//
//	{"rules": [{
//	  "argv_prefix": ["go", "test"],
//	  "decision": "allow",               // allow | ask | deny
//	  "justification": "read-only tests",
//	  "match": [["go", "test", "./..."], "go test -run X ./pkg"],
//	  "not_match": [["gofmt", "-w", "."], "go run ."]
//	}]}
//
// Semantics, mirroring codex execpolicy:
//   - A rule matches when the command argv starts with argv_prefix (exact
//     token comparison, no globbing).
//   - match/not_match examples are validated AT LOAD: every match example
//     must hit the rule, every not_match example must miss it. A file that
//     fails self-testing is rejected whole, so a half-broken ruleset never
//     silently applies.
//   - When several rules match, the strictest decision wins:
//     deny > ask > allow.
//   - Layers are merged: user dir (~/.loom/rules) plus, optionally, the
//     project dir (<workspace>/.loom/rules). Because a checked-out
//     repository is not fully trusted, project-layer "allow" rules are
//     ignored unless explicitly enabled (LOOM_PROJECT_RULES_ALLOW=1) —
//     untrusted layers may only tighten, never loosen (same shape as
//     codex's requirements overlay).
package permission

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// Rule is one argv-prefix policy rule for run_cmd calls.
type Rule struct {
	// ArgvPrefix is the exact token prefix the command argv must start
	// with. Empty prefix matches every run_cmd call — useful only for
	// deny/ask rules in trusted layers.
	ArgvPrefix []string `json:"argv_prefix"`
	// Decision is allow | ask | deny (domain.Decision values).
	Decision string `json:"decision"`
	// Justification is the human-readable rationale surfaced in approval
	// prompts and denial messages.
	Justification string `json:"justification,omitempty"`
	// Match lists example invocations that MUST hit this rule (load-time
	// self-test). Entries are token arrays or strings (strings.Fields).
	Match []ArgvExample `json:"match,omitempty"`
	// NotMatch lists example invocations that MUST NOT hit this rule.
	NotMatch []ArgvExample `json:"not_match,omitempty"`
	// Source is the file the rule came from (diagnostics only, not serialized).
	Source string `json:"-"`
}

// ArgvExample is one self-test invocation: a token array or a bare string.
type ArgvExample []string

// UnmarshalJSON accepts both ["go","test"] and "go test".
func (e *ArgvExample) UnmarshalJSON(data []byte) error {
	var tokens []string
	if err := json.Unmarshal(data, &tokens); err == nil {
		*e = tokens
		return nil
	}
	var s string
	if err := json.Unmarshal(data, &s); err != nil {
		return fmt.Errorf("example must be a string or a string array")
	}
	*e = strings.Fields(s)
	return nil
}

// ruleFile is the on-disk shape of one *.json rule file.
type ruleFile struct {
	Rules []Rule `json:"rules"`
}

// matches reports whether argv hits the rule's prefix.
func (r *Rule) matches(argv []string) bool {
	return ArgvHasPrefix(argv, r.ArgvPrefix)
}

// decisionStrictness ranks decisions so the strictest wins (deny > ask > allow).
func decisionStrictness(d domain.Decision) int {
	switch d {
	case domain.DecisionDeny:
		return 3
	case domain.DecisionAsk:
		return 2
	case domain.DecisionAllow:
		return 1
	}
	return 0
}

// RuleSet is an ordered collection of rules from one or more layers.
type RuleSet struct {
	rules []Rule
}

// Rules returns the loaded rules (for `loom rules list` and tests).
func (s *RuleSet) Rules() []Rule {
	if s == nil {
		return nil
	}
	return append([]Rule(nil), s.rules...)
}

// builtinJSON is the curated set of read-only commands that never deserve
// an approval prompt. Inclusion bar (v1): no writes, no code execution, no
// network — harmless even if the sandbox failed open. NOT included on
// purpose: find (-exec), xargs, awk (system()), sed (-i), git branch (-d),
// go test (runs code), shells.
//
//go:embed builtin.json
var builtinJSON []byte

// builtinSource marks rules that came from the embedded set.
const builtinSource = "builtin"

// LoadBuiltinRules parses and self-tests the embedded rule set. A broken
// embedded rule is a build-time bug, so the error is fatal to the caller
// (AttachRules panics in tests via the unit test over this function).
func LoadBuiltinRules() (*RuleSet, error) {
	var f ruleFile
	if err := json.Unmarshal(builtinJSON, &f); err != nil {
		return nil, fmt.Errorf("parse embedded builtin rules: %w", err)
	}
	set := &RuleSet{}
	for i := range f.Rules {
		f.Rules[i].Source = builtinSource
		if err := validateRule(&f.Rules[i]); err != nil {
			return nil, fmt.Errorf("embedded builtin rules: %w", err)
		}
		set.rules = append(set.rules, f.Rules[i])
	}
	return set, nil
}

// merge appends other's rules into s (evaluation is strictest-wins, so
// merge order never changes the outcome).
func (s *RuleSet) merge(other *RuleSet) {
	if s == nil || other == nil {
		return
	}
	s.rules = append(s.rules, other.rules...)
}

// Evaluate returns the strictest decision among matching rules, or "" when
// no rule matches (callers then fall back to risk-based policy).
func (s *RuleSet) Evaluate(argv []string) (domain.Decision, Rule) {
	if s == nil {
		return "", Rule{}
	}
	var (
		best    domain.Decision
		bestR   Rule
		bestInt int
	)
	for _, r := range s.rules {
		if !r.matches(argv) {
			continue
		}
		d := domain.Decision(r.Decision)
		if n := decisionStrictness(d); n > bestInt {
			best, bestR, bestInt = d, r, n
		}
	}
	return best, bestR
}

// MatchRule returns the winning rule for argv (zero Rule with empty Source
// when nothing matches) — the display-side companion of Evaluate, for
// `loom rules check`.
func MatchRule(s *RuleSet, argv []string) Rule {
	_, r := s.Evaluate(argv)
	return r
}

// Size reports the number of loaded rules.
func (s *RuleSet) Size() int {
	if s == nil {
		return 0
	}
	return len(s.rules)
}

// RulesDirUser is the user-layer rules directory (~/.loom/rules).
func RulesDirUser() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("locate home dir: %w", err)
	}
	return filepath.Join(home, ".loom", "rules"), nil
}

// RulesDirProject is the project-layer rules directory for a workspace.
func RulesDirProject(workspaceRoot string) string {
	return filepath.Join(workspaceRoot, ".loom", "rules")
}

// LoadOptions controls rule loading. ProjectAllows enables "allow" rules
// from the project layer (LOOM_PROJECT_RULES_ALLOW=1); off by default so an
// untrusted checkout can only tighten policy.
type LoadOptions struct {
	ProjectAllows bool
}

// LoadRuleSets loads and merges rules from the user layer (trusted) and
// the project layer (untrusted unless opts.ProjectAllows). Evaluation is
// precedence-independent because the strictest match always wins. Every
// file is self-tested; a file that fails parsing or self-testing is
// rejected whole and reported in the returned error slice — loading
// continues with the remaining files. Empty dirs are skipped.
func LoadRuleSets(userDir, projectDir string, opts LoadOptions) (*RuleSet, []error) {
	set := &RuleSet{}
	var errs []error
	load := func(dir string, trusted bool) {
		if dir == "" {
			return
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			if !os.IsNotExist(err) {
				errs = append(errs, fmt.Errorf("read rules dir %s: %w", dir, err))
			}
			return
		}
		var names []string
		for _, e := range entries {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names) // deterministic load order within a layer
		for _, name := range names {
			path := filepath.Join(dir, name)
			rules, err := loadRuleFile(path)
			if err != nil {
				errs = append(errs, err)
				continue
			}
			for _, r := range rules {
				if !trusted && domain.Decision(r.Decision) == domain.DecisionAllow {
					// Untrusted layer: allow rules are dropped (tighten-only).
					continue
				}
				set.rules = append(set.rules, r)
			}
		}
	}
	load(userDir, true)
	load(projectDir, opts.ProjectAllows)
	return set, errs
}

// loadRuleFile parses and self-tests one rule file.
func loadRuleFile(path string) ([]Rule, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read rule file %s: %w", path, err)
	}
	var f ruleFile
	if err := json.Unmarshal(data, &f); err != nil {
		return nil, fmt.Errorf("parse rule file %s: %w", path, err)
	}
	for i := range f.Rules {
		f.Rules[i].Source = path
		if err := validateRule(&f.Rules[i]); err != nil {
			return nil, fmt.Errorf("rule file %s: %w", path, err)
		}
	}
	return f.Rules, nil
}

// validateRule checks decision validity and runs the match/not_match
// self-test, mirroring codex execpolicy's load-time rule validation.
func validateRule(r *Rule) error {
	switch domain.Decision(r.Decision) {
	case domain.DecisionAllow, domain.DecisionAsk, domain.DecisionDeny:
	default:
		return fmt.Errorf("rule %v: decision must be allow|ask|deny, got %q", r.ArgvPrefix, r.Decision)
	}
	for _, ex := range r.Match {
		if !r.matches(ex) {
			return fmt.Errorf("rule %v self-test: match example %v does not hit the rule", r.ArgvPrefix, []string(ex))
		}
	}
	for _, ex := range r.NotMatch {
		if r.matches(ex) {
			return fmt.Errorf("rule %v self-test: not_match example %v hits the rule", r.ArgvPrefix, []string(ex))
		}
	}
	return nil
}

// --- Session rules: in-memory categorical prefixes remembered from "allow
// always" decisions during this process. ---

// SessionRules stores categorical run_cmd prefixes approved for the rest of
// the session. Only prefixes derived via DeriveRunCmdPrefix are stored —
// shells, eval-form interpreters, destructive programs, heredocs, and
// escalated runs are never eligible.
type SessionRules struct {
	mu       sync.RWMutex
	prefixes [][]string
}

// NewSessionRules creates an empty store.
func NewSessionRules() *SessionRules { return &SessionRules{} }

// RememberRunCmd derives and stores a categorical prefix for a run_cmd
// argv, returning the stored prefix. ok=false means the call must never be
// rule-persisted.
func (s *SessionRules) RememberRunCmd(argv []string) (prefix []string, ok bool) {
	prefix, ok = DeriveRunCmdPrefix(argv)
	if !ok {
		return nil, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, existing := range s.prefixes {
		if stringSliceEqual(existing, prefix) {
			return prefix, true
		}
	}
	s.prefixes = append(s.prefixes, prefix)
	return prefix, true
}

// Matches reports whether argv starts with any remembered prefix.
func (s *SessionRules) Matches(argv []string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, prefix := range s.prefixes {
		if ArgvHasPrefix(argv, prefix) {
			return true
		}
	}
	return false
}

// Prefixes returns a copy of the remembered prefixes (status display, tests).
func (s *SessionRules) Prefixes() [][]string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([][]string, len(s.prefixes))
	copy(out, s.prefixes)
	return out
}

// --- run_cmd argv helpers ---

// RunCmdArgv extracts [program, ...args] from run_cmd call arguments.
// Escalated calls are never rule-eligible: they run outside the sandbox and
// must always be decided per-call.
func RunCmdArgv(raw json.RawMessage) ([]string, bool) {
	var args struct {
		Program            string   `json:"program"`
		Args               []string `json:"args"`
		SandboxPermissions string   `json:"sandbox_permissions"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.Program == "" {
		return nil, false
	}
	if args.SandboxPermissions == "require_escalated" {
		return nil, false
	}
	return append([]string{args.Program}, args.Args...), true
}

// ArgvHasPrefix reports whether argv starts with prefix (exact tokens).
func ArgvHasPrefix(argv, prefix []string) bool {
	if len(prefix) == 0 || len(argv) < len(prefix) {
		return false
	}
	for i := range prefix {
		if argv[i] != prefix[i] {
			return false
		}
	}
	return true
}

// subcommandToken matches simple subcommand words (test, run, vet) used to
// widen a rule from ["go"] to ["go", "test"] without allowing arbitrary
// scripting.
var subcommandToken = regexp.MustCompile(`^[a-z][a-z0-9_+-]*$`)

// neverPersistPrograms must never start a persisted rule: they compose or
// destroy arbitrarily and do not deserve a standing approval.
var neverPersistPrograms = map[string]struct{}{
	"rm": {}, "sudo": {}, "su": {}, "dd": {}, "mkfs": {}, "shred": {},
}

// interpreterPrograms are rule-eligible ONLY when invoking a script file
// (node scripts/lx.js) or a harmless informational flag (node -v), never
// for inline evaluation (node -e, python -c) or REPL/stdin. The script path
// becomes part of the categorical prefix, so remembering "node
// ~/.loom/skills/bi-query-sql/scripts/lx.js" approves only that script —
// mirroring codex's ban on eval-form amendments while keeping skill
// workflows memorable.
var interpreterPrograms = map[string]struct{}{
	"python": {}, "python3": {}, "node": {}, "ruby": {}, "perl": {},
}

// informationalInterpreterFlags print and exit without executing user code;
// they are the ONLY flag forms interpreters may derive rules for
// (["node", "-v"] approves exactly the version probe, nothing else).
var informationalInterpreterFlags = map[string]struct{}{
	"-v": {}, "--version": {}, "-V": {}, "-h": {}, "--help": {},
}

// scriptFileToken reports whether arg looks like a script path rather than
// inline code or a flag: contains a path separator or ends with a known
// script extension.
func scriptFileToken(arg string) bool {
	if strings.ContainsAny(arg, `/\`) {
		return true
	}
	for _, ext := range []string{".js", ".mjs", ".cjs", ".ts", ".py", ".rb", ".pl"} {
		if strings.HasSuffix(arg, ext) {
			return true
		}
	}
	return false
}

// subcommandedPrograms are known subcommand-style tools for which the first
// positional argument is a categorical subcommand worth including
// (["go", "test"] instead of the too-broad ["go"]). For other programs the
// first argument is data ("rg pattern"), so the rule stays [program].
var subcommandedPrograms = map[string]struct{}{
	"go": {}, "npm": {}, "npx": {}, "pnpm": {}, "yarn": {}, "cargo": {},
	"git": {}, "bazel": {}, "docker": {}, "kubectl": {}, "helm": {}, "gh": {},
	"golangci-lint": {}, "ruff": {}, "eslint": {}, "tsc": {}, "talos": {},
}

// DeriveRunCmdPrefix computes the categorical rule prefix for a run_cmd
// argv ([program, ...args]): [program] plus the first subcommand-like
// positional argument, e.g. ["go", "test"] from ["go", "test", "./..."].
// ok=false when the call must not be persisted: shells, heredocs,
// destructive programs, or eval-form/REPL interpreters.
func DeriveRunCmdPrefix(argv []string) ([]string, bool) {
	if len(argv) == 0 {
		return nil, false
	}
	program := argv[0]
	if process.IsShellProgram(program) {
		return nil, false
	}
	base := program
	if idx := strings.LastIndexAny(base, `/\`); idx >= 0 {
		base = base[idx+1:]
	}
	base = strings.ToLower(base)
	if _, banned := neverPersistPrograms[base]; banned {
		return nil, false
	}
	for _, arg := range argv[1:] {
		if strings.Contains(arg, "<<") {
			return nil, false
		}
	}
	// Interpreters: script-file invocation or a print-and-exit
	// informational flag is eligible; eval forms (-e/-c/--eval) and bare
	// REPL or stdin are never eligible.
	if _, interp := interpreterPrograms[base]; interp {
		if len(argv) < 2 {
			return nil, false
		}
		if strings.HasPrefix(argv[1], "-") {
			if _, harmless := informationalInterpreterFlags[argv[1]]; harmless {
				return []string{program, argv[1]}, true
			}
			return nil, false
		}
		if !scriptFileToken(argv[1]) {
			return nil, false
		}
		return []string{program, argv[1]}, true
	}
	prefix := []string{program}
	if _, subcommanded := subcommandedPrograms[base]; !subcommanded {
		return prefix, true
	}
	for _, arg := range argv[1:] {
		if strings.HasPrefix(arg, "-") {
			break
		}
		if subcommandToken.MatchString(arg) {
			prefix = append(prefix, arg)
		}
		break
	}
	return prefix, true
}

func stringSliceEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// --- Persistence of remembered rules ---

// RememberedRulesFile is the user-layer file "allow always" appends to.
const RememberedRulesFile = "remembered.json"

// AppendRememberedRule persists a categorical allow rule to the user-layer
// remembered.json, creating the directory on first use. The justification
// records that the rule came from an interactive approval.
func AppendRememberedRule(rulesDir string, prefix []string) error {
	if err := os.MkdirAll(rulesDir, 0o700); err != nil {
		return err
	}
	path := filepath.Join(rulesDir, RememberedRulesFile)
	var f ruleFile
	if data, err := os.ReadFile(path); err == nil {
		if err := json.Unmarshal(data, &f); err != nil {
			return fmt.Errorf("parse %s: %w", path, err)
		}
	}
	for _, r := range f.Rules {
		if stringSliceEqual(r.ArgvPrefix, prefix) && r.Decision == string(domain.DecisionAllow) {
			return nil // already persisted
		}
	}
	f.Rules = append(f.Rules, Rule{
		ArgvPrefix:    prefix,
		Decision:      string(domain.DecisionAllow),
		Justification: "remembered from an interactive loom approval",
	})
	data, err := json.MarshalIndent(f, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o600)
}
