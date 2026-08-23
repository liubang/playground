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

package config

import (
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/logging"
	"github.com/liubang/playground/go/pl/loom/internal/model/anthropic"
	"github.com/liubang/playground/go/pl/loom/internal/model/images"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/trace"
)

// EnvLookup resolves an environment variable (os.LookupEnv in production),
// injected for testability. It is used ONLY for secret references
// (api_key_env and friends), never for configuration values.
type EnvLookup func(string) (string, bool)

// ProviderModelRef identifies one resolved provider/model selection.
type ProviderModelRef struct {
	Provider string
	Model    string
}

// String renders the canonical "provider/model" form.
func (r ProviderModelRef) String() string { return r.Provider + "/" + r.Model }

// ResolvedProvider is a fully assembled provider: the domain.Model instance
// is built at load time (construction is cheap — an HTTP client config — so
// every provider is prebuilt and switching costs nothing).
type ResolvedProvider struct {
	Name         string
	Model        domain.Model
	Models       []Model
	DefaultModel string
	// WireModels holds one prebuilt instance per distinct wire_api in the
	// catalog, keyed by wire-api short name ("chat"/"responses"/"messages").
	// Models may override the provider-level wire_api; without this the
	// override would be silently ignored.
	WireModels map[string]domain.Model
}

// ModelFor returns the model instance speaking the wire API configured for
// the named model. Unknown names degrade to the provider default instance.
func (p *ResolvedProvider) ModelFor(modelName string) domain.Model {
	if meta, ok := p.modelMeta(modelName); ok {
		if inst, ok := p.WireModels[meta.WireAPI]; ok {
			return inst
		}
	}
	return p.Model
}

// WireAPIFor returns the effective wire API short name for the named
// model ("chat"/"responses"/"messages"); model entries already inherit
// the provider default at resolve time. Unknown names conservatively
// report "messages" (no structured-output support assumed).
func (p *ResolvedProvider) WireAPIFor(modelName string) string {
	if meta, ok := p.modelMeta(modelName); ok {
		return meta.WireAPI
	}
	return "messages"
}

// modelMeta returns the metadata for the named model.
func (p *ResolvedProvider) modelMeta(name string) (Model, bool) {
	for _, m := range p.Models {
		if m.Name == name {
			return m, true
		}
	}
	return Model{}, false
}

// ResolvedConfig is the validated, secret-resolved, fully assembled
// configuration. Consumers never touch the raw File schema.
type ResolvedConfig struct {
	Providers     []ResolvedProvider
	Default       ProviderModelRef
	Limits        domain.Limits
	Context       domain.ContextConfig
	Runaway       domain.RunawayConfig
	Prompt        Prompt
	Skills        ResolvedSkills
	Rules         ResolvedRules
	Approval      ResolvedApproval
	Tools         ResolvedTools
	Tracing       trace.Config
	Storage       ResolvedStorage
	Share         ResolvedShare
	Logging       ResolvedLogging
	UI            UI
	Subagent      ResolvedSubagent
	Memory        ResolvedMemory
	Sessions      ResolvedSessions
	Image         ResolvedImage
	Browser       ResolvedBrowser
	KnowledgeBase ResolvedKnowledgeBase
	MCP           ResolvedMCP
	// Workspaces are the pre-registered project workspaces (docs/WORKSPACE_DESIGN.md §10).
	Workspaces []ResolvedWorkspace
}

// ResolvedWorkspace is one pre-registered workspace with its root resolved
// to an absolute path ("~" expanded). Existence and canonicalization are
// enforced at registration time, not load time.
type ResolvedWorkspace struct {
	Name string
	Root string
}

// ResolvedShare is the share section with defaults applied.
type ResolvedShare struct {
	Enabled bool
	Listen  string
}

// ResolvedTools is the tools section with defaults applied.
type ResolvedTools struct {
	// PathExtra holds the additional PATH directories, "~" expanded and
	// validated absolute.
	PathExtra []string
}

// resolveTools expands and validates tools.path_extra: entries must
// resolve to absolute paths (a relative PATH entry resolves against each
// spawned command's working directory — a security smell worth rejecting
// at load time).
func resolveTools(in Tools) (ResolvedTools, error) {
	var out ResolvedTools
	for i, raw := range in.PathExtra {
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" {
			return ResolvedTools{}, fmt.Errorf("config: tools.path_extra[%d]: entry is empty", i)
		}
		expanded, err := expandHomeDir(trimmed)
		if err != nil {
			return ResolvedTools{}, fmt.Errorf("config: tools.path_extra[%d]: %w", i, err)
		}
		if !filepath.IsAbs(expanded) {
			return ResolvedTools{}, fmt.Errorf("config: tools.path_extra[%d]: must be an absolute path, got %q", i, raw)
		}
		out.PathExtra = append(out.PathExtra, expanded)
	}
	return out, nil
}

// resolveSkills applies the skills defaults and expands a leading "~" in
// extra_roots, so every directory-typed config field accepts the same home
// shorthand (workspaces[].root and tools.path_extra already do). Blank
// entries are dropped rather than rejected: the settings UI edits the list
// as one-line-per-entry text, where stray blank lines are easy to leave.
func resolveSkills(in Skills) (ResolvedSkills, error) {
	out := ResolvedSkills{
		Enabled:  in.Enabled == nil || *in.Enabled,
		Disabled: in.Disabled,
	}
	for i, raw := range in.ExtraRoots {
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" {
			continue
		}
		expanded, err := expandHomeDir(trimmed)
		if err != nil {
			return ResolvedSkills{}, fmt.Errorf("config: skills.extra_roots[%d]: %w", i, err)
		}
		out.ExtraRoots = append(out.ExtraRoots, expanded)
	}
	return out, nil
}

// DefaultShareListen is the built-in bind address for the LAN share
// listener: all interfaces on a fixed port — distinct from `loom
// serve`'s default 7680 so both may run on one machine.
const DefaultShareListen = "0.0.0.0:7681"

// resolveShare overlays the share section onto the defaults and
// validates the bind address (host:port, numeric port 1-65535; a port
// of 0 would make share links unreproducible across restarts).
func resolveShare(in Share) (ResolvedShare, error) {
	out := ResolvedShare{
		Enabled: in.Enabled != nil && *in.Enabled,
		Listen:  DefaultShareListen,
	}
	raw := strings.TrimSpace(in.Listen)
	if raw == "" {
		return out, nil
	}
	_, port, err := net.SplitHostPort(raw)
	if err != nil {
		return ResolvedShare{}, fmt.Errorf("config: share.listen: expected host:port, got %q", raw)
	}
	p, err := strconv.Atoi(port)
	if err != nil || p < 1 || p > 65535 {
		return ResolvedShare{}, fmt.Errorf("config: share.listen: port must be 1-65535, got %q", port)
	}
	out.Listen = raw
	return out, nil
}

// ResolvedLogging is the logging section with defaults applied and MiB
// converted to bytes (logging.Quotas 直接可用）。
type ResolvedLogging struct {
	MaxFileBytes  int64
	MaxTotalBytes int64
}

