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
//   - Layers are merged: the user dir (<loom home>/rules, passed in by
//     the caller) plus, optionally, the project dir (<workspace>/.loom/rules).
//     Because a checked-out
//     repository is not fully trusted, project-layer "allow" rules are
//     ignored unless explicitly enabled (rules.project_allow) — untrusted
//     layers may only tighten, never loosen (same shape as codex's
//     requirements overlay).
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

// RuleKind distinguishes argv-prefix rules from domain and tool rules.
type RuleKind int

const (
	RuleArgv   RuleKind = iota // argv-prefix command rule
	RuleDomain                 // domain (host) rule
	RuleTool                   // tool-name rule
)

// String returns a short label for status messages ("argv" / "domain" / "tool").
func (k RuleKind) String() string {
	switch k {
	case RuleDomain:
		return "domain"
	case RuleTool:
		return "tool"
	}
	return "argv"
}

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
	// Grant widens the execution sandbox for allow rules (schema v2,
	// docs/PERMISSION_DESIGN.md §5). Only the user layer may carry one;
	// other layers have it stripped at load time.
	Grant *RuleGrant `json:"grant,omitempty"`
	// Match lists example invocations that MUST hit this rule (load-time
	// self-test). Entries are token arrays or strings (strings.Fields).
	Match []ArgvExample `json:"match,omitempty"`
	// NotMatch lists example invocations that MUST NOT hit this rule.
	NotMatch []ArgvExample `json:"not_match,omitempty"`
	// Source is the file the rule came from (diagnostics only, not serialized).
	Source string `json:"-"`
}

// RuleGrant carries capability widenings for an allow rule: instead of
// dropping the sandbox, the rule opens exactly the capabilities the
// command needs (docs/PERMISSION_DESIGN.md §3.2).
type RuleGrant struct {
	// Network is "full" to allow outbound network/DNS inside the sandbox.
	Network string `json:"network,omitempty"`
	// Write lists additional writable absolute paths (~ is expanded at
	// load; protected workspace subpaths stay excluded).
	Write []string `json:"write,omitempty"`
	// Unsandboxed drops the sandbox entirely (L2 trust). Mutually
	// exclusive with Network/Write/GUIOpen.
	Unsandboxed bool `json:"unsandboxed,omitempty"`
	// GUIOpen allows the command to drive macOS GUI applications via
	// `open` from inside the sandbox (docs/BROWSER_DESIGN.md §4).
	GUIOpen bool `json:"gui_open,omitempty"`
}

