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
// Created: 2026/07/24

package webfetch

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// --- helpers ---

func newToolCall(t *testing.T, name string, args any) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatalf("marshal args: %v", err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw}
}

func decodeToolResult(t *testing.T, result domain.ToolResult, out any) {
	t.Helper()
	if len(result.Content) == 0 {
		t.Fatalf("result has no content parts")
	}
	if err := json.Unmarshal([]byte(result.Content[0].Text), out); err != nil {
		t.Fatalf("unmarshal result content: %v", err)
	}
}

func newTestTool(t *testing.T, artifacts domain.ArtifactStore) *WebFetchTool {
	t.Helper()
	tool, err := NewWebFetchTool(artifacts)
	if err != nil {
		t.Fatalf("NewWebFetchTool() error = %v", err)
	}
	return tool
}

func execute(t *testing.T, tool *WebFetchTool, args fetchArgs) domain.ToolResult {
	t.Helper()
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "web_fetch", args))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	return tool.Execute(context.Background(), prepared)
}

func privateArgs(server *httptest.Server) fetchArgs {
	return fetchArgs{URL: server.URL, AllowPrivate: true}
}

// --- Prepare validation ---

func TestValidateFetchArgs(t *testing.T) {
	tests := []struct {
		name    string
		args    fetchArgs
		wantErr domain.ErrorCode
		check   func(t *testing.T, args fetchArgs)
	}{
		{
			name: "defaults applied and fragment stripped",
			args: fetchArgs{URL: "https://example.com/page#section"},
			check: func(t *testing.T, args fetchArgs) {
				if args.Format != "markdown" || args.MaxBytes != defaultMaxBytes || args.TimeoutMs != defaultTimeoutMs {
					t.Fatalf("defaults not applied: %+v", args)
				}
				if args.URL != "https://example.com/page" {
					t.Fatalf("fragment not stripped: %q", args.URL)
				}
			},
		},
		{name: "ftp scheme rejected", args: fetchArgs{URL: "ftp://example.com/f"}, wantErr: domain.ErrInvalidInput},
		{name: "userinfo rejected", args: fetchArgs{URL: "https://user:pw@example.com/"}, wantErr: domain.ErrInvalidInput},
		{name: "missing host rejected", args: fetchArgs{URL: "https:///path"}, wantErr: domain.ErrInvalidInput},
		{name: "bad format rejected", args: fetchArgs{URL: "https://example.com", Format: "pdf"}, wantErr: domain.ErrInvalidInput},
		{name: "max_bytes too small", args: fetchArgs{URL: "https://example.com", MaxBytes: 8}, wantErr: domain.ErrInvalidInput},
		{name: "max_bytes too large", args: fetchArgs{URL: "https://example.com", MaxBytes: 64 << 20}, wantErr: domain.ErrInvalidInput},
		{name: "timeout out of range", args: fetchArgs{URL: "https://example.com", TimeoutMs: 50}, wantErr: domain.ErrInvalidInput},
		{name: "literal loopback rejected", args: fetchArgs{URL: "http://127.0.0.1:8080/x"}, wantErr: domain.ErrSecurity},
		{name: "literal rfc1918 rejected", args: fetchArgs{URL: "http://192.168.1.1/"}, wantErr: domain.ErrSecurity},
		{name: "literal cgnat rejected", args: fetchArgs{URL: "http://100.64.1.1/"}, wantErr: domain.ErrSecurity},
		{
			name: "literal loopback allowed with allow_private",
			args: fetchArgs{URL: "http://127.0.0.1:8080/x", AllowPrivate: true},
		},
		{
			name: "public literal allowed",
			args: fetchArgs{URL: "http://8.8.8.8/"},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := validateFetchArgs(tt.args)
			if tt.wantErr != "" {
				if err == nil {
					t.Fatalf("expected error %s, got nil", tt.wantErr)
				}
				var ae *domain.AgentError
				if !domain.As(err, &ae) || ae.Code != tt.wantErr {
					t.Fatalf("error code = %v, want %s", err, tt.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if tt.check != nil {
				tt.check(t, got)
			}
		})
	}
}

func TestPrepareRejectsWrongNameAndUnknownFields(t *testing.T) {
	tool := newTestTool(t, nil)
	if _, err := tool.Prepare(context.Background(), newToolCall(t, "read_file", fetchArgs{URL: "https://example.com"})); err == nil {
		t.Fatal("expected error for wrong tool name")
	}
	bad := domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_fetch", Arguments: json.RawMessage(`{"url":"https://example.com","hack":true}`)}
	if _, err := tool.Prepare(context.Background(), bad); err == nil {
		t.Fatal("expected error for unknown argument field")
	}
}

func TestExecuteRejectsTamperedCall(t *testing.T) {
	tool := newTestTool(t, nil)
	prepared, err := tool.Prepare(context.Background(), newToolCall(t, "web_fetch", fetchArgs{URL: "https://example.com"}))
	if err != nil {
		t.Fatalf("Prepare() error = %v", err)
	}
	prepared.ArgsHash = "deadbeef"
	result := tool.Execute(context.Background(), prepared)
	if result.Status != domain.ToolStatusError || result.Error == nil || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("expected security error, got %+v", result)
	}
}

// --- Execute end-to-end against httptest ---

func TestExecuteHTMLToMarkdown(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, `<html><head><title>Sample Page</title><style>body{color:red}</style></head>
<body>
<h1>Hello</h1>
<p>Some <strong>bold</strong> text and <a href="/other">a link</a>.</p>
<script>evil()</script>
<pre>line1
line2</pre>
<ul><li>one</li><li>two</li></ul>
</body></html>`)
	}))
	defer server.Close()

	result := execute(t, newTestTool(t, nil), privateArgs(server))
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}
	var out fetchOutput
	decodeToolResult(t, result, &out)

	if out.Status != 200 || out.Format != "markdown" || out.Cache != "miss" || out.Truncated {
		t.Fatalf("unexpected output metadata: %+v", out)
	}
	for _, want := range []string{"# Sample Page", "# Hello", "**bold**", "[a link](" + server.URL + "/other)", "```\nline1\nline2\n```", "- one", "- two"} {
		if !strings.Contains(out.Content, want) {
			t.Fatalf("content missing %q:\n%s", want, out.Content)
		}
	}
	for _, banned := range []string{"evil()", "color:red"} {
		if strings.Contains(out.Content, banned) {
			t.Fatalf("content should not contain %q:\n%s", banned, out.Content)
		}
	}
	if out.ContentType != "text/html" {
		t.Fatalf("content_type = %q, want text/html", out.ContentType)
	}
}