// ResolvedMemory is the memory section with defaults applied.
type ResolvedMemory struct {
	Enabled bool
	// ExtractModel pins the Phase 1 extraction model; nil follows Default.
	ExtractModel *ProviderModelRef
	// ConsolidationModel pins the Phase 2 consolidation model; nil follows
	// Default.
	ConsolidationModel *ProviderModelRef
	// MaxJobsPerRun bounds Phase 1 jobs claimed per pipeline pass.
	MaxJobsPerRun int
	// RunInterval re-runs the pipeline periodically; 0 runs it once at
	// startup only.
	RunInterval time.Duration
	// MinSessionIdle skips sessions touched more recently than this.
	MinSessionIdle time.Duration
	// MaxSessionAge skips sessions last touched longer ago than this.
	MaxSessionAge time.Duration
}

// ResolvedStorage carries the loom home — the single root for every
// loom data location. BaseDir comes from the home locator (LOOM_HOME or
// the default), never from configuration itself: a config file pointing
// at its own data root would be a self-referential knob. The derived
// accessors below are the only sanctioned way to compute data locations,
// so no other code may hard-code ~/.loom.
type ResolvedStorage struct {
	BaseDir string
}

// HomeEnv locates the loom home directory. It is a *locator* (like
// CARGO_HOME), not configuration itself: config, logs, sessions,
// memories, rules, and skills all live under it.
const HomeEnv = "LOOM_HOME"

// DefaultHomeDir returns ~/.loom — the default loom home.
func DefaultHomeDir() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("config: resolve user home: %w", err)
	}
	return filepath.Join(home, ".loom"), nil
}

// HomeDir resolves the active loom home: $LOOM_HOME when set, otherwise
// DefaultHomeDir. The result is always absolute.
func HomeDir(lookup EnvLookup) (string, error) {
	if lookup == nil {
		lookup = os.LookupEnv
	}
	if dir, ok := lookup(HomeEnv); ok && strings.TrimSpace(dir) != "" {
		abs, err := filepath.Abs(strings.TrimSpace(dir))
		if err != nil {
			return "", fmt.Errorf("config: resolve %s: %w", HomeEnv, err)
		}
		return abs, nil
	}
	return DefaultHomeDir()
}

// ConfigPathForHome returns the config file path within a loom home.
func ConfigPathForHome(home string) string { return filepath.Join(home, FileName) }

// SessionsDir is the session data directory: sessions.db plus its
// sibling artifacts/, prompt_cache/, serve.token, and loom.lock.
func (s ResolvedStorage) SessionsDir() string { return filepath.Join(s.BaseDir, "sessions") }

// SessionDBPath is the SQLite session store path.
func (s ResolvedStorage) SessionDBPath() string {
	return filepath.Join(s.SessionsDir(), "sessions.db")
}

// LogsDir is the file-log directory (loom.YYYY-MM-DD.log).
func (s ResolvedStorage) LogsDir() string { return filepath.Join(s.BaseDir, "logs") }

// CacheDir is the regenerable runtime cache root (login-shell PATH probe
// snapshot, ...): losing it costs one recompute, never correctness.
func (s ResolvedStorage) CacheDir() string { return filepath.Join(s.BaseDir, "cache") }

// MemoriesDir is the long-term memory store root.
func (s ResolvedStorage) MemoriesDir() string { return filepath.Join(s.BaseDir, "memories") }

// RulesDir is the user-layer permission rules directory (plus the
// remembered-approvals store).
func (s ResolvedStorage) RulesDir() string { return filepath.Join(s.BaseDir, "rules") }

// SkillsDir is the user-scope skills discovery root.
func (s ResolvedStorage) SkillsDir() string { return filepath.Join(s.BaseDir, "skills") }

// LoomMDPath is the user-global rule file injected into every prompt.
func (s ResolvedStorage) LoomMDPath() string { return filepath.Join(s.BaseDir, "LOOM.md") }

// resolveWorkspaces validates the workspaces section and resolves each root
// to an absolute path (with "~" home expansion, docs/WORKSPACE_DESIGN.md §10).
// Existence/canonicalization are enforced at registration, not here.
func resolveWorkspaces(in []WorkspaceSpec) ([]ResolvedWorkspace, error) {
	out := make([]ResolvedWorkspace, 0, len(in))
	for i, ws := range in {
		root := strings.TrimSpace(ws.Root)
		if root == "" {
			return nil, fmt.Errorf("config: workspaces[%d]: root is required", i)
		}
		expanded, err := expandHomeDir(root)
		if err != nil {
			return nil, fmt.Errorf("config: workspaces[%d]: %w", i, err)
		}
		abs, err := filepath.Abs(expanded)
		if err != nil {
			return nil, fmt.Errorf("config: workspaces[%d]: %w", i, err)
		}
		out = append(out, ResolvedWorkspace{Name: strings.TrimSpace(ws.Name), Root: abs})
	}
	return out, nil
}

