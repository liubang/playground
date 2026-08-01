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

package websearch

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	providerBrave      = "brave"
	providerTavily     = "tavily"
	providerDuckDuckGo = "duckduckgo"

	braveEndpoint      = "https://api.search.brave.com/res/v1/web/search"
	tavilyEndpoint     = "https://api.tavily.com/search"
	duckDuckGoEndpoint = "https://html.duckduckgo.com/html/"

	maxResponseBytes = 1 << 20
	maxSnippetBytes  = 512
)

// searchResult is one normalized hit independent of the provider wire format.
type searchResult struct {
	Title   string `json:"title"`
	URL     string `json:"url"`
	Snippet string `json:"snippet"`
}

// searchProvider executes a query against one search backend.
type searchProvider interface {
	Name() string
	Search(ctx context.Context, client *http.Client, query string, count int) ([]searchResult, error)
}

// selectProvider picks the search backend: the explicit
// LOOM_WEB_SEARCH_PROVIDER override wins, otherwise the first configured
// API key (brave, then tavily), otherwise the keyless DuckDuckGo HTML
// endpoint. Returns an error when the override names a provider whose
// required key is missing.
func selectProvider(getenv func(string) string) (searchProvider, error) {
	if getenv == nil {
		getenv = os.Getenv
	}
	override := strings.ToLower(strings.TrimSpace(getenv("LOOM_WEB_SEARCH_PROVIDER")))
	if override != "" {
		switch override {
		case providerBrave:
			key := strings.TrimSpace(getenv("BRAVE_SEARCH_API_KEY"))
			if key == "" {
				return nil, domain.NewError(domain.ErrInvalidInput, "LOOM_WEB_SEARCH_PROVIDER=brave requires BRAVE_SEARCH_API_KEY")
			}
			return &braveProvider{endpoint: braveEndpoint, apiKey: key}, nil
		case providerTavily:
			key := strings.TrimSpace(getenv("TAVILY_API_KEY"))
			if key == "" {
				return nil, domain.NewError(domain.ErrInvalidInput, "LOOM_WEB_SEARCH_PROVIDER=tavily requires TAVILY_API_KEY")
			}
			return &tavilyProvider{endpoint: tavilyEndpoint, apiKey: key}, nil
		case providerDuckDuckGo, "ddg":
			return &duckDuckGoProvider{endpoint: duckDuckGoEndpoint}, nil
		default:
			return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("unsupported LOOM_WEB_SEARCH_PROVIDER %q (want brave, tavily, or duckduckgo)", override))
		}
	}
	if key := strings.TrimSpace(getenv("BRAVE_SEARCH_API_KEY")); key != "" {
		return &braveProvider{endpoint: braveEndpoint, apiKey: key}, nil
	}
	if key := strings.TrimSpace(getenv("TAVILY_API_KEY")); key != "" {
		return &tavilyProvider{endpoint: tavilyEndpoint, apiKey: key}, nil
	}
	return &duckDuckGoProvider{endpoint: duckDuckGoEndpoint}, nil
}

// readResponseBody bounds and validates a provider HTTP response.
func readResponseBody(resp *http.Response) ([]byte, error) {
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusTooManyRequests {
		return nil, domain.NewError(domain.ErrRateLimited, fmt.Sprintf("search provider returned status %d", resp.StatusCode), domain.WithRetryable(true))
	}
	if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
		return nil, domain.NewError(domain.ErrSecurity, fmt.Sprintf("search provider rejected the API key (status %d)", resp.StatusCode))
	}
	if resp.StatusCode >= 400 {
		return nil, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("search provider returned status %d", resp.StatusCode), domain.WithRetryable(resp.StatusCode >= 500))
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes+1))
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to read search response", domain.WithCause(err), domain.WithRetryable(true))
	}
	if len(data) > maxResponseBytes {
		data = data[:maxResponseBytes]
	}
	return data, nil
}

func clipSnippet(s string) string {
	s = strings.Join(strings.Fields(s), " ")
	if len(s) > maxSnippetBytes {
		s = s[:maxSnippetBytes]
	}
	return s
}

// --- Brave Search API ---

