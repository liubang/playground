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
	"net/url"
	"sort"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/anthropic"
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
	Providers []ResolvedProvider
	Default   ProviderModelRef
	Limits    domain.Limits
	Context   domain.ContextConfig
	Runaway   domain.RunawayConfig
	Prompt    Prompt
	Skills    ResolvedSkills
	Rules     ResolvedRules
	Approval  ResolvedApproval
	Tracing   trace.Config
	Storage   Storage
	UI        UI
	Subagent  ResolvedSubagent
}

// ResolvedSubagent is the subagent section with defaults applied
// (docs/SUBAGENT_DESIGN.md §7).
type ResolvedSubagent struct {
	Enabled bool
	// MaxTokens caps the child run's cumulative metered tokens; 0 inherits
	// limits.max_tokens.
	MaxTokens int64
	// Model pins the sub-agent's model selection; nil follows the current
	// turn's model.
	Model *ProviderModelRef
}

// ResolvedSkills is the skills section with defaults applied.
type ResolvedSkills struct {
	Enabled    bool
	ExtraRoots []string
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
// (docs/CONFIG_DESIGN.md §7).
func resolve(f *File, lookup EnvLookup) (*ResolvedConfig, error) {
	if lookup == nil {
		return nil, fmt.Errorf("config: env lookup is required")
	}
	providers, err := resolveProviders(f.Providers, lookup)
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
	approval := ResolvedApproval{Mode: mode}
	out := &ResolvedConfig{
		Providers: providers,
		Limits:    limits,
		Context:   contextCfg,
		Runaway:   runawayCfg,
		Prompt:    f.Prompt,
		Skills: ResolvedSkills{
			Enabled:    f.Skills.Enabled == nil || *f.Skills.Enabled,
			ExtraRoots: f.Skills.ExtraRoots,
		},
		Rules:    resolveRules(f.Rules),
		Approval: approval,
		Tracing:  tracing,
		Storage:  f.Storage,
		UI:       f.UI,
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
		Enabled: f.Subagent.Enabled == nil || *f.Subagent.Enabled,
	}
	if f.Subagent.MaxTokens != nil {
		if *f.Subagent.MaxTokens < 0 {
			return nil, fmt.Errorf("config: subagent.max_tokens must be >= 0")
		}
		sub.MaxTokens = *f.Subagent.MaxTokens
	}
	if m := strings.TrimSpace(f.Subagent.Model); m != "" {
		ref, err := out.ResolveRef(m)
		if err != nil {
			return nil, fmt.Errorf("config: subagent.model: %w", err)
		}
		sub.Model = &ref
	}
	out.Subagent = sub
	return out, nil
}

// resolveProviders validates uniqueness and required fields, resolves each
// API key, and prebuilds one domain.Model per provider.
func resolveProviders(in []Provider, lookup EnvLookup) ([]ResolvedProvider, error) {
	seen := make(map[string]bool, len(in))
	out := make([]ResolvedProvider, 0, len(in))
	for i := range in {
		p := in[i]
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
