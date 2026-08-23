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
// Created: 2026/08/23

// Package files (schema v3) are the declarative layer of the capability
// set. One file holds any number of packages:
//
//	{"packages": [{
//	  "bind": {"argv_prefix": ["git", "push"]},
//	  "decision": "allow",
//	  "grant": {"network_full": true},
//	  "consequence": "shared-state",
//	  "justification": "everyday push",
//	  "match": ["git push origin main"],
//	  "not_match": ["git push --force"]
//	}]}
//
// Bindings: argv_prefix (categorical command family) | argv_exact |
// host (exact or "*.suffix") | path (writable directory) | tool.
// Semantics preserved from the earlier rule formats:
//
//   - match/not_match examples self-test AT LOAD (argv bindings); a
//     file that fails parsing or self-testing is rejected whole.
//   - Project-layer files are untrusted: allow packages are dropped
//     unless rules.project_allow is set, grants are stripped, and the
//     consequence ceiling is forced to confined — an untrusted checkout
//     may only tighten, never loosen.
//   - The embedded builtin set must never carry a grant or a
//     consequence above confined (build-time bug otherwise).
//   - Allow packages with a grant must bind something concrete (no
//     empty argv prefix).
package permission

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// packageFileV3 is the on-disk shape of one *.json package file.
type packageFileV3 struct {
	Packages []packageJSON `json:"packages"`
}

// packageJSON is the serialized form of one Package.
type packageJSON struct {
	Bind          bindingJSON   `json:"bind"`
	Decision      string        `json:"decision"`
	Grant         *PackageGrant `json:"grant,omitempty"`
	Consequence   string        `json:"consequence,omitempty"`
	Justification string        `json:"justification,omitempty"`
	Match         []ArgvExample `json:"match,omitempty"`
	NotMatch      []ArgvExample `json:"not_match,omitempty"`
}

