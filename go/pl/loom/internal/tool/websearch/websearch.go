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
// Created: 2026/08/01

// Package websearch implements the web_search tool: pluggable search
// backends (Brave/Tavily via API key, keyless DuckDuckGo by default) behind
// the same SSRF dial guard, result caching, and prepared-call signing the
// rest of the built-in tools use. Pair web_search (discovery) with
// web_fetch (reading) for full web access.
package websearch

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/tool/webfetch"
)

const (
	defaultCount      = 5
	maxCount          = 10
	maxQueryBytes     = 1024
	defaultTimeoutMs  = 15000
	maxTimeoutMs      = 30000
	dialTimeout       = 10 * time.Second
	userAgent         = "loom-websearch/0.1 (+https://github.com/liubang/playground)"
	cacheTTL          = 5 * time.Minute
	cacheMaxEntries   = 64
	cacheMaxBodyBytes = 64 << 10
)

type searchArgs struct {
	Query     string `json:"query"`
	Count     int    `json:"count,omitempty"`
	TimeoutMs int    `json:"timeout_ms,omitempty"`
}

type searchOutput struct {
	Query     string         `json:"query"`
	Provider  string         `json:"provider"`
	Count     int            `json:"count"`
	Results   []searchResult `json:"results"`
	Cache     string         `json:"cache"`
	FetchedAt time.Time      `json:"fetched_at"`
}

// WebSearchTool implements web_search.
type WebSearchTool struct {
	base     baseTool
	provider searchProvider
	cache    *responseCache
	resolver *net.Resolver
	now      func() time.Time
	// allowPrivate relaxes the SSRF dial guard; tests set it to reach
	// loopback httptest servers. Never set from configuration.
	allowPrivate bool
}

// NewWebSearchTool creates the web_search tool, selecting the backend from
// the environment (LOOM_WEB_SEARCH_PROVIDER, else the first configured API
// key, else keyless DuckDuckGo).
func NewWebSearchTool() (*WebSearchTool, error) {
	provider, err := selectProvider(nil)
	if err != nil {
		return nil, err
	}
	return newWebSearchTool(provider, nil)
}

func newWebSearchTool(provider searchProvider, now func() time.Time) (*WebSearchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "web_search",
		Description: "Search the web and return ranked results (title, url, snippet). " +
			"Use it to discover sources for current or temporally unstable information (news, prices, library " +
			"versions, current docs) and then call web_fetch on the most relevant URLs to read them. " +
			"The backend is selected by configuration: Brave when BRAVE_SEARCH_API_KEY is set, Tavily when " +
			"TAVILY_API_KEY is set, otherwise a keyless DuckDuckGo endpoint; the active one is reported in " +
			"the provider field. Results are cached for 5 minutes.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"query":{"type":"string","minLength":1,"maxLength":1024},"count":{"type":"integer","minimum":1,"maximum":10},"timeout_ms":{"type":"integer","minimum":1000,"maximum":30000}},"required":["query"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"query":{"type":"string"},"provider":{"type":"string"},"count":{"type":"integer"},"results":{"type":"array","items":{"type":"object","properties":{"title":{"type":"string"},"url":{"type":"string"},"snippet":{"type":"string"}},"required":["title","url","snippet"]}},"cache":{"type":"string"},"fetched_at":{"type":"string"}},"required":["query","provider","count","results","cache","fetched_at"]}`),
		Capabilities: []domain.Capability{domain.CapNetworkConnect},
		Source:       domain.ToolSourceBuiltin,
	})
	if err != nil {
		return nil, err
	}
	if now == nil {
		now = time.Now
	}
	return &WebSearchTool{
		base:     base,
		provider: provider,
		cache:    newResponseCache(cacheMaxEntries, cacheMaxBodyBytes, cacheTTL, now),
		resolver: net.DefaultResolver,
		now:      now,
	}, nil
}

func (t *WebSearchTool) Definition() domain.ToolDefinition {
	return t.base.def
}

// ConcurrentSafe implements domain.ConcurrentSafely: searches are
// independent and the result cache is mutex-protected.
func (t *WebSearchTool) ConcurrentSafe() bool { return true }

func (t *WebSearchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[searchArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, err = validateSearchArgs(args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Search the web for %q (%s)", args.Query, t.provider.Name())
	return t.base.prepareCall(ctx, call, canonical, approvalDesc)
}

func (t *WebSearchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := decodeStrict[searchArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	cacheKey := t.provider.Name() + "\x1f" + args.Query + "\x1f" + fmt.Sprintf("%d", args.Count)
	if entry, ok := t.cache.get(cacheKey); ok {
		var results []searchResult
		if err := json.Unmarshal([]byte(entry.Body), &results); err == nil {
			return successResult(prepared.Call.ID, startedAt, t.buildOutput(args, results, "hit", entry.FetchedAt))
		}
	}

	results, err := t.search(ctx, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if body, err := json.Marshal(results); err == nil && len(body) <= cacheMaxBodyBytes {
		t.cache.put(cacheKey, cachedResponse{FetchedAt: t.now().UTC(), Body: string(body)})
	}
	return successResult(prepared.Call.ID, startedAt, t.buildOutput(args, results, "miss", t.now().UTC()))
}

func validateSearchArgs(args searchArgs) (searchArgs, error) {
	if args.Count == 0 {
		args.Count = defaultCount
	}
	if args.Count < 1 || args.Count > maxCount {
		return searchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("count must be between 1 and %d", maxCount))
	}
	if args.TimeoutMs == 0 {
		args.TimeoutMs = defaultTimeoutMs
	}
	if args.TimeoutMs < 1000 || args.TimeoutMs > maxTimeoutMs {
		return searchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("timeout_ms must be between 1000 and %d", maxTimeoutMs))
	}
	if len(args.Query) == 0 || len(args.Query) > maxQueryBytes {
		return searchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("query must be 1..%d bytes", maxQueryBytes))
	}
	return args, nil
}

// search issues the provider request behind the shared SSRF dial guard:
// provider endpoints are fixed public hosts, and the guard keeps DNS
// answers honest (a hostile resolver cannot redirect the API key to a
// private address).
func (t *WebSearchTool) search(ctx context.Context, args searchArgs) ([]searchResult, error) {
	transport := &http.Transport{
		DialContext:         webfetch.GuardedDialFunc(t.resolver, t.allowPrivate),
		TLSHandshakeTimeout: dialTimeout,
		ForceAttemptHTTP2:   true,
	}
	defer transport.CloseIdleConnections()
	client := &http.Client{
		Transport: transport,
		Timeout:   time.Duration(args.TimeoutMs) * time.Millisecond,
	}

	reqCtx, cancel := context.WithTimeout(ctx, time.Duration(args.TimeoutMs)*time.Millisecond)
	defer cancel()
	return t.provider.Search(reqCtx, client, args.Query, args.Count)
}

func (t *WebSearchTool) buildOutput(args searchArgs, results []searchResult, cacheState string, fetchedAt time.Time) searchOutput {
	if results == nil {
		results = []searchResult{}
	}
	if len(results) > args.Count {
		results = results[:args.Count]
	}
	return searchOutput{
		Query:     args.Query,
		Provider:  t.provider.Name(),
		Count:     len(results),
		Results:   results,
		Cache:     cacheState,
		FetchedAt: fetchedAt,
	}
}
