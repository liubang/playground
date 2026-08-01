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
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func getenvFrom(env map[string]string) func(string) string {
	return func(key string) string { return env[key] }
}

func TestSelectProvider(t *testing.T) {
	tests := []struct {
		name     string
		env      map[string]string
		wantName string
		wantErr  bool
	}{
		{"default duckduckgo", map[string]string{}, providerDuckDuckGo, false},
		{"brave key wins", map[string]string{"BRAVE_SEARCH_API_KEY": "k", "TAVILY_API_KEY": "k2"}, providerBrave, false},
		{"tavily key", map[string]string{"TAVILY_API_KEY": "k2"}, providerTavily, false},
		{"override ddg", map[string]string{"LOOM_WEB_SEARCH_PROVIDER": "duckduckgo", "BRAVE_SEARCH_API_KEY": "k"}, providerDuckDuckGo, false},
		{"override brave without key", map[string]string{"LOOM_WEB_SEARCH_PROVIDER": "brave"}, "", true},
		{"override tavily without key", map[string]string{"LOOM_WEB_SEARCH_PROVIDER": "tavily"}, "", true},
		{"override unknown", map[string]string{"LOOM_WEB_SEARCH_PROVIDER": "bing"}, "", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			provider, err := selectProvider(getenvFrom(tt.env))
			if tt.wantErr {
				if err == nil {
					t.Fatalf("selectProvider() expected error, got %v", provider.Name())
				}
				return
			}
			if err != nil {
				t.Fatalf("selectProvider() error = %v", err)
			}
			if provider.Name() != tt.wantName {
				t.Fatalf("provider = %q, want %q", provider.Name(), tt.wantName)
			}
		})
	}
}

func TestBraveProviderParsesResults(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Subscription-Token") != "test-key" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		if r.URL.Query().Get("q") != "golang" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"web":{"results":[{"title":"The Go Programming Language","url":"https://go.dev","description":"Go is an open source  language.\nMultiline."},{"title":"","url":"","description":"skip me"}]}}`))
	}))
	defer server.Close()

	provider := &braveProvider{endpoint: server.URL, apiKey: "test-key"}
	results, err := provider.Search(context.Background(), server.Client(), "golang", 5)
	if err != nil {
		t.Fatalf("Search() error = %v", err)
	}
	if len(results) != 1 {
		t.Fatalf("len(results) = %d, want 1", len(results))
	}
	if results[0].Title != "The Go Programming Language" || results[0].URL != "https://go.dev" {
		t.Fatalf("result = %+v", results[0])
	}
	if strings.Contains(results[0].Snippet, "\n") {
		t.Fatalf("snippet not normalized: %q", results[0].Snippet)
	}
}

func TestBraveProviderAuthFailure(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
	}))
	defer server.Close()
	provider := &braveProvider{endpoint: server.URL, apiKey: "bad"}
	_, err := provider.Search(context.Background(), server.Client(), "q", 5)
	var agentErr *domain.AgentError
	if err == nil || !errors.As(err, &agentErr) || agentErr.Code != domain.ErrSecurity {
		t.Fatalf("Search() error = %v, want security AgentError", err)
	}
}

func TestTavilyProviderParsesResults(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body map[string]any
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		if body["api_key"] != "tv-key" || body["query"] != "loom" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"results":[{"title":"Loom","url":"https://example.com/loom","content":"An agent."}]}`))
	}))
	defer server.Close()

	provider := &tavilyProvider{endpoint: server.URL, apiKey: "tv-key"}
	results, err := provider.Search(context.Background(), server.Client(), "loom", 5)
	if err != nil {
		t.Fatalf("Search() error = %v", err)
	}
	if len(results) != 1 || results[0].URL != "https://example.com/loom" || results[0].Snippet != "An agent." {
		t.Fatalf("results = %+v", results)
	}
}

func TestDuckDuckGoProviderParsesHTML(t *testing.T) {
	doc := `<html><body>