// ExecGrant converts the rule form into the domain contract. A nil
// receiver yields the zero grant (default sandbox).
func (g *RuleGrant) ExecGrant() domain.ExecGrant {
	if g == nil {
		return domain.ExecGrant{}
	}
	return domain.ExecGrant{
		Unsandboxed:   g.Unsandboxed,
		NetworkFull:   g.Network == "full",
		WritablePaths: append([]string(nil), g.Write...),
		GUIOpen:       g.GUIOpen,
	}
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

// ruleFile is the on-disk shape of one *.json rule file. The optional
// "domains" section holds web_fetch host rules (domain_rules.go).
type ruleFile struct {
	Rules   []Rule       `json:"rules"`
	Domains []DomainRule `json:"domains,omitempty"`
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

// RuleSet is an ordered collection of rules from one or more layers:
// argv-prefix rules for run_cmd, host rules for web_fetch, and tool-name
// rules for the fixed-blast-radius tools (tool_rules.go).
type RuleSet struct {
	rules   []Rule
	domains []DomainRule
	tools   []ToolRule
}

// Rules returns the loaded rules (for `loom rules list` and tests).
func (s *RuleSet) Rules() []Rule {
	if s == nil {
		return nil
	}
	return append([]Rule(nil), s.rules...)
}

// Domains returns the loaded domain rules (for `loom rules list` and tests).
func (s *RuleSet) Domains() []DomainRule {
	if s == nil {
		return nil
	}
	return append([]DomainRule(nil), s.domains...)
}

// Tools returns the loaded tool-name rules (for `loom rules list` and tests).
func (s *RuleSet) Tools() []ToolRule {
	if s == nil {
		return nil
	}
	return append([]ToolRule(nil), s.tools...)
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
		if f.Rules[i].Grant != nil {
			// The embedded set is curated read-only commands; grants are
			// user-layer-only by design and must never ship in builtin.
			return nil, fmt.Errorf("embedded builtin rules: rule %v must not carry a grant", f.Rules[i].ArgvPrefix)
		}
		set.rules = append(set.rules, f.Rules[i])
	}
	for i := range f.Domains {
		f.Domains[i].Source = builtinSource
		if err := validateDomainRule(&f.Domains[i]); err != nil {
			return nil, fmt.Errorf("embedded builtin rules: %w", err)
		}
		set.domains = append(set.domains, f.Domains[i])
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
	s.domains = append(s.domains, other.domains...)
	s.tools = append(s.tools, other.tools...)
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

// HasAny reports whether the set holds at least one rule of any kind
// (argv-prefix, domain, or tool-name). Callers that merge a possibly
// domain-only set (e.g. the remembered store) must gate on HasAny, not
// on a per-kind count — a store with remembered hosts but no argv rules
// would otherwise be dropped as "empty".
func (s *RuleSet) HasAny() bool {
	if s == nil {
		return false
	}
	return len(s.rules) > 0 || len(s.domains) > 0 || len(s.tools) > 0
}

// RulesDirProject is the project-layer rules directory for a workspace.
// (The user-layer directory is not computed here: it derives from the
// loom home — config.ResolvedStorage.RulesDir — and is passed in by
// the caller.)
func RulesDirProject(workspaceRoot string) string {
	return filepath.Join(workspaceRoot, ".loom", "rules")
}

// LoadOptions controls rule loading. ProjectAllows enables "allow" rules
// from the project layer (rules.project_allow); off by default so an
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
	load := func(dir string, trusted, allowGrants bool) {
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
			rules, domains, err := loadRuleFile(path)
			if err != nil {
				errs = append(errs, err)
				continue
			}
			for _, r := range rules {
				if !trusted && domain.Decision(r.Decision) == domain.DecisionAllow {
					// Untrusted layer: allow rules are dropped (tighten-only).
					continue
				}
				if !allowGrants && r.Grant != nil {
					// Grants widen the sandbox, so they are user-layer-only:
					// an untrusted checkout may never loosen the boundary,
					// even when its allow decisions are honored. Strip (not
					// drop) so the rule's decision still applies.
					errs = append(errs, fmt.Errorf("rule file %s: grant stripped from non-user-layer rule %v", path, r.ArgvPrefix))
					r.Grant = nil
				}
				set.rules = append(set.rules, r)
			}
			for _, d := range domains {
				if !trusted && domain.Decision(d.Decision) == domain.DecisionAllow {
					// Same tighten-only rule for web_fetch host allows.
					continue
				}
				set.domains = append(set.domains, d)
			}
		}
	}
	load(userDir, true, true)
	load(projectDir, opts.ProjectAllows, false)
	return set, errs
}

// loadRuleFile parses and self-tests one rule file (argv rules plus the
// optional domains section).
func loadRuleFile(path string) ([]Rule, []DomainRule, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read rule file %s: %w", path, err)
	}
	var f ruleFile
	if err := json.Unmarshal(data, &f); err != nil {
		return nil, nil, fmt.Errorf("parse rule file %s: %w", path, err)
	}
	for i := range f.Rules {
		f.Rules[i].Source = path
		if err := validateRule(&f.Rules[i]); err != nil {
			return nil, nil, fmt.Errorf("rule file %s: %w", path, err)
		}
	}
	for i := range f.Domains {
		f.Domains[i].Source = path
		if err := validateDomainRule(&f.Domains[i]); err != nil {
			return nil, nil, fmt.Errorf("rule file %s: %w", path, err)
		}
	}
	return f.Rules, f.Domains, nil
}