func TestExecuteTextAndRawFormats(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `<html><head><title>T</title></head><body><p>Hello <a href="/x">World</a></p></body></html>`)
	}))
	defer server.Close()

	tool := newTestTool(t, nil)

	textArgs := privateArgs(server)
	textArgs.Format = "text"
	result := execute(t, tool, textArgs)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("text Execute() failed: %+v", result.Error)
	}
	var textOut fetchOutput
	decodeToolResult(t, result, &textOut)
	if strings.Contains(textOut.Content, "#") || strings.Contains(textOut.Content, "](") {
		t.Fatalf("text format should have no markdown syntax:\n%s", textOut.Content)
	}
	if !strings.Contains(textOut.Content, "Hello World") {
		t.Fatalf("text format missing content:\n%s", textOut.Content)
	}

	rawArgs := privateArgs(server)
	rawArgs.Format = "raw"
	result = execute(t, tool, rawArgs)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("raw Execute() failed: %+v", result.Error)
	}
	var rawOut fetchOutput
	decodeToolResult(t, result, &rawOut)
	if !strings.Contains(rawOut.Content, "<p>Hello <a href=") {
		t.Fatalf("raw format should contain original markup:\n%s", rawOut.Content)
	}
}

func TestExecuteJSONPassthrough(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"ok":true}`)
	}))
	defer server.Close()

	result := execute(t, newTestTool(t, nil), privateArgs(server))
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}
	var out fetchOutput
	decodeToolResult(t, result, &out)
	if out.Content != `{"ok":true}` {
		t.Fatalf("content = %q, want raw JSON", out.Content)
	}
	if out.ContentType != "application/json" {
		t.Fatalf("content_type = %q", out.ContentType)
	}
}

func TestExecuteUnsupportedContentType(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write([]byte{0x1, 0x2, 0x3})
	}))
	defer server.Close()

	result := execute(t, newTestTool(t, nil), privateArgs(server))
	if result.Status != domain.ToolStatusError || result.Error.Code != string(domain.ErrInvalidInput) {
		t.Fatalf("expected invalid_input error, got %+v", result)
	}
	if !strings.Contains(result.Error.Message, "application/octet-stream") {
		t.Fatalf("error should name the content type: %s", result.Error.Message)
	}
}

func TestExecuteHTTPStatusErrors(t *testing.T) {
	tests := []struct {
		status    int
		wantCode  domain.ErrorCode
		wantRetry bool
	}{
		{http.StatusNotFound, domain.ErrUnavailable, false},
		{http.StatusForbidden, domain.ErrUnavailable, false},
		{http.StatusInternalServerError, domain.ErrUnavailable, true},
		{http.StatusBadGateway, domain.ErrUnavailable, true},
		{http.StatusTooManyRequests, domain.ErrRateLimited, true},
	}
	for _, tt := range tests {
		t.Run(fmt.Sprintf("status_%d", tt.status), func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(tt.status)
			}))
			defer server.Close()

			result := execute(t, newTestTool(t, nil), privateArgs(server))
			if result.Status != domain.ToolStatusError {
				t.Fatalf("expected error, got success")
			}
			if result.Error.Code != string(tt.wantCode) || result.Error.Retryable != tt.wantRetry {
				t.Fatalf("error = %+v, want code=%s retryable=%v", result.Error, tt.wantCode, tt.wantRetry)
			}
		})
	}
}

func TestExecuteRedirects(t *testing.T) {
	var finalHits atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/start", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/middle", http.StatusFound)
	})
	mux.HandleFunc("/middle", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/final", http.StatusMovedPermanently)
	})
	mux.HandleFunc("/final", func(w http.ResponseWriter, r *http.Request) {
		finalHits.Add(1)
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprint(w, "landed")
	})
	mux.HandleFunc("/loop", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/loop", http.StatusFound)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	tool := newTestTool(t, nil)
	args := fetchArgs{URL: server.URL + "/start", AllowPrivate: true}
	result := execute(t, tool, args)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("redirect Execute() failed: %+v", result.Error)
	}
	var out fetchOutput
	decodeToolResult(t, result, &out)
	if out.FinalURL != server.URL+"/final" || out.Content != "landed" {
		t.Fatalf("unexpected redirect outcome: final=%q content=%q", out.FinalURL, out.Content)
	}

	loop := execute(t, tool, fetchArgs{URL: server.URL + "/loop", AllowPrivate: true})
	if loop.Status != domain.ToolStatusError || !strings.Contains(loop.Error.Message, "redirect") {
		t.Fatalf("expected redirect-limit error, got %+v", loop)
	}
}

func TestExecuteTruncationStoresArtifact(t *testing.T) {
	body := strings.Repeat("abcdefghij\n", 400) // 4400 bytes
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprint(w, body)
	}))
	defer server.Close()

	store, err := artifact.Open(t.TempDir(), 1<<20)
	if err != nil {
		t.Fatalf("artifact.Open() error = %v", err)
	}
	args := privateArgs(server)
	args.MaxBytes = 1024
	result := execute(t, newTestTool(t, store), args)
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("Execute() failed: %+v", result.Error)
	}
	var out fetchOutput
	decodeToolResult(t, result, &out)
	if !out.Truncated || out.Artifact == nil {
		t.Fatalf("expected truncated output with artifact: %+v", out)
	}
	if out.Bytes != len(body) {
		t.Fatalf("bytes = %d, want full body %d", out.Bytes, len(body))
	}
	if len(out.Content) > 1024 {
		t.Fatalf("content exceeds budget: %d", len(out.Content))
	}
	if int(out.Artifact.Size) != len(body) {
		t.Fatalf("artifact size = %d, want %d", out.Artifact.Size, len(body))
	}
}

func TestExecuteTruncationWithoutArtifactStore(t *testing.T) {
	body := strings.Repeat("x", 5000)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprint(w, body)
	}))
	defer server.Close()

	args := privateArgs(server)
	args.MaxBytes = 1024
	result := execute(t, newTestTool(t, nil), args)
	var out fetchOutput
	decodeToolResult(t, result, &out)
	if !out.Truncated || out.Artifact != nil {
		t.Fatalf("expected truncation without artifact: %+v", out)
	}
}

func TestExecuteCacheHitAndNoStore(t *testing.T) {
	var hits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprint(w, "cached body")
	}))
	defer server.Close()

	tool := newTestTool(t, nil)
	args := privateArgs(server)

	first := execute(t, tool, args)
	var firstOut fetchOutput
	decodeToolResult(t, first, &firstOut)
	if firstOut.Cache != "miss" {
		t.Fatalf("first fetch cache = %q, want miss", firstOut.Cache)
	}

	second := execute(t, tool, args)
	var secondOut fetchOutput
	decodeToolResult(t, second, &secondOut)
	if secondOut.Cache != "hit" {
		t.Fatalf("second fetch cache = %q, want hit", secondOut.Cache)
	}
	if secondOut.Content != firstOut.Content {
		t.Fatalf("cached content mismatch: %q vs %q", secondOut.Content, firstOut.Content)
	}
	if hits.Load() != 1 {
		t.Fatalf("server hits = %d, want 1 (cache should avoid refetch)", hits.Load())
	}

	// A different format is a different cache key.
	other := args
	other.Format = "text"
	third := execute(t, tool, other)
	var thirdOut fetchOutput
	decodeToolResult(t, third, &thirdOut)
	if thirdOut.Cache != "miss" || hits.Load() != 2 {
		t.Fatalf("format variant should miss cache: %+v hits=%d", thirdOut, hits.Load())
	}
}

func TestExecuteNoStoreNotCached(t *testing.T) {
	var hits atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/plain")
		w.Header().Set("Cache-Control", "no-store")
		fmt.Fprint(w, "fresh")
	}))
	defer server.Close()

	tool := newTestTool(t, nil)
	args := privateArgs(server)
	execute(t, tool, args)
	second := execute(t, tool, args)
	var out fetchOutput
	decodeToolResult(t, second, &out)
	if out.Cache != "miss" || hits.Load() != 2 {
		t.Fatalf("no-store response must not be cached: cache=%q hits=%d", out.Cache, hits.Load())
	}
}

func TestExecuteSSRFGuard(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "secret intranet")
	}))
	defer server.Close()

	// localhost resolves to a loopback address: allowed past Prepare (it is a
	// DNS name, not a literal IP) but must be blocked by the dial-time guard.
	port := server.URL[strings.LastIndex(server.URL, ":"):]
	result := execute(t, newTestTool(t, nil), fetchArgs{URL: "http://localhost" + port})
	if result.Status != domain.ToolStatusError || result.Error.Code != string(domain.ErrSecurity) {
		t.Fatalf("expected security error for loopback destination, got %+v", result)
	}

	// With allow_private the same destination is reachable.
	result = execute(t, newTestTool(t, nil), fetchArgs{URL: "http://localhost" + port, AllowPrivate: true})
	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("allow_private fetch failed: %+v", result.Error)
	}
}

// --- unit tests ---

func TestIsPublicIP(t *testing.T) {
	tests := []struct {
		ip     string
		public bool
	}{
		{"8.8.8.8", true},
		{"1.1.1.1", true},
		{"2606:4700:4700::1111", true},
		{"127.0.0.1", false},
		{"10.0.0.1", false},
		{"172.16.0.1", false},
		{"192.168.0.1", false},
		{"169.254.1.1", false},
		{"0.0.0.0", false},
		{"255.255.255.255", false},
		{"224.0.0.1", false},
		{"100.64.0.1", false},
		{"100.127.255.254", false},
		{"198.18.0.1", false},
		{"198.19.255.255", false},
		{"192.0.0.1", false},
		{"192.0.2.1", false},
		{"198.51.100.1", false},
		{"203.0.113.1", false},
		{"240.0.0.1", false},
		{"::1", false},
		{"fe80::1", false},
		{"fc00::1", false},
		{"2001:db8::1", false},
		{"ff02::1", false},
	}
	for _, tt := range tests {
		t.Run(tt.ip, func(t *testing.T) {
			ip, err := netip.ParseAddr(tt.ip)
			if err != nil {
				t.Fatalf("ParseAddr(%q) error = %v", tt.ip, err)
			}
			if got := isPublicIP(ip); got != tt.public {
				t.Fatalf("isPublicIP(%s) = %v, want %v", tt.ip, got, tt.public)
			}
		})
	}
}

func TestHTMLConversion(t *testing.T) {
	tests := []struct {
		name     string
		html     string
		contains []string
		excludes []string
	}{
		{
			name:     "headings and paragraphs",
			html:     `<h2>Title</h2><p>First</p><p>Second</p>`,
			contains: []string{"## Title", "First", "Second"},
		},
		{
			name:     "ordered list",
			html:     `<ol><li>a</li><li>b</li></ol>`,
			contains: []string{"1. a", "2. b"},
		},
		{
			name:     "nested list indent",
			html:     `<ul><li>a<ul><li>b</li></ul></li></ul>`,
			contains: []string{"- a", "  - b"},
		},
		{
			name:     "table",
			html:     `<table><tr><th>Name</th><th>Age</th></tr><tr><td>Ann</td><td>3</td></tr></table>`,
			contains: []string{"| Name | Age |", "| --- | --- |", "| Ann | 3 |"},
		},
		{
			name:     "blockquote",
			html:     `<blockquote><p>quoted</p></blockquote>`,
			contains: []string{"> quoted"},
		},
		{
			name:     "entities decoded",
			html:     `<p>fish &amp; chips &lt;tag&gt;</p>`,
			contains: []string{"fish & chips <tag>"},
		},
		{
			name:     "javascript links dropped",
			html:     `<a href="javascript:alert(1)">click</a>`,
			contains: []string{"click"},
			excludes: []string{"javascript:"},
		},
		{
			name:     "blank lines collapsed",
			html:     `<p>a</p><div></div><div></div><p>b</p>`,
			contains: []string{"a\n\nb"},
		},
		{
			name: "pre keeps blank lines",
			html: `<pre>a