// expandHomeDir replaces a leading "~" with the user's home directory.
func expandHomeDir(path string) (string, error) {
	if path != "~" && !strings.HasPrefix(path, "~/") {
		return path, nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	if path == "~" {
		return home, nil
	}
	return filepath.Join(home, path[2:]), nil
}

// ResolvedBrowser is the browser section with defaults applied.
type ResolvedBrowser struct {
	Enabled        bool
	ChromePath     string
	CdpURL         string
	IdleTTL        time.Duration
	NavTimeout     time.Duration
	ScreenshotQual int
	ViewportW      int
	ViewportH      int
}

// Default browser constants.
const (
	defaultBrowserIdleTTL     = 5 * time.Minute
	defaultBrowserNavTimeout  = 30 * time.Second
	defaultBrowserScreenshotQ = 80
	defaultBrowserViewportW   = 1280
	defaultBrowserViewportH   = 720
	minBrowserNavTimeoutMs    = 5000
	maxBrowserNavTimeoutMs    = 120000
	minBrowserScreenshotQ     = 10
	maxBrowserScreenshotQ     = 100
	minBrowserViewportDim     = 320
	maxBrowserViewportDim     = 4096
)

// resolveBrowser overlays the file's browser section onto built-in defaults.
func resolveBrowser(in Browser) (ResolvedBrowser, error) {
	out := ResolvedBrowser{
		Enabled:        in.Enabled == nil || *in.Enabled,
		IdleTTL:        defaultBrowserIdleTTL,
		NavTimeout:     defaultBrowserNavTimeout,
		ScreenshotQual: defaultBrowserScreenshotQ,
		ViewportW:      defaultBrowserViewportW,
		ViewportH:      defaultBrowserViewportH,
	}
	chromePath, err := expandHomeDir(strings.TrimSpace(in.ChromePath))
	if err != nil {
		return ResolvedBrowser{}, fmt.Errorf("config: browser.chrome_path: %w", err)
	}
	out.ChromePath = chromePath
	out.CdpURL = strings.TrimSpace(in.CdpURL)
	if out.CdpURL != "" {
		u, err := url.Parse(out.CdpURL)
		if err != nil || u.Host == "" {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.cdp_url: must be a valid ws:// or http:// URL, got %q", in.CdpURL)
		}
		if u.Scheme != "ws" && u.Scheme != "wss" && u.Scheme != "http" && u.Scheme != "https" {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.cdp_url: scheme must be ws://, wss://, http://, or https://, got %q", in.CdpURL)
		}
	}
	if in.IdleTTL != "" {
		v, err := time.ParseDuration(in.IdleTTL)
		if err != nil {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.idle_ttl: expected a Go duration (e.g. \"5m\"), got %q", in.IdleTTL)
		}
		if v < 0 {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.idle_ttl must be >= 0")
		}
		out.IdleTTL = v
	}
	if in.NavTimeoutMs != 0 {
		if in.NavTimeoutMs < minBrowserNavTimeoutMs || in.NavTimeoutMs > maxBrowserNavTimeoutMs {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.nav_timeout_ms must be between %d and %d", minBrowserNavTimeoutMs, maxBrowserNavTimeoutMs)
		}
		out.NavTimeout = time.Duration(in.NavTimeoutMs) * time.Millisecond
	}
	if in.ScreenshotQ != 0 {
		if in.ScreenshotQ < minBrowserScreenshotQ || in.ScreenshotQ > maxBrowserScreenshotQ {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.screenshot_quality must be between %d and %d", minBrowserScreenshotQ, maxBrowserScreenshotQ)
		}
		out.ScreenshotQual = in.ScreenshotQ
	}
	if in.ViewportWidth != 0 {
		if in.ViewportWidth < minBrowserViewportDim || in.ViewportWidth > maxBrowserViewportDim {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.viewport_width must be between %d and %d", minBrowserViewportDim, maxBrowserViewportDim)
		}
		out.ViewportW = in.ViewportWidth
	}
	if in.ViewportHeight != 0 {
		if in.ViewportHeight < minBrowserViewportDim || in.ViewportHeight > maxBrowserViewportDim {
			return ResolvedBrowser{}, fmt.Errorf("config: browser.viewport_height must be between %d and %d", minBrowserViewportDim, maxBrowserViewportDim)
		}
		out.ViewportH = in.ViewportHeight
	}
	return out, nil
}

// ResolvedImage is the image section with defaults applied. Generator is
// the prebuilt images client (constructed at load time like chat
// providers); it is nil whenever Enabled is false.
type ResolvedImage struct {
	Enabled  bool
	Provider string
	Model    string
	// Size/Quality are the generation defaults; empty means "auto".
	Size      string
	Quality   string
	Generator images.Generator
}

// ResolvedSubagent is the subagent section with defaults applied
// (docs/SUBAGENT_DESIGN.md §7).
type ResolvedSubagent struct {
	Enabled bool
	// MaxTokens caps the child run's cumulative metered tokens; 0 inherits
	// limits.max_tokens.
	MaxTokens int64
	// MaxOutputTokens caps each child model response; 0 inherits
	// limits.max_output_tokens.
	MaxOutputTokens int64
	// Model pins the sub-agent's model selection; nil follows the current
	// turn's model.
	Model *ProviderModelRef
}

// ResolvedSkills is the skills section with defaults applied.
type ResolvedSkills struct {
	Enabled    bool
	ExtraRoots []string
	// Disabled carries the configured skill names to suppress at load
	// time. It is hot-applied to every assembled workspace loader, unlike
	// Enabled/ExtraRoots which are frozen at assembly.
	Disabled []string
}

// ResolvedRules is the rules section with defaults applied. The zero-value
// bools are load-bearing defaults chosen in resolveRules.
type ResolvedRules struct {
	Enabled           bool
	Builtin           bool
	Project           bool
	ProjectAllow      bool
	PersistRemembered bool
}

// ResolvedApproval is the approval section with defaults applied.
type ResolvedApproval struct {
	Mode permission.ApprovalMode
	// TrustUserURLs enables the user-intent decider (auto-allow hosts the
	// user mentioned); defaults to true.
	TrustUserURLs bool
}

// ProviderByName returns the named provider, or nil.
func (c *ResolvedConfig) ProviderByName(name string) *ResolvedProvider {
	for i := range c.Providers {
		if c.Providers[i].Name == name {
			return &c.Providers[i]
		}
	}
	return nil
}

// ModelMeta returns the metadata of the referenced model.
func (c *ResolvedConfig) ModelMeta(ref ProviderModelRef) (Model, bool) {
	p := c.ProviderByName(ref.Provider)
	if p == nil {
		return Model{}, false
	}
	return p.modelMeta(ref.Model)
}

// ResolveRef parses a user-supplied model reference (the /model argument or
// the config "default" key) into a concrete selection:
//
//  1. "provider/model" — exact match;
//  2. bare name matching a provider — that provider's default model;
//  3. bare model name — must match exactly one provider's catalog;
//
// Any ambiguity or miss is an error listing the candidates.
func (c *ResolvedConfig) ResolveRef(ref string) (ProviderModelRef, error) {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return ProviderModelRef{}, fmt.Errorf("model reference must not be empty")
	}
	if strings.Contains(ref, "/") {
		parts := strings.SplitN(ref, "/", 2)
		p := c.ProviderByName(parts[0])
		if p == nil {
			return ProviderModelRef{}, fmt.Errorf("unknown provider %q in %q (have: %s)", parts[0], ref, c.providerNames())
		}
		if _, ok := p.modelMeta(parts[1]); !ok {
			return ProviderModelRef{}, fmt.Errorf("provider %q has no model %q (have: %s)", parts[0], parts[1], strings.Join(modelNames(p), ", "))
		}
		return ProviderModelRef{Provider: parts[0], Model: parts[1]}, nil
	}
	if p := c.ProviderByName(ref); p != nil {
		return ProviderModelRef{Provider: p.Name, Model: p.DefaultModel}, nil
	}
	var matches []ProviderModelRef
	for i := range c.Providers {
		if _, ok := c.Providers[i].modelMeta(ref); ok {
			matches = append(matches, ProviderModelRef{Provider: c.Providers[i].Name, Model: ref})
		}
	}
	switch len(matches) {
	case 0:
		return ProviderModelRef{}, fmt.Errorf("unknown model %q (have: %s)", ref, c.allRefs())
	case 1:
		return matches[0], nil
	default:
		refs := make([]string, len(matches))
		for i, m := range matches {
			refs[i] = m.String()
		}
		return ProviderModelRef{}, fmt.Errorf("model %q is ambiguous: %s — use the provider/model form", ref, strings.Join(refs, ", "))
	}
}

func (c *ResolvedConfig) providerNames() string {
	names := make([]string, len(c.Providers))
	for i, p := range c.Providers {
		names[i] = p.Name
	}
	return strings.Join(names, ", ")
}

// allRefs lists every selectable provider/model reference, sorted for
// stable error messages.
func (c *ResolvedConfig) allRefs() string {
	var refs []string
	for i := range c.Providers {
		for _, m := range c.Providers[i].Models {
			refs = append(refs, c.Providers[i].Name+"/"+m.Name)
		}
	}
	sort.Strings(refs)
	return strings.Join(refs, ", ")
}

func modelNames(p *ResolvedProvider) []string {
	names := make([]string, len(p.Models))
	for i, m := range p.Models {
		names[i] = m.Name
	}
	return names
}