// validateRule checks decision and grant validity and runs the
// match/not_match self-test, mirroring codex execpolicy's load-time rule
// validation.
func validateRule(r *Rule) error {
	switch domain.Decision(r.Decision) {
	case domain.DecisionAllow, domain.DecisionAsk, domain.DecisionDeny:
	default:
		return fmt.Errorf("rule %v: decision must be allow|ask|deny, got %q", r.ArgvPrefix, r.Decision)
	}
	if err := validateRuleGrant(r); err != nil {
		return err
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

// validateRuleGrant enforces the grant invariants (§5): grants only on
// allow rules with a non-empty prefix, unsandboxed is exclusive, network
// is "full" or absent, and write paths are normalized in place.
func validateRuleGrant(r *Rule) error {
	g := r.Grant
	if g == nil {
		return nil
	}
	if domain.Decision(r.Decision) != domain.DecisionAllow {
		return fmt.Errorf("rule %v: grant requires decision=allow, got %q", r.ArgvPrefix, r.Decision)
	}
	if len(r.ArgvPrefix) == 0 {
		return fmt.Errorf("rule %v: grant on an empty argv_prefix would widen every command", r.ArgvPrefix)
	}
	if g.Unsandboxed && (g.Network != "" || len(g.Write) > 0 || g.GUIOpen) {
		return fmt.Errorf("rule %v: grant.unsandboxed is mutually exclusive with network/write/gui_open", r.ArgvPrefix)
	}
	if g.Network != "" && g.Network != "full" {
		return fmt.Errorf("rule %v: grant.network must be \"full\", got %q", r.ArgvPrefix, g.Network)
	}
	normalized := make([]string, 0, len(g.Write))
	for _, p := range g.Write {
		expanded, err := expandHome(p)
		if err != nil {
			return fmt.Errorf("rule %v: grant.write path %q: %w", r.ArgvPrefix, p, err)
		}
		if !filepath.IsAbs(expanded) {
			return fmt.Errorf("rule %v: grant.write path %q is not absolute", r.ArgvPrefix, p)
		}
		normalized = append(normalized, filepath.Clean(expanded))
	}
	g.Write = normalized
	return nil
}

// expandHome resolves a leading ~ against the user's home directory.
func expandHome(p string) (string, error) {
	if p != "~" && !strings.HasPrefix(p, "~/") {
		return p, nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	if p == "~" {
		return home, nil
	}
	return filepath.Join(home, p[2:]), nil
}

// --- Session rules: in-memory categorical prefixes remembered from "allow
// always" decisions during this process. ---

// SessionRules stores categorical run_cmd prefixes approved for the rest of
// the session, each with the capability grant the user approved. Only
// prefixes derived via DeriveRunCmdPrefix are stored — shells, eval-form
// interpreters, destructive programs, and heredocs are never eligible.
type SessionRules struct {
	mu      sync.RWMutex
	rules   []sessionRule
	domains map[string]struct{}
	tools   map[string]struct{}
}

// sessionRule is one remembered prefix plus its approved grant.
type sessionRule struct {
	prefix []string
	grant  domain.ExecGrant
}

// NewSessionRules creates an empty store.
func NewSessionRules() *SessionRules { return &SessionRules{} }

// RememberRunCmd derives and stores categorical prefixes for a run_cmd
// call together with their approved grant, returning the stored prefixes.
// A composed shell command contributes ONE prefix per subcommand
// (DeriveRunCmdPrefixes). ok=false means the call must never be
// rule-persisted.
func (s *SessionRules) RememberRunCmd(info RunCmdCall, grant domain.ExecGrant) (prefixes [][]string, ok bool) {
	prefixes, ok = DeriveRunCmdPrefixes(info)
	if !ok {
		return nil, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, prefix := range prefixes {
		duplicate := false
		for i, existing := range s.rules {
			if stringSliceEqual(existing.prefix, prefix) {
				if !execGrantsEqual(existing.grant, grant) {
					// The user's LATEST approval wins: upgrading a plain
					// memory to a granted one (and tightening a granted one)
					// both honor the most recent interactive decision.
					s.rules[i].grant = grant
				}
				duplicate = true
				break
			}
		}
		if !duplicate {
			s.rules = append(s.rules, sessionRule{prefix: prefix, grant: grant})
		}
	}
	return prefixes, true
}

// execGrantsEqual compares two grants for dedup purposes.
func execGrantsEqual(a, b domain.ExecGrant) bool {
	return a.Unsandboxed == b.Unsandboxed && a.NetworkFull == b.NetworkFull &&
		a.GUIOpen == b.GUIOpen &&
		stringSliceEqual(a.WritablePaths, b.WritablePaths)
}

// Match returns the remembered grant for the LONGEST remembered prefix
// matching argv (REVIEW M32): DeriveRunCmdPrefix derives both broad
// prefixes (["go"] from ["go", "-x", ...]) and narrow ones (["go", "test"]
// from ["go", "test", ...]), and the two coexist in the store.
// First-inserted-wins would let a broad prefix shadow the narrow one, so
// `go test ./...` would inherit the broad memory's (typically wider)
// grant and the more specific approval would never fire. Ties (equal
// length) keep insertion order.
func (s *SessionRules) Match(argv []string) (domain.ExecGrant, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.matchLocked(argv)
}

// matchLocked is Match without the lock (callers hold it).
func (s *SessionRules) matchLocked(argv []string) (domain.ExecGrant, bool) {
	var (
		grant domain.ExecGrant
		best  int
		found bool
	)
	for _, r := range s.rules {
		if len(r.prefix) > best && ArgvHasPrefix(argv, r.prefix) {
			grant, best, found = r.grant, len(r.prefix), true
		}
	}
	return grant, found
}

// MatchAll matches a composed shell command against session memory: EVERY
// subcommand argv must be covered by a remembered prefix, and the
// returned grant is the union of the matched grants (a compound command
// needs every capability its subcommands were approved with).
// ok=false when any subcommand is not remembered — one unremembered
// subcommand keeps the whole script at its normal verdict.
func (s *SessionRules) MatchAll(commands [][]string) (domain.ExecGrant, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	var union domain.ExecGrant
	for _, argv := range commands {
		grant, ok := s.matchLocked(argv)
		if !ok {
			return domain.ExecGrant{}, false
		}
		union.Unsandboxed = union.Unsandboxed || grant.Unsandboxed
		union.NetworkFull = union.NetworkFull || grant.NetworkFull
		union.GUIOpen = union.GUIOpen || grant.GUIOpen
		union.WritablePaths = append(union.WritablePaths, grant.WritablePaths...)
	}
	return union, true
}

// ForgetRunCmd removes a remembered argv prefix. ok=false means the prefix
// was not in the session store. Forgetting mirrors RememberRunCmd: the
// caller passes the DERIVED categorical prefix (as returned by
// DeriveRunCmdPrefix / RememberedRule.Prefix), not a raw argv.
func (s *SessionRules) ForgetRunCmd(prefix []string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, r := range s.rules {
		if stringSliceEqual(r.prefix, prefix) {
			s.rules = append(s.rules[:i], s.rules[i+1:]...)
			return true
		}
	}
	return false
}

// Prefixes returns a copy of the remembered prefixes (status display, tests).
func (s *SessionRules) Prefixes() [][]string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([][]string, len(s.rules))
	for i, r := range s.rules {
		out[i] = r.prefix
	}
	return out
}

// --- run_cmd argv helpers ---

// RunCmdCall is the policy-relevant shape of a run_cmd invocation: the
// argv plus the execution-mode flags the model declared.
type RunCmdCall struct {
	// Argv is [program, ...args]. For a provably simple sh -c script this
	// is the UNWRAPPED inner argv (ShellUnwrapped=true): policy — rules,
	// danger screen, memory — classifies the real command, not the shell.
	Argv []string
	// Escalated marks sandbox_permissions=require_escalated (the model
	// requests unsandboxed execution). Escalated calls are rule-eligible
	// ONLY for rules carrying an unsandboxed grant; a lesser grant never
	// covers an escalation (AllowGrantCovers).
	Escalated bool
	// NeedsNetwork marks needs_network=true: the model declares the
	// command requires outbound network inside the sandbox.
	NeedsNetwork bool
	// NeedsGUIOpen marks needs_gui_open=true: the model declares the
	// command drives macOS GUI applications (open / Apple Events) from
	// inside the sandbox (docs/BROWSER_DESIGN.md §4).
	NeedsGUIOpen bool
	// ShellUnwrapped reports that Argv was recovered from a simple
	// sh -c script (process.UnwrapSimpleShell). Execution still goes
	// through the shell; the unwrap only feeds classification.
	ShellUnwrapped bool
	// Shell is the AST analysis of a COMPOSED sh -c invocation (pipes,
	// sequencing, redirects — anything UnwrapSimpleShell cannot reduce
	// to one plain command). nil for plain argv and unwrapped shells.
	Shell *process.ShellAnalysis
}

// ShellCommandArgvs returns the per-command argvs policy should classify:
// the single (possibly unwrapped) argv for plain calls, or every
// subcommand of a composed shell script. ok=false when any subcommand has
// dynamic words — its argv cannot be proven, so prefix classification is
// unsound.
func (info RunCmdCall) ShellCommandArgvs() ([][]string, bool) {
	if info.Shell == nil {
		return [][]string{info.Argv}, true
	}
	argvs := make([][]string, 0, len(info.Shell.Commands))
	for _, cmd := range info.Shell.Commands {
		if len(cmd.Argv) == 0 {
			return nil, false
		}
		argvs = append(argvs, cmd.Argv)
	}
	return argvs, true
}

// ExecInfoOf resolves the policy-relevant execution shape of a prepared
// call. The typed PreparedCall.ExecRequest (signed by the producing tool)
// is authoritative; raw-argument parsing remains only as the fallback for
// calls constructed outside Prepare — tests, and the approval UI boundary
// where only the event payload's raw arguments survive. This is the single
// entry point that frees the policy layer from tool-name coupling
// (REVIEW M17): run_cmd and exec_session both flow through it.
func ExecInfoOf(call domain.PreparedCall) (RunCmdCall, bool) {
	if call.ExecRequest != nil && len(call.ExecRequest.Argv) > 0 {
		info := RunCmdCall{
			Argv:         append([]string(nil), call.ExecRequest.Argv...),
			Escalated:    call.ExecRequest.Escalated,
			NeedsNetwork: call.ExecRequest.NeedsNetwork,
			NeedsGUIOpen: call.ExecRequest.NeedsGUIOpen,
		}
		classifyShell(&info)
		return info, true
	}
	return ParseRunCmdCall(call.Call.Arguments)
}

// ParseRunCmdCall extracts the RunCmdCall from raw run_cmd arguments.
func ParseRunCmdCall(raw json.RawMessage) (RunCmdCall, bool) {
	var args struct {
		Program            string   `json:"program"`
		Args               []string `json:"args"`
		SandboxPermissions string   `json:"sandbox_permissions"`
		NeedsNetwork       bool     `json:"needs_network"`
		NeedsGUIOpen       bool     `json:"needs_gui_open"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.Program == "" {
		return RunCmdCall{}, false
	}
	argv := append([]string{args.Program}, args.Args...)
	info := RunCmdCall{
		Argv:         argv,
		Escalated:    args.SandboxPermissions == "require_escalated",
		NeedsNetwork: args.NeedsNetwork,
		NeedsGUIOpen: args.NeedsGUIOpen,
	}
	classifyShell(&info)
	return info, true
}

// classifyShell fills the shell-classification fields of a run_cmd shape:
// provably simple sh -c scripts are unwrapped to their inner argv;
// composed scripts keep the raw argv and carry the AST analysis instead.
func classifyShell(info *RunCmdCall) {
	if unwrapped, ok := process.UnwrapSimpleShell(info.Argv); ok {
		info.Argv = unwrapped
		info.ShellUnwrapped = true
		return
	}
	if len(info.Argv) == 0 || !process.IsShellProgram(info.Argv[0]) {
		return
	}
	if analysis, ok := process.AnalyzeShellArgv(info.Argv); ok {
		info.Shell = &analysis
	}
}

// RunCmdArgv extracts [program, ...args] from run_cmd call arguments.
// Prefer ParseRunCmdCall when the execution-mode flags matter.
func RunCmdArgv(raw json.RawMessage) ([]string, bool) {
	info, ok := ParseRunCmdCall(raw)
	if !ok {
		return nil, false
	}
	return info.Argv, true
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
	"golangci-lint": {}, "ruff": {}, "eslint": {}, "tsc": {}, "mycli": {},
}

// DeriveRunCmdPrefixes computes the categorical rule prefixes for a
// run_cmd call: one prefix for a plain argv, one PER SUBCOMMAND for a
// composed shell command. A composed command is derivable only when its
// analysis is fully static (no dynamic words, no dynamic write targets)
// and every subcommand is individually derivable — remembering
// `go test ./... && git status` stores ["go","test"] and
// ["git","status"], so any later combination of remembered subcommands
// matches while a script containing anything unremembered does not.
// ok=false when the call must not be persisted.
func DeriveRunCmdPrefixes(info RunCmdCall) ([][]string, bool) {
	if info.Shell == nil {
		prefix, ok := DeriveRunCmdPrefix(info.Argv)
		if !ok {
			return nil, false
		}
		return [][]string{prefix}, true
	}
	if !info.Shell.Static || info.Shell.DynamicWrites {
		return nil, false
	}
	argvs, ok := info.ShellCommandArgvs()
	if !ok {
		return nil, false
	}
	var prefixes [][]string
	for _, argv := range argvs {
		prefix, ok := DeriveRunCmdPrefix(argv)
		if !ok {
			return nil, false
		}
		duplicate := false
		for _, existing := range prefixes {
			if stringSliceEqual(existing, prefix) {
				duplicate = true
				break
			}
		}
		if !duplicate {
			prefixes = append(prefixes, prefix)
		}
	}
	if len(prefixes) == 0 {
		return nil, false
	}
	return prefixes, true
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