type braveProvider struct {
	endpoint string
	apiKey   string
}

func (p *braveProvider) Name() string { return providerBrave }

func (p *braveProvider) Search(ctx context.Context, client *http.Client, query string, count int) ([]searchResult, error) {
	u, err := url.Parse(p.endpoint)
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "invalid brave endpoint", domain.WithCause(err))
	}
	q := u.Query()
	q.Set("q", query)
	q.Set("count", fmt.Sprintf("%d", count))
	q.Set("text_decorations", "false")
	u.RawQuery = q.Encode()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to build brave request", domain.WithCause(err))
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Subscription-Token", p.apiKey)

	resp, err := client.Do(req)
	if err != nil {
		return nil, mapRequestError(err)
	}
	data, err := readResponseBody(resp)
	if err != nil {
		return nil, err
	}

	var payload struct {
		Web struct {
			Results []struct {
				Title       string `json:"title"`
				URL         string `json:"url"`
				Description string `json:"description"`
			} `json:"results"`
		} `json:"web"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to parse brave response", domain.WithCause(err))
	}
	out := make([]searchResult, 0, len(payload.Web.Results))
	for _, r := range payload.Web.Results {
		if r.URL == "" {
			continue
		}
		out = append(out, searchResult{Title: strings.TrimSpace(r.Title), URL: r.URL, Snippet: clipSnippet(r.Description)})
	}
	return out, nil
}

// --- Tavily Search API ---

type tavilyProvider struct {
	endpoint string
	apiKey   string
}

func (p *tavilyProvider) Name() string { return providerTavily }

func (p *tavilyProvider) Search(ctx context.Context, client *http.Client, query string, count int) ([]searchResult, error) {
	body, err := json.Marshal(map[string]any{
		"api_key":     p.apiKey,
		"query":       query,
		"max_results": count,
	})
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to encode tavily request", domain.WithCause(err))
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.endpoint, strings.NewReader(string(body)))
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to build tavily request", domain.WithCause(err))
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")

	resp, err := client.Do(req)
	if err != nil {
		return nil, mapRequestError(err)
	}
	data, err := readResponseBody(resp)
	if err != nil {
		return nil, err
	}

	var payload struct {
		Results []struct {
			Title   string `json:"title"`
			URL     string `json:"url"`
			Content string `json:"content"`
		} `json:"results"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "failed to parse tavily response", domain.WithCause(err))
	}
	out := make([]searchResult, 0, len(payload.Results))
	for _, r := range payload.Results {
		if r.URL == "" {
			continue
		}
		out = append(out, searchResult{Title: strings.TrimSpace(r.Title), URL: r.URL, Snippet: clipSnippet(r.Content)})
	}
	return out, nil
}

// --- DuckDuckGo HTML endpoint (keyless fallback) ---

type duckDuckGoProvider struct {
	endpoint string
}

func (p *duckDuckGoProvider) Name() string { return providerDuckDuckGo }

func (p *duckDuckGoProvider) Search(ctx context.Context, client *http.Client, query string, count int) ([]searchResult, error) {
	form := url.Values{"q": {query}}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.endpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to build duckduckgo request", domain.WithCause(err))
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Accept", "text/html")
	req.Header.Set("User-Agent", userAgent)

	resp, err := client.Do(req)
	if err != nil {
		return nil, mapRequestError(err)
	}
	data, err := readResponseBody(resp)
	if err != nil {
		return nil, err
	}
	results := parseDuckDuckGoHTML(data, count)
	if len(results) == 0 {
		return nil, domain.NewError(domain.ErrUnavailable, "no results parsed from duckduckgo response (markup may have changed)")
	}
	for i := range results {
		results[i].Snippet = clipSnippet(results[i].Snippet)
	}
	return results, nil
}

func mapRequestError(err error) error {
	switch {
	case strings.Contains(err.Error(), "deadline") || strings.Contains(err.Error(), "timeout"):
		return domain.NewError(domain.ErrTimeout, "search request timed out", domain.WithRetryable(true), domain.WithCause(err))
	default:
		return domain.NewError(domain.ErrUnavailable, "search request failed", domain.WithCause(err), domain.WithRetryable(true))
	}
}