// resolve validates the raw file, resolves secrets, builds provider
// instances, and applies built-in defaults. Any problem is a hard error —
// a configuration that silently half-applies is worse than no run at all
// (docs/CONFIG_DESIGN.md §7). baseDir is the loom home (LOOM_HOME or the
// default) — storage is not configurable.
func resolve(f *File, baseDir string, lookup EnvLookup) (*ResolvedConfig, error) {
	if lookup == nil {
		return nil, fmt.Errorf("config: env lookup is required")
	}
	if baseDir == "" {
		return nil, fmt.Errorf("config: base dir is required")
	}
	providers, auths, err := resolveProviders(f.Providers, lookup)
	if err != nil {
		return nil, err
	}
	limits, err := resolveLimits(f.Limits)
	if err != nil {
		return nil, err
	}
	contextCfg, err := resolveContext(f.Context)
	if err != nil {
		return nil, err
	}
	runawayCfg, err := resolveRunaway(f.Runaway)
	if err != nil {
		return nil, err
	}
	tracing, err := resolveTracing(f.Tracing, lookup)
	if err != nil {
		return nil, err
	}
	mode, err := permission.ParseApprovalMode(f.Approval.Mode)
	if err != nil {
		return nil, fmt.Errorf("config: %w", err)
	}
	approval := ResolvedApproval{
		Mode:          mode,
		TrustUserURLs: f.Approval.TrustUserURLs == nil || *f.Approval.TrustUserURLs,
	}
	share, err := resolveShare(f.Share)
	if err != nil {
		return nil, err
	}
	tools, err := resolveTools(f.Tools)
	if err != nil {
		return nil, err
	}
	skills, err := resolveSkills(f.Skills)
	if err != nil {
		return nil, err
	}
	out := &ResolvedConfig{
		Providers: providers,
		Limits:    limits,
		Context:   contextCfg,
		Runaway:   runawayCfg,
		Prompt:    f.Prompt,
		Skills:    skills,
		Rules:     resolveRules(f.Rules),
		Approval:  approval,
		Tracing:   tracing,
		Storage:   ResolvedStorage{BaseDir: baseDir},
		Share:     share,
		Tools:     tools,
		Logging:   resolveLogging(f.Logging),
		UI:        f.UI,
	}
	if len(providers) > 0 {
		def := f.Default
		if def == "" {
			// Implicit default: the first provider's default model, so a
			// single-provider file need not spell "default" out (§4.2).
			def = providers[0].Name + "/" + providers[0].DefaultModel
		}
		ref, err := out.ResolveRef(def)
		if err != nil {
			return nil, fmt.Errorf("config: default: %w", err)
		}
		out.Default = ref
	}
	sub := ResolvedSubagent{
		Enabled:         f.Subagent.Enabled == nil || *f.Subagent.Enabled,
		MaxOutputTokens: 8192,
	}
	if f.Subagent.MaxTokens != nil {
		if *f.Subagent.MaxTokens < 0 {
			return nil, fmt.Errorf("config: subagent.max_tokens must be >= 0")
		}
		sub.MaxTokens = *f.Subagent.MaxTokens
	}
	if f.Subagent.MaxOutputTokens != nil {
		if *f.Subagent.MaxOutputTokens < 0 {
			return nil, fmt.Errorf("config: subagent.max_output_tokens must be >= 0")
		}
		sub.MaxOutputTokens = *f.Subagent.MaxOutputTokens
	}
	if m := strings.TrimSpace(f.Subagent.Model); m != "" {
		ref, err := out.ResolveRef(m)
		if err != nil {
			return nil, fmt.Errorf("config: subagent.model: %w", err)
		}
		sub.Model = &ref
	}
	out.Subagent = sub

	// Memory: default enabled when absent or explicitly true.
	memory, err := resolveMemory(f.Memory, out)
	if err != nil {
		return nil, err
	}
	out.Memory = memory

	// Session lifecycle maintenance (auto-archive): opt-in, off by default.
	sessions, err := resolveSessions(f.Sessions)
	if err != nil {
		return nil, err
	}
	out.Sessions = sessions

	// Text-to-image: reuses a named provider's endpoint and credentials.
	image, err := resolveImage(f.Image, auths)
	if err != nil {
		return nil, err
	}
	out.Image = image

	// Headless browser tool.
	browser, err := resolveBrowser(f.Browser)
	if err != nil {
		return nil, err
	}
	out.Browser = browser

	// MCP servers: validate config-level constraints; runtime startup
	// (process spawning, tool discovery) happens in bootstrap.go.
	resolvedMCP, err := resolveMCP(f.MCPServers, lookup)
	if err != nil {
		return nil, err
	}
	out.MCP = resolvedMCP

	// Knowledge base tools (kb_search/kb_read): opt-in, like image and
	// browser — an unconfigured deployment never advertises the tools.
	kb, err := resolveKnowledgeBase(f.KnowledgeBase)
	if err != nil {
		return nil, err
	}
	out.KnowledgeBase = kb

	workspaces, err := resolveWorkspaces(f.Workspaces)
	if err != nil {
		return nil, err
	}
	out.Workspaces = workspaces

	return out, nil
}

// providerAuth carries a resolved provider's connection credentials for
// sibling clients that share the endpoint (the images generator); the API
// key never leaves the config package except inside prebuilt clients.
type providerAuth struct {
	pType   string
	baseURL string
	apiKey  string
}

// resolveProviders validates uniqueness and required fields, resolves each
// API key, and prebuilds one domain.Model per provider. It also returns the
// per-provider credentials for endpoint-sharing clients (image generation).
func resolveProviders(in []Provider, lookup EnvLookup) ([]ResolvedProvider, map[string]providerAuth, error) {
	auths := make(map[string]providerAuth, len(in))
	out, err := resolveProviderList(in, lookup, auths)
	if err != nil {
		return nil, nil, err
	}
	return out, auths, nil
}