// bindingJSON selects the binding kind by which field is present.
type bindingJSON struct {
	ArgvPrefix []string `json:"argv_prefix,omitempty"`
	ArgvExact  []string `json:"argv_exact,omitempty"`
	Host       string   `json:"host,omitempty"`
	Path       string   `json:"path,omitempty"`
	Tool       string   `json:"tool,omitempty"`
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

// builtinJSON is the curated set of read-only commands and domain
// policy that ships with the binary.
//
//go:embed builtin.json
var builtinJSON []byte

// builtinSource marks packages that came from the embedded set.
const builtinSource = "builtin"

// LoadBuiltinPackages parses and self-tests the embedded package set.
// A broken embedded package is a build-time bug, so the error is fatal
// to the caller.
func LoadBuiltinPackages() ([]Package, error) {
	pkgs, err := parsePackageFile(builtinJSON, builtinSource)
	if err != nil {
		return nil, fmt.Errorf("parse embedded builtin packages: %w", err)
	}
	for i := range pkgs {
		pkgs[i].Scope = ScopeBuiltin
		if !pkgs[i].Grant.IsZero() {
			return nil, fmt.Errorf("embedded builtin packages: %v must not carry a grant", pkgs[i].Bind)
		}
		if pkgs[i].MaxConsequence > ConsequenceConfined {
			return nil, fmt.Errorf("embedded builtin packages: %v must not exceed confined consequence", pkgs[i].Bind)
		}
	}
	return pkgs, nil
}

// LoadOptions controls package loading. ProjectAllows enables "allow"
// packages from the project layer (rules.project_allow); off by default
// so an untrusted checkout can only tighten policy.
type LoadOptions struct {
	ProjectAllows bool
}

// LoadPackageSets loads and merges packages from the user layer
// (trusted) and the project layer (untrusted). Every file is
// self-tested; a file that fails is rejected whole and reported in the
// returned error slice — loading continues with the remaining files.
func LoadPackageSets(userDir, projectDir string, opts LoadOptions) ([]Package, []error) {
	var out []Package
	var errs []error
	load := func(dir string, scope Scope) {
		if dir == "" {
			return
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			if !os.IsNotExist(err) {
				errs = append(errs, fmt.Errorf("read packages dir %s: %w", dir, err))
			}
			return
		}
		var names []string
		for _, e := range entries {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		for _, name := range names {
			path := filepath.Join(dir, name)
			data, err := os.ReadFile(path)
			if err != nil {
				errs = append(errs, fmt.Errorf("read package file %s: %w", path, err))
				continue
			}
			pkgs, err := parsePackageFile(data, path)
			if err != nil {
				errs = append(errs, err)
				continue
			}
			for _, p := range pkgs {
				p.Scope = scope
				if scope == ScopeProject {
					if p.Decision == domain.DecisionAllow && !opts.ProjectAllows {
						continue // untrusted layer: tighten-only
					}
					if !p.Grant.IsZero() {
						errs = append(errs, fmt.Errorf("package file %s: grant stripped from project-layer package %v", path, p.Bind))
						p.Grant = PackageGrant{}
					}
					if p.Decision == domain.DecisionAllow && p.MaxConsequence > ConsequenceConfined {
						errs = append(errs, fmt.Errorf("package file %s: consequence ceiling of project-layer package %v forced to confined", path, p.Bind))
						p.MaxConsequence = ConsequenceConfined
					}
				}
				out = append(out, p)
			}
		}
	}
	load(userDir, ScopeUser)
	load(projectDir, ScopeProject)
	return out, errs
}

// RulesDirProject is the project-layer packages directory of a workspace.
func RulesDirProject(workspaceRoot string) string {
	return filepath.Join(workspaceRoot, ".loom", "rules")
}

// parsePackageFile parses and validates one package file.
func parsePackageFile(data []byte, source string) ([]Package, error) {
	var f packageFileV3
	if err := json.Unmarshal(data, &f); err != nil {
		return nil, fmt.Errorf("parse package file %s: %w", source, err)
	}
	out := make([]Package, 0, len(f.Packages))
	for i, pj := range f.Packages {
		p, err := pj.materialize(source)
		if err != nil {
			return nil, fmt.Errorf("package file %s: package #%d: %w", source, i+1, err)
		}
		out = append(out, p)
	}
	return out, nil
}

// materialize validates one serialized package and normalizes its
// binding (host lowercase, path canonicalized and sensitive-checked).
func (pj packageJSON) materialize(source string) (Package, error) {
	bind, err := pj.Bind.materialize()
	if err != nil {
		return Package{}, err
	}
	p := Package{
		Bind:          bind,
		Decision:      domain.Decision(pj.Decision),
		Justification: pj.Justification,
		Source:        source,
	}
	switch p.Decision {
	case domain.DecisionAllow, domain.DecisionAsk, domain.DecisionDeny:
	default:
		return Package{}, fmt.Errorf("decision must be allow|ask|deny, got %q", pj.Decision)
	}
	if pj.Grant != nil {
		p.Grant = *pj.Grant
	}
	consequence, err := ParseConsequence(pj.Consequence)
	if err != nil {
		return Package{}, err
	}
	p.MaxConsequence = consequence
	if err := validateGrant(&p); err != nil {
		return Package{}, err
	}
	if err := p.selfTest(pj.Match, pj.NotMatch); err != nil {
		return Package{}, err
	}
	return p, nil
}

// materialize normalizes the binding, enforcing exactly-one-kind.
func (bj bindingJSON) materialize() (Binding, error) {
	set := 0
	for _, present := range []bool{
		len(bj.ArgvPrefix) > 0, len(bj.ArgvExact) > 0,
		bj.Host != "", bj.Path != "", bj.Tool != "",
	} {
		if present {
			set++
		}
	}
	if set != 1 {
		return Binding{}, fmt.Errorf("bind must set exactly one of argv_prefix, argv_exact, host, path, tool")
	}
	switch {
	case len(bj.ArgvPrefix) > 0:
		return Binding{Kind: BindArgv, Argv: append([]string(nil), bj.ArgvPrefix...)}, nil
	case len(bj.ArgvExact) > 0:
		return Binding{Kind: BindArgvExact, Argv: append([]string(nil), bj.ArgvExact...)}, nil
	case bj.Host != "":
		host, err := normalizeHostPattern(bj.Host)
		if err != nil {
			return Binding{}, err
		}
		return Binding{Kind: BindHost, Host: host}, nil
	case bj.Path != "":
		path, err := normalizePackagePath(bj.Path)
		if err != nil {
			return Binding{}, err
		}
		return Binding{Kind: BindPath, Path: path}, nil
	default:
		tool, err := normalizeToolName(bj.Tool)
		if err != nil {
			return Binding{}, err
		}
		return Binding{Kind: BindTool, Tool: tool}, nil
	}
}

// validateGrant enforces the grant invariants: grants only on allow
// packages with a concrete binding; unsandboxed is exclusive; write
// paths are canonicalized in place and must not name sensitive
// locations.
func validateGrant(p *Package) error {
	if p.Grant.IsZero() {
		return nil
	}
	if p.Decision != domain.DecisionAllow {
		return fmt.Errorf("grant requires decision=allow, got %q", p.Decision)
	}
	if p.Bind.Kind == BindArgv && len(p.Bind.Argv) == 0 {
		return fmt.Errorf("grant on an empty argv_prefix would widen every command")
	}
	g := &p.Grant
	if g.Unsandboxed && (g.NetworkFull || len(g.NetworkHosts) > 0 || len(g.WritablePaths) > 0 || g.GUIOpen) {
		return fmt.Errorf("grant.unsandboxed is mutually exclusive with network/write/gui_open")
	}
	for i, w := range g.WritablePaths {
		normalized, err := normalizePackagePath(w)
		if err != nil {
			return fmt.Errorf("grant.write path %q: %w", w, err)
		}
		g.WritablePaths[i] = normalized
	}
	for i, h := range g.NetworkHosts {
		normalized, err := normalizeHostPattern(h)
		if err != nil {
			return fmt.Errorf("grant.network_hosts entry %q: %w", h, err)
		}
		g.NetworkHosts[i] = normalized
	}
	return nil
}

// selfTest runs the match/not_match examples (argv bindings only).
func (p Package) selfTest(match, notMatch []ArgvExample) error {
	if p.Bind.Kind != BindArgv {
		if len(match) > 0 || len(notMatch) > 0 {
			return fmt.Errorf("match/not_match self-tests only apply to argv_prefix bindings")
		}
		return nil
	}
	for _, ex := range match {
		if !argvBindsPrefix(ex, p.Bind.Argv) {
			return fmt.Errorf("self-test: match example %v does not hit %v", []string(ex), p.Bind.Argv)
		}
	}
	for _, ex := range notMatch {
		if argvBindsPrefix(ex, p.Bind.Argv) {
			return fmt.Errorf("self-test: not_match example %v hits %v", []string(ex), p.Bind.Argv)
		}
	}
	return nil
}

// normalizeHostPattern canonicalizes a host or "*.suffix" pattern:
// scheme and port are stripped, everything lowercased.
func normalizeHostPattern(raw string) (string, error) {
	h := strings.TrimSpace(strings.ToLower(raw))
	if h == "" {
		return "", fmt.Errorf("host pattern is empty")
	}
	wildcard := strings.HasPrefix(h, "*.")
	if wildcard {
		h = h[2:]
	}
	if strings.Contains(h, "://") {
		u, err := url.Parse(h)
		if err != nil || u.Hostname() == "" {
			return "", fmt.Errorf("host pattern %q is not a valid host or URL", raw)
		}
		h = u.Hostname()
	}
	if i := strings.LastIndex(h, ":"); i >= 0 && !strings.Contains(h, "]") {
		h = h[:i] // strip port
	}
	h = strings.TrimSuffix(strings.TrimSpace(h), ".")
	if h == "" || strings.ContainsAny(h, "/?#@ ") {
		return "", fmt.Errorf("host pattern %q is not a valid host", raw)
	}
	if wildcard {
		return "*." + h, nil
	}
	return h, nil
}

// normalizePackagePath canonicalizes a binding/grant path: "~" expands,
// the result must be absolute and non-root, symlinks resolve, and any
// path that IS sensitive or COVERS a sensitive location is refused —
// a write grant must never reopen what the sensitive list closes
// (granting ~ would otherwise open ~/.ssh to a plain file-write).
func normalizePackagePath(p string) (string, error) {
	expanded := expandTilde(p)
	if !filepath.IsAbs(expanded) {
		return "", fmt.Errorf("path %q is not absolute", p)
	}
	canonical := workspacepkg.Canonicalize(expanded)
	if filepath.Dir(canonical) == canonical {
		return "", fmt.Errorf("path %q must not be the filesystem root", p)
	}
	if workspacepkg.CoversSensitiveLocation(canonical) {
		return "", fmt.Errorf("path %q is or covers a sensitive location", p)
	}
	return canonical, nil
}

// toolNameToken is the canonical tool-name shape (snake_case).
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
// categorically BY NAME. The bar: the tool's risk profile must not
// depend on its arguments — a fixed remote endpoint and no filesystem
// effect — so one interactive approval covers every future invocation.
var toolMemoryEligible = map[string]struct{}{
	"generate_image": {},
	"web_search":     {},
	"kb_search":      {},
	"kb_read":        {},
}

// mcpToolPrefix is the qualified-name prefix of MCP-sourced tools.
const mcpToolPrefix = "mcp__"

// ToolMemoryEligible reports whether a tool's "allow always" approval
// may be remembered by tool name, returning the canonical name.
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