<div class="result"><a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fgo.dev%2F&rut=abc">The <b>Go</b> Programming Language</a>
<a class="result__snippet" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fgo.dev%2F">Go is an open source programming language.</a></div>
<div class="result"><a rel="nofollow" class="result__a" href="https://example.com/direct">Direct Link</a>
<a class="result__snippet">Second snippet.</a></div>
</body></html>`
	results := parseDuckDuckGoHTML([]byte(doc), 5)
	if len(results) != 2 {
		t.Fatalf("len(results) = %d, want 2: %+v", len(results), results)
	}
	if results[0].URL != "https://go.dev/" {
		t.Fatalf("results[0].URL = %q, want unwrapped https://go.dev/", results[0].URL)
	}
	if results[0].Title != "The Go Programming Language" {
		t.Fatalf("results[0].Title = %q, want tags stripped", results[0].Title)
	}
	if results[0].Snippet != "Go is an open source programming language." {
		t.Fatalf("results[0].Snippet = %q", results[0].Snippet)
	}
	if results[1].URL != "https://example.com/direct" {
		t.Fatalf("results[1].URL = %q, want direct link passthrough", results[1].URL)
	}
	if limited := parseDuckDuckGoHTML([]byte(doc), 1); len(limited) != 1 {
		t.Fatalf("len(limited) = %d, want 1", len(limited))
	}
}

func TestParseDuckDuckGoHTMLEmptyReturnsNothing(t *testing.T) {
	if got := parseDuckDuckGoHTML([]byte("<html>no results</html>"), 5); len(got) != 0 {
		t.Fatalf("parseDuckDuckGoHTML() = %v, want empty", got)
	}
}

func newTestTool(t *testing.T, provider searchProvider) *WebSearchTool {
	t.Helper()
	tool, err := newWebSearchTool(provider, func() time.Time { return time.Date(2026, 8, 1, 0, 0, 0, 0, time.UTC) })
	if err != nil {
		t.Fatalf("newWebSearchTool() error = %v", err)
	}
	tool.allowPrivate = true
	return tool
}

func newToolCall(t *testing.T, args searchArgs) domain.ToolCall {
	t.Helper()
	data, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_search", Arguments: data}
}

func decodeOutput(t *testing.T, result domain.ToolResult) searchOutput {
	t.Helper()
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("result status = %s, want success: %+v", result.Status, result.Error)
	}
	if len(result.Content) != 1 || result.Content[0].Kind != domain.PartText {
		t.Fatalf("unexpected content: %+v", result.Content)
	}
	var out searchOutput
	if err := json.Unmarshal([]byte(result.Content[0].Text), &out); err != nil {
		t.Fatalf("decode output: %v", err)
	}
	return out
}

func TestWebSearchToolEndToEndAndCache(t *testing.T) {
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"results":[{"title":"T","url":"https://example.com","content":"C"}]}`))
	}))
	defer server.Close()

	tool := newTestTool(t, &tavilyProvider{endpoint: server.URL, apiKey: "k"})
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, searchArgs{Query: "loom"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	if prepared.Risk != domain.R3 {
		t.Fatalf("Risk = %v, want R3 (network)", prepared.Risk)
	}

	out := decodeOutput(t, tool.Execute(context.Background(), prepared))
	if out.Cache != "miss" || out.Provider != providerTavily || out.Count != 1 {
		t.Fatalf("first output = %+v", out)
	}
	if out.Results[0].URL != "https://example.com" {
		t.Fatalf("result = %+v", out.Results[0])
	}

	// Second identical call must be served from the cache (no new HTTP call).
	prepared2, err := tool.Prepare(context.Background(), newToolCall(t, searchArgs{Query: "loom"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	out = decodeOutput(t, tool.Execute(context.Background(), prepared2))
	if out.Cache != "hit" {
		t.Fatalf("second output cache = %q, want hit", out.Cache)
	}
	if calls != 1 {
		t.Fatalf("provider called %d times, want 1 (cache hit on second)", calls)
	}
}

func TestWebSearchToolRejectsTampering(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"results":[]}`))
	}))
	defer server.Close()
	tool := newTestTool(t, &tavilyProvider{endpoint: server.URL, apiKey: "k"})

	prepared, err := tool.Prepare(context.Background(), newToolCall(t, searchArgs{Query: "loom"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	prepared.Call.Arguments = json.RawMessage(`{"query":"tampered"}`)
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("result = %+v, want security error", result)
	}
}

func TestWebSearchToolValidatesArgs(t *testing.T) {
	tool := newTestTool(t, &duckDuckGoProvider{endpoint: "https://html.duckduckgo.com/html/"})
	if _, err := tool.Prepare(context.Background(), newToolCall(t, searchArgs{Query: ""})); err == nil {
		t.Fatal("empty query should fail")
	}
	if _, err := tool.Prepare(context.Background(), newToolCall(t, searchArgs{Query: "q", Count: 99})); err == nil {
		t.Fatal("count > max should fail")
	}
}