// resolveProviderList is the validation/assembly loop; resolved
// credentials are recorded into auths for endpoint-sharing clients.
func resolveProviderList(in []Provider, lookup EnvLookup, auths map[string]providerAuth) ([]ResolvedProvider, error) {
	seen := make(map[string]bool, len(in))
	out := make([]ResolvedProvider, 0, len(in))
	for i := range in {
		p := in[i]
		// The inheritance expansion below writes defaults back into model
		// entries (wire_api, reasoning); a slice-header copy would share the
		// caller's backing array and mutate the File it passed in — which
		// Validate callers then serialize to disk (PUT /v1/config).
		p.Models = append([]Model(nil), p.Models...)
		ctx := fmt.Sprintf("providers[%d] (%q)", i, p.Name)
		if p.Name == "" {
			return nil, fmt.Errorf("config: providers[%d]: name is required", i)
		}
		if strings.Contains(p.Name, "/") {
			return nil, fmt.Errorf("config: %s: provider name must not contain '/' (it is the provider/model reference separator)", ctx)
		}
		if seen[p.Name] {
			return nil, fmt.Errorf("config: duplicate provider name %q", p.Name)
		}
		seen[p.Name] = true
		pType := strings.TrimSpace(p.Type)
		if pType == "" {
			pType = "openai"
		}
		switch pType {
		case "openai", "anthropic":
		default:
			return nil, fmt.Errorf("config: %s: unsupported type %q (supported: \"openai\", \"anthropic\")", ctx, p.Type)
		}
		// base_url is mandatory: openai.New silently falls back to the
		// official OpenAI endpoint when empty, which would send a foreign
		// key and the whole conversation to the wrong host (§4.2).
		if strings.TrimSpace(p.BaseURL) == "" {
			return nil, fmt.Errorf("config: %s: base_url is required", ctx)
		}
		if u, err := url.Parse(p.BaseURL); err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
			return nil, fmt.Errorf("config: %s: base_url must be an absolute http(s) URL, got %q", ctx, p.BaseURL)
		}
		apiKey, err := resolveSecret(ctx, "api_key", p.APIKey, p.APIKeyEnv, lookup)
		if err != nil {
			return nil, err
		}
		// The wire_api vocabulary is protocol-specific: OpenAI-compatible
		// providers speak "chat" or "responses"; Anthropic has exactly one
		// Messages API and accepts only "messages" (or the default).
		wireAPI, wireAPIName, err := resolveProviderWireAPI(ctx, pType, p.WireAPI)
		if err != nil {
			return nil, err
		}
		if err := resolveReasoning(ctx, p.Reasoning); err != nil {
			return nil, err
		}
		if err := resolveAuthType(ctx, pType, p.AuthType); err != nil {
			return nil, err
		}
		if len(p.Models) == 0 {
			return nil, fmt.Errorf("config: %s: at least one model is required", ctx)
		}
		retries := 2
		if p.MaxRetries != nil {
			if *p.MaxRetries < 0 {
				return nil, fmt.Errorf("config: %s: max_retries must be >= 0", ctx)
			}
			retries = *p.MaxRetries
		}
		modelSeen := make(map[string]bool, len(p.Models))
		for j := range p.Models {
			m := &p.Models[j]
			mctx := fmt.Sprintf("%s models[%d] (%q)", ctx, j, m.Name)
			if m.Name == "" {
				return nil, fmt.Errorf("config: %s models[%d]: name is required", ctx, j)
			}
			if strings.Contains(m.Name, "/") {
				return nil, fmt.Errorf("config: %s: model name must not contain '/'", mctx)
			}
			if modelSeen[m.Name] {
				return nil, fmt.Errorf("config: %s: duplicate model name %q", ctx, m.Name)
			}
			modelSeen[m.Name] = true
			if m.ContextWindow < 0 || m.MaxOutputTokens < 0 {
				return nil, fmt.Errorf("config: %s: context_window and max_output_tokens must be >= 0", mctx)
			}
			if m.WindowUtilization != nil && (*m.WindowUtilization <= 0 || *m.WindowUtilization > 1) {
				return nil, fmt.Errorf("config: %s: window_utilization must be in (0, 1], got %v", mctx, *m.WindowUtilization)
			}
			if _, _, err := resolveProviderWireAPI(mctx, pType, m.WireAPI); err != nil {
				return nil, err
			}
			// Expand inheritance so consumers never re-implement the fallback:
			// a model without an explicit wire_api speaks the provider's.
			if strings.TrimSpace(m.WireAPI) == "" {
				m.WireAPI = wireAPIName
			}
			if err := resolveReasoning(mctx, m.Reasoning); err != nil {
				return nil, err
			}
			// Same expansion for reasoning: a model without an opinion
			// inherits the provider-level default.
			if strings.TrimSpace(m.Reasoning.Effort) == "" && m.Reasoning.BudgetTokens == 0 {
				m.Reasoning = p.Reasoning
			}
		}
		defaultModel := p.DefaultModel
		if defaultModel == "" {
			defaultModel = p.Models[0].Name
		} else if !modelSeen[defaultModel] {
			return nil, fmt.Errorf("config: %s: default_model %q is not in its models list", ctx, defaultModel)
		}
		instance, err := buildProvider(pType, p, apiKey, wireAPI, retries)
		if err != nil {
			// Assembly failure (e.g. an unparseable base_url) is a config
			// error, not a runtime one — fail fast with full context.
			return nil, fmt.Errorf("config: %s: %w", ctx, err)
		}
		// Models may override the wire API individually (e.g. a reasoner
		// speaking "responses" while the provider default is "chat").
		// Prebuild one instance per distinct API so switching costs nothing.
		wireModels := map[string]domain.Model{wireAPIName: instance}
		for j := range p.Models {
			name := p.Models[j].WireAPI // already expanded to the provider default above
			if _, ok := wireModels[name]; ok {
				continue
			}
			modelWireAPI, _, err := resolveProviderWireAPI(ctx, pType, name)
			if err != nil {
				return nil, err
			}
			inst, err := buildProvider(pType, p, apiKey, modelWireAPI, retries)
			if err != nil {
				return nil, fmt.Errorf("config: %s: %w", ctx, err)
			}
			wireModels[name] = inst
		}
		auths[p.Name] = providerAuth{pType: pType, baseURL: strings.TrimSpace(p.BaseURL), apiKey: apiKey}
		out = append(out, ResolvedProvider{
			Name:         p.Name,
			Model:        instance,
			Models:       p.Models,
			DefaultModel: defaultModel,
			WireModels:   wireModels,
		})
	}
	return out, nil
}

// resolveImage validates the image section and prebuilds the images client
// from the named provider's endpoint and credentials. Generation is opt-in:
// the section stays disabled when entirely absent or when enabled is
// explicitly false.
func resolveImage(in Image, auths map[string]providerAuth) (ResolvedImage, error) {
	provider := strings.TrimSpace(in.Provider)
	model := strings.TrimSpace(in.Model)
	if in.Enabled != nil && !*in.Enabled {
		return ResolvedImage{}, nil
	}
	if provider == "" && model == "" {
		if in.Enabled != nil {
			return ResolvedImage{}, fmt.Errorf("config: image: provider and model are required when enabled")
		}
		if in.Size != "" || in.Quality != "" {
			return ResolvedImage{}, fmt.Errorf("config: image: size/quality require provider and model")
		}
		return ResolvedImage{}, nil
	}
	if provider == "" || model == "" {
		return ResolvedImage{}, fmt.Errorf("config: image: provider and model must both be set")
	}
	auth, ok := auths[provider]
	if !ok {
		return ResolvedImage{}, fmt.Errorf("config: image: unknown provider %q", provider)
	}
	if auth.pType != "openai" {
		return ResolvedImage{}, fmt.Errorf("config: image: provider %q is %q; only openai-type providers expose an Images API", provider, auth.pType)
	}
	if in.Size != "" && !images.ValidSize(in.Size) {
		return ResolvedImage{}, fmt.Errorf("config: image: invalid size %q", in.Size)
	}
	if in.Quality != "" && !images.ValidQuality(in.Quality) {
		return ResolvedImage{}, fmt.Errorf("config: image: invalid quality %q", in.Quality)
	}
	gen, err := images.NewOpenAI(images.Config{BaseURL: auth.baseURL, APIKey: auth.apiKey})
	if err != nil {
		return ResolvedImage{}, fmt.Errorf("config: image: %w", err)
	}
	return ResolvedImage{
		Enabled:   true,
		Provider:  provider,
		Model:     model,
		Size:      in.Size,
		Quality:   in.Quality,
		Generator: gen,
	}, nil
}