b</pre>`,
			contains: []string{"```\na\n\nb\n```"},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			out, err := htmlToMarkdown(tt.html, nil)
			if err != nil {
				t.Fatalf("htmlToMarkdown() error = %v", err)
			}
			for _, want := range tt.contains {
				if !strings.Contains(out, want) {
					t.Fatalf("output missing %q:\n%s", want, out)
				}
			}
			for _, banned := range tt.excludes {
				if strings.Contains(out, banned) {
					t.Fatalf("output should not contain %q:\n%s", banned, out)
				}
			}
		})
	}
}

func TestHTMLLinkResolution(t *testing.T) {
	base := mustParseURL(t, "https://example.com/docs/guide/index.html")
	out, err := htmlToMarkdown(`<a href="../api">API</a><img src="logo.png" alt="logo">`, base)
	if err != nil {
		t.Fatalf("htmlToMarkdown() error = %v", err)
	}
	if !strings.Contains(out, "[API](https://example.com/docs/api)") {
		t.Fatalf("relative link not resolved:\n%s", out)
	}
	if !strings.Contains(out, "![logo](https://example.com/docs/guide/logo.png)") {
		t.Fatalf("relative image not resolved:\n%s", out)
	}
}

func mustParseURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("url parse: %v", err)
	}
	return u
}

func TestResponseCacheExpiryAndEviction(t *testing.T) {
	now := time.Now()
	current := now
	cache := newResponseCache(2, 1<<20, time.Minute, func() time.Time { return current })

	cache.put("a", cachedResponse{Body: "A"})
	cache.put("b", cachedResponse{Body: "B"})
	if _, ok := cache.get("a"); !ok {
		t.Fatal("expected hit for a")
	}
	// a is now most-recently-used; inserting c evicts b.
	cache.put("c", cachedResponse{Body: "C"})
	if _, ok := cache.get("b"); ok {
		t.Fatal("expected b to be evicted")
	}
	if _, ok := cache.get("a"); !ok {
		t.Fatal("expected a to survive eviction")
	}

	current = current.Add(2 * time.Minute)
	if _, ok := cache.get("a"); ok {
		t.Fatal("expected a to expire after TTL")
	}

	// Oversized bodies are never stored.
	cache.put("big", cachedResponse{Body: strings.Repeat("x", 2<<20)})
	if _, ok := cache.get("big"); ok {
		t.Fatal("oversized body should not be cached")
	}
}

func TestTruncateAtBoundary(t *testing.T) {
	s := "line one\nline two\nline three"
	if got := truncateAtBoundary(s, len(s)); got != s {
		t.Fatalf("no-op truncation changed content")
	}
	got := truncateAtBoundary(s, 20)
	if got != "line one\nline two" {
		t.Fatalf("truncateAtBoundary = %q, want line-boundary cut", got)
	}
}