// ResolvedKnowledgeBase is the knowledge_base section with defaults
// applied; Timeout is the parsed request timeout. Enabled is false when
// the section is absent or explicitly disabled.
type ResolvedKnowledgeBase struct {
	Enabled           bool
	BaseURL           string
	APIKey            string
	Timeout           time.Duration
	DefaultTopK       int
	DefaultCollection string
	Collections       []ResolvedKBCollection
}

// ResolvedKBCollection is one searchable collection.
type ResolvedKBCollection struct {
	Name        string
	Description string
}

const (
	defaultKBTimeoutMs = 10000
	minKBTimeoutMs     = 1000
	maxKBTimeoutMs     = 60000
	defaultKBTopK      = 5
	maxKBTopK          = 20
)

// resolveKnowledgeBase validates the knowledge_base section and applies
// defaults. The section is opt-in (like image/browser): absent or disabled
// yields a disabled result; an explicit enabled: true requires base_url
// and at least one collection.
func resolveKnowledgeBase(in KnowledgeBase) (ResolvedKnowledgeBase, error) {
	if in.Enabled != nil && !*in.Enabled {
		return ResolvedKnowledgeBase{}, nil
	}
	baseURL := strings.TrimSpace(in.BaseURL)
	if baseURL == "" {
		if in.Enabled != nil {
			return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.base_url is required when enabled")
		}
		if in.APIKey != "" || in.TimeoutMs != 0 || in.DefaultTopK != 0 || in.DefaultCollection != "" || len(in.Collections) > 0 {
			return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base: base_url is required to enable the section")
		}
		return ResolvedKnowledgeBase{}, nil
	}
	u, err := url.Parse(baseURL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.base_url: must be a valid http(s) URL, got %q", baseURL)
	}
	timeoutMs := in.TimeoutMs
	if timeoutMs == 0 {
		timeoutMs = defaultKBTimeoutMs
	}
	if timeoutMs < minKBTimeoutMs || timeoutMs > maxKBTimeoutMs {
		return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.timeout_ms must be between %d and %d", minKBTimeoutMs, maxKBTimeoutMs)
	}
	topK := in.DefaultTopK
	if topK == 0 {
		topK = defaultKBTopK
	}
	if topK < 1 || topK > maxKBTopK {
		return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.default_top_k must be between 1 and %d", maxKBTopK)
	}
	if len(in.Collections) == 0 {
		return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.collections: at least one collection is required")
	}
	seen := make(map[string]struct{}, len(in.Collections))
	cols := make([]ResolvedKBCollection, 0, len(in.Collections))
	for i, c := range in.Collections {
		name := strings.TrimSpace(c.Name)
		if name == "" {
			return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.collections[%d].name: must not be empty", i)
		}
		if _, dup := seen[name]; dup {
			return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.collections[%d]: duplicate collection %q", i, name)
		}
		seen[name] = struct{}{}
		cols = append(cols, ResolvedKBCollection{Name: name, Description: strings.TrimSpace(c.Description)})
	}
	defaultCol := strings.TrimSpace(in.DefaultCollection)
	if defaultCol == "" {
		defaultCol = cols[0].Name
	} else if _, ok := seen[defaultCol]; !ok {
		return ResolvedKnowledgeBase{}, fmt.Errorf("config: knowledge_base.default_collection: %q is not in collections", defaultCol)
	}
	return ResolvedKnowledgeBase{
		Enabled:           true,
		BaseURL:           baseURL,
		APIKey:            in.APIKey,
		Timeout:           time.Duration(timeoutMs) * time.Millisecond,
		DefaultTopK:       topK,
		DefaultCollection: defaultCol,
		Collections:       cols,
	}, nil
}

// resolveSecret implements the inline-or-env-reference rule for a secret
// field pair ("api_key"/"api_key_env", tracing keys, ...).
func resolveSecret(ctx, field, inline, envName string, lookup EnvLookup) (string, error) {
	if inline != "" && envName != "" {
		return "", fmt.Errorf("config: %s: %s and %s_env are mutually exclusive", ctx, field, field)
	}
	if envName != "" {
		value, ok := lookup(envName)
		if !ok || value == "" {
			return "", fmt.Errorf("config: %s: %s_env references %s, which is not set", ctx, field, envName)
		}
		return value, nil
	}
	return inline, nil
}

// buildProvider dispatches provider assembly on the protocol family.
func buildProvider(pType string, p Provider, apiKey string, wireAPI openai.WireAPI, retries int) (domain.Model, error) {
	switch pType {
	case "anthropic":
		return anthropic.New(anthropic.Config{
			BaseURL:    p.BaseURL,
			APIKey:     apiKey,
			AuthType:   anthropic.AuthType(p.AuthType),
			Version:    p.APIVersion,
			MaxRetries: retries,
		})
	default:
		return openai.New(openai.Config{
			BaseURL:    p.BaseURL,
			APIKey:     apiKey,
			WireAPI:    wireAPI,
			MaxRetries: retries,
		})
	}
}

// resolveProviderWireAPI validates a wire_api value in the vocabulary of
// the provider's protocol family and returns the openai wire constant (for
// OpenAI-compatible providers) plus the user-facing short name that travels
// into resolved model metadata; empty defaults to the family's natural
// choice ("chat" / "messages").
func resolveProviderWireAPI(ctx, pType, raw string) (openai.WireAPI, string, error) {
	trimmed := strings.TrimSpace(raw)
	if pType == "anthropic" {
		switch trimmed {
		case "", "messages":
			return "", "messages", nil
		default:
			return "", "", fmt.Errorf("config: %s: wire_api must be \"messages\" for an anthropic provider, got %q", ctx, raw)
		}
	}
	switch trimmed {
	case "", "chat":
		return openai.WireAPIChatCompletions, "chat", nil
	case "responses":
		return openai.WireAPIResponses, "responses", nil
	default:
		return "", "", fmt.Errorf("config: %s: wire_api must be \"chat\" or \"responses\", got %q", ctx, raw)
	}
}

// resolveAuthType validates the credential-header selection. Only the
// anthropic protocol family has more than one convention; for everything
// else the header is fixed (Bearer) and the key must stay unset.
func resolveAuthType(ctx, pType, raw string) error {
	trimmed := strings.TrimSpace(raw)
	if pType != "anthropic" {
		if trimmed != "" {
			return fmt.Errorf("config: %s: auth_type is only meaningful for anthropic providers", ctx)
		}
		return nil
	}
	switch trimmed {
	case "", "x-api-key", "bearer":
		return nil
	default:
		return fmt.Errorf("config: %s: auth_type must be \"x-api-key\" or \"bearer\", got %q", ctx, raw)
	}
}

// resolveReasoning validates a reasoning section (provider or model level).
func resolveReasoning(ctx string, r Reasoning) error {
	switch strings.TrimSpace(r.Effort) {
	case "", "off", "low", "medium", "high":
	default:
		return fmt.Errorf("config: %s: reasoning.effort must be \"off\", \"low\", \"medium\", or \"high\", got %q", ctx, r.Effort)
	}
	if r.BudgetTokens < 0 {
		return fmt.Errorf("config: %s: reasoning.budget_tokens must be >= 0", ctx)
	}
	return nil
}

// resolveLimits overlays the file's limits onto the built-in defaults.
func resolveLimits(in Limits) (domain.Limits, error) {
	out := domain.DefaultLimits()
	if in.MaxInputTokens != nil {
		out.MaxInputTokens = *in.MaxInputTokens
	}
	if in.MaxOutputTokens != nil {
		out.MaxOutputTokens = *in.MaxOutputTokens
	}
	if in.MaxCostUSD != nil {
		out.MaxEstimatedCostUSD = *in.MaxCostUSD
	}
	if in.MaxTokens != nil {
		out.MaxTokens = *in.MaxTokens
	}
	if in.MaxToolOutputBytes != nil {
		out.MaxToolOutputBytes = *in.MaxToolOutputBytes
	}
	if in.MaxArtifactBytes != nil {
		out.MaxArtifactBytes = *in.MaxArtifactBytes
	}
	// Negative values would silently disable a budget dimension or, worse,
	// make comparisons meaningless — reject them all in one place.
	negatives := map[string]bool{
		"max_input_tokens":      out.MaxInputTokens < 0,
		"max_output_tokens":     out.MaxOutputTokens < 0,
		"max_cost_usd":          out.MaxEstimatedCostUSD < 0,
		"max_tokens":            out.MaxTokens < 0,
		"max_tool_output_bytes": out.MaxToolOutputBytes < 0,
		"max_artifact_bytes":    out.MaxArtifactBytes < 0,
	}
	for field, negative := range negatives {
		if negative {
			return domain.Limits{}, fmt.Errorf("config: limits.%s must be >= 0", field)
		}
	}
	return out, nil
}

// resolveContext overlays the file's context section onto the built-in
// defaults and enforces the ordering invariants (fail-fast at startup).
func resolveContext(in Context) (domain.ContextConfig, error) {
	out := domain.DefaultContextConfig()
	if in.Utilization != nil {
		out.Utilization = *in.Utilization
	}
	if in.CompactTriggerRatio != nil {
		out.CompactTriggerRatio = *in.CompactTriggerRatio
	}
	if in.CompactTargetRatio != nil {
		out.CompactTargetRatio = *in.CompactTargetRatio
	}
	if in.NoticeLevels != nil {
		out.NoticeLevels = append([]float64(nil), in.NoticeLevels...)
	}
	if err := out.Validate(); err != nil {
		return domain.ContextConfig{}, fmt.Errorf("config: %w", err)
	}
	return out, nil
}

// resolveMemory overlays the file's memory section onto the built-in
// defaults. Model references resolve against the already-validated
// providers (mirroring subagent.model); the pipeline scheduling knobs use
// Go duration syntax.
func resolveMemory(in Memory, out *ResolvedConfig) (ResolvedMemory, error) {
	resolved := ResolvedMemory{
		Enabled:        in.Enabled == nil || *in.Enabled,
		MaxJobsPerRun:  8,
		RunInterval:    30 * time.Minute,
		MinSessionIdle: time.Hour,
		MaxSessionAge:  30 * 24 * time.Hour,
	}
	if m := strings.TrimSpace(in.ExtractModel); m != "" {
		ref, err := out.ResolveRef(m)
		if err != nil {
			return ResolvedMemory{}, fmt.Errorf("config: memory.extract_model: %w", err)
		}
		resolved.ExtractModel = &ref
	}
	if m := strings.TrimSpace(in.ConsolidationModel); m != "" {
		ref, err := out.ResolveRef(m)
		if err != nil {
			return ResolvedMemory{}, fmt.Errorf("config: memory.consolidation_model: %w", err)
		}
		resolved.ConsolidationModel = &ref
	}
	if in.MaxJobsPerRun != nil {
		if *in.MaxJobsPerRun < 1 || *in.MaxJobsPerRun > 128 {
			return ResolvedMemory{}, fmt.Errorf("config: memory.max_jobs_per_run must be between 1 and 128")
		}
		resolved.MaxJobsPerRun = *in.MaxJobsPerRun
	}
	for _, d := range []struct {
		name  string
		raw   string
		field *time.Duration
	}{
		{"memory.run_interval", in.RunInterval, &resolved.RunInterval},
		{"memory.min_session_idle", in.MinSessionIdle, &resolved.MinSessionIdle},
		{"memory.max_session_age", in.MaxSessionAge, &resolved.MaxSessionAge},
	} {
		if d.raw == "" {
			continue
		}
		v, err := time.ParseDuration(d.raw)
		if err != nil {
			return ResolvedMemory{}, fmt.Errorf("config: %s: expected a Go duration (e.g. \"30m\"), got %q", d.name, d.raw)
		}
		if v < 0 {
			return ResolvedMemory{}, fmt.Errorf("config: %s must be >= 0", d.name)
		}
		*d.field = v
	}
	return resolved, nil
}

// ResolvedSessions is the sessions section with defaults applied. It must
// stay comparable (scalar fields only): reload classification diffs it
// with !=.
type ResolvedSessions struct {
	// AutoArchiveAfter archives sessions idle (no appended events) for
	// longer than this; 0 disables the background archiver.
	AutoArchiveAfter time.Duration
	// GCArchivedAfter permanently deletes sessions archived for longer than
	// this; 0 disables the purge (archived sessions are kept forever).
	GCArchivedAfter time.Duration
}

// resolveSessions validates the sessions section. Both knobs are opt-in:
// absent/empty/"0" leaves the durations at 0 (disabled).
func resolveSessions(in Sessions) (ResolvedSessions, error) {
	var out ResolvedSessions
	for _, d := range []struct {
		name  string
		raw   string
		field *time.Duration
	}{
		{"sessions.auto_archive_after", in.AutoArchiveAfter, &out.AutoArchiveAfter},
		{"sessions.gc_archived_after", in.GCArchivedAfter, &out.GCArchivedAfter},
	} {
		raw := strings.TrimSpace(d.raw)
		if raw == "" {
			continue
		}
		v, err := time.ParseDuration(raw)
		if err != nil {
			return ResolvedSessions{}, fmt.Errorf("config: %s: expected a Go duration (e.g. \"720h\"), got %q", d.name, d.raw)
		}
		if v < 0 {
			return ResolvedSessions{}, fmt.Errorf("config: %s must be >= 0", d.name)
		}
		*d.field = v
	}
	return out, nil
}

// resolveRunaway overlays the file's runaway section onto the built-in
// defaults.
func resolveRunaway(in Runaway) (domain.RunawayConfig, error) {
	out := domain.DefaultRunawayConfig()
	if in.MaxRepeatedCalls != nil {
		out.MaxRepeatedCalls = *in.MaxRepeatedCalls
	}
	if in.MaxConsecutiveFailures != nil {
		out.MaxConsecutiveFailures = *in.MaxConsecutiveFailures
	}
	if in.StallWarnTurns != nil {
		out.StallWarnTurns = *in.StallWarnTurns
	}
	if in.StallTimeout != "" {
		d, err := time.ParseDuration(in.StallTimeout)
		if err != nil {
			return domain.RunawayConfig{}, fmt.Errorf("config: runaway.stall_timeout: expected a Go duration (e.g. \"15m\"), got %q", in.StallTimeout)
		}
		out.StallTimeout = d
	}
	if err := out.Validate(); err != nil {
		return domain.RunawayConfig{}, fmt.Errorf("config: %w", err)
	}
	return out, nil
}

// LoadOptions maps the resolved rules section onto the permission layer's
// load switches (shared by the TUI bootstrap and the headless entry).
func (r ResolvedRules) LoadOptions() permission.RuleLoadOptions {
	return permission.RuleLoadOptions{
		Enabled:      r.Enabled,
		Builtin:      r.Builtin,
		Project:      r.Project,
		ProjectAllow: r.ProjectAllow,
	}
}

// resolveRules applies the rule-layer defaults: every layer loads, and
// project rules may only tighten policy, never loosen it.
func resolveRules(in Rules) ResolvedRules {
	return ResolvedRules{
		Enabled:           in.Enabled == nil || *in.Enabled,
		Builtin:           in.Builtin == nil || *in.Builtin,
		Project:           in.Project == nil || *in.Project,
		ProjectAllow:      in.ProjectAllow != nil && *in.ProjectAllow,
		PersistRemembered: in.PersistRemembered == nil || *in.PersistRemembered,
	}
}

// resolveLogging converts the MiB quotas to bytes, keeping the built-in
// defaults for absent fields and passing negatives through (unlimited).
func resolveLogging(in Logging) ResolvedLogging {
	out := ResolvedLogging{
		MaxFileBytes:  logging.DefaultMaxFileBytes,
		MaxTotalBytes: logging.DefaultMaxTotalBytes,
	}
	if in.MaxFileMB != 0 {
		out.MaxFileBytes = int64(in.MaxFileMB) << 20
	}
	if in.MaxTotalMB != 0 {
		out.MaxTotalBytes = int64(in.MaxTotalMB) << 20
	}
	return out
}

// resolveTracing builds trace.Config; tracing is enabled only when host
// and both keys are present.
func resolveTracing(in Tracing, lookup EnvLookup) (trace.Config, error) {
	publicKey, err := resolveSecret("tracing", "public_key", in.PublicKey, in.PublicKeyEnv, lookup)
	if err != nil {
		return trace.Config{}, err
	}
	secretKey, err := resolveSecret("tracing", "secret_key", in.SecretKey, in.SecretKeyEnv, lookup)
	if err != nil {
		return trace.Config{}, err
	}
	if in.CostInputPerMTok < 0 || in.CostOutputPerMTok < 0 {
		return trace.Config{}, fmt.Errorf("config: tracing cost rates must be >= 0")
	}
	env := in.Environment
	if env == "" {
		env = "dev"
	}
	return trace.Config{
		Host:              strings.TrimRight(in.Host, "/"),
		PublicKey:         publicKey,
		SecretKey:         secretKey,
		Environment:       env,
		IncludeContent:    in.IncludeContent == nil || *in.IncludeContent,
		UserID:            in.User, // empty → trace.Setup derives git email / $USER
		CostInputPerMTok:  in.CostInputPerMTok,
		CostOutputPerMTok: in.CostOutputPerMTok,
		Enabled:           in.Host != "" && publicKey != "" && secretKey != "",
	}, nil
}

// ResolvedMCP holds the validated MCP server configuration. The map is
// passed directly to mcp.StartServers in bootstrap.go; the resolve step
// only validates config-level constraints (required fields, timeout
// bounds) — process spawning and tool discovery are deferred to runtime.
type ResolvedMCP struct {
	Servers map[string]MCPServer
}

// headerEnvRef matches a ${VAR} environment reference in an MCP header
// value; refs resolve secrets at load time, mirroring api_key_env.
var headerEnvRef = regexp.MustCompile(`\$\{([A-Za-z_][A-Za-z0-9_]*)\}`)

// resolveMCP validates the MCP server entries: each server selects its
// transport with exactly one of command (stdio) or url (streamable
// HTTP), transport-specific fields must not cross over, header ${VAR}
// references must resolve, and timeout overrides must be non-negative.
func resolveMCP(in map[string]MCPServer, lookup EnvLookup) (ResolvedMCP, error) {
	if len(in) == 0 {
		return ResolvedMCP{}, nil
	}
	out := make(map[string]MCPServer, len(in))
	for name, srv := range in {
		ctx := fmt.Sprintf("mcp_servers.%s", name)
		if strings.TrimSpace(name) == "" {
			return ResolvedMCP{}, fmt.Errorf("config: mcp_servers: server name must not be empty")
		}
		// The name travels into tool names (mcp__{name}__{tool}) and the
		// reconnect endpoint's path segment — keep it URL- and
		// identifier-safe.
		if strings.ContainsAny(name, "/ \\?#%") {
			return ResolvedMCP{}, fmt.Errorf("config: %s: server name must not contain slashes, spaces, or URL-reserved characters", ctx)
		}
		hasCommand := strings.TrimSpace(srv.Command) != ""
		hasURL := strings.TrimSpace(srv.URL) != ""
		switch {
		case !hasCommand && !hasURL:
			return ResolvedMCP{}, fmt.Errorf("config: %s: command or url is required", ctx)
		case hasCommand && hasURL:
			return ResolvedMCP{}, fmt.Errorf("config: %s: command and url are mutually exclusive", ctx)
		}
		if hasURL {
			u, err := url.Parse(srv.URL)
			if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
				return ResolvedMCP{}, fmt.Errorf("config: %s: url must be a valid http(s) URL", ctx)
			}
			if len(srv.Args) > 0 || len(srv.Env) > 0 || srv.Cwd != "" {
				return ResolvedMCP{}, fmt.Errorf("config: %s: args/env/cwd only apply to command (stdio) servers", ctx)
			}
			headers, err := expandHeaderRefs(ctx, srv.Headers, lookup)
			if err != nil {
				return ResolvedMCP{}, err
			}
			srv.Headers = headers
		} else if len(srv.Headers) > 0 {
			return ResolvedMCP{}, fmt.Errorf("config: %s: headers only apply to url (streamable HTTP) servers", ctx)
		}
		if srv.StartupTimeoutSec < 0 {
			return ResolvedMCP{}, fmt.Errorf("config: %s: startup_timeout_sec must be >= 0", ctx)
		}
		if srv.ToolTimeoutSec < 0 {
			return ResolvedMCP{}, fmt.Errorf("config: %s: tool_timeout_sec must be >= 0", ctx)
		}
		out[name] = srv
	}
	return ResolvedMCP{Servers: out}, nil
}

// expandHeaderRefs resolves ${VAR} environment references in header
// values; an unset variable is a load-time error, like api_key_env.
func expandHeaderRefs(ctx string, headers map[string]string, lookup EnvLookup) (map[string]string, error) {
	if len(headers) == 0 {
		return nil, nil
	}
	keys := make([]string, 0, len(headers))
	for k := range headers {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	out := make(map[string]string, len(headers))
	for _, k := range keys {
		missing := ""
		value := headerEnvRef.ReplaceAllStringFunc(headers[k], func(ref string) string {
			name := headerEnvRef.FindStringSubmatch(ref)[1]
			resolved, ok := lookup(name)
			if !ok {
				missing = name
				return ""
			}
			return resolved
		})
		if missing != "" {
			return nil, fmt.Errorf("config: %s: header %q references environment variable %s, which is not set", ctx, k, missing)
		}
		out[k] = value
	}
	return out, nil
}
