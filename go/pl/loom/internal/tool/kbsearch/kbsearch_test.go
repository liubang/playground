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
// Created: 2026/08/17

package kbsearch

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func mustNew(t *testing.T, opts Options) (*SearchTool, *ReadTool) {
	t.Helper()
	s, r, err := New(opts)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return s, r
}

func singleOpts(baseURL string) Options {
	return Options{
		BaseURL:           baseURL,
		APIKey:            "msk_test",
		Timeout:           5 * time.Second,
		DefaultTopK:       5,
		DefaultCollection: "loom-kb",
		Collections:       []Collection{{Name: "loom-kb", Description: "loom docs"}},
	}
}

func multiOpts(baseURL string) Options {
	return Options{
		BaseURL:           baseURL,
		APIKey:            "msk_test",
		Timeout:           5 * time.Second,
		DefaultTopK:       5,
		DefaultCollection: "loom-kb",
		Collections: []Collection{
			{Name: "loom-kb", Description: "loom docs"},
			{Name: "presto-oncall", Description: "presto on-call"},
		},
	}
}

func callID() domain.ToolCallID { return domain.NewToolCallID() }

func decodeResult(t *testing.T, res domain.ToolResult, out any) {
	t.Helper()
	if len(res.Content) == 0 {
		t.Fatalf("result has no content parts")
	}
	if err := json.Unmarshal([]byte(res.Content[0].Text), out); err != nil {
		t.Fatalf("unmarshal result content: %v", err)
	}
}

func searchJSON() string {
	return `{
  "hits": [
    {
      "id": "kb-presto-tuning",
      "score": 0.031,
      "document": {
        "id": "kb-presto-tuning",
        "fields": {
          "title": {"s": "Presto 调优指南"},
          "content": {"s": "当 CPU 热点出现在 join 阶段时优先检查统计信息是否过期"},
          "tags": {"s": "presto"},
          "embedding": {"v": {"data": [0.1, 0.2]}}
        }
      }
    }
  ],
  "took_ms": 3
}`
}

// TestSearch_HappyPath verifies the request is built correctly (path,
// method, auth header, JSON body) and the response is unwrapped, with
// vector fields dropped and long strings truncated.
func TestSearch_HappyPath(t *testing.T) {
	var gotPath, gotMethod, gotAuth, gotBody string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotMethod = r.Method
		gotAuth = r.Header.Get("Authorization")
		b, _ := io.ReadAll(r.Body)
		gotBody = string(b)
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(searchJSON()))
	}))
	defer srv.Close()

	search, _ := mustNew(t, singleOpts(srv.URL))
	call := domain.ToolCall{
		ID: callID(), Name: "kb_search",
		Arguments: json.RawMessage(`{"query":"presto 调优","top_k":3}`),
	}
	prepared, err := search.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	res := search.Execute(context.Background(), prepared)
	if res.Status != domain.ToolStatusSuccess {
		t.Fatalf("status=%v err=%v", res.Status, res.Error)
	}

	if gotPath != "/api/v2/loom-kb/search" || gotMethod != http.MethodPost {
		t.Fatalf("unexpected path/method: %s %s", gotMethod, gotPath)
	}
	if gotAuth != "Bearer msk_test" {
		t.Fatalf("auth header: got %q", gotAuth)
	}
	var req struct {
		Text string `json:"text"`
		TopK int    `json:"top_k"`
	}
	if err := json.Unmarshal([]byte(gotBody), &req); err != nil || req.Text != "presto 调优" || req.TopK != 3 {
		t.Fatalf("request body unexpected: %q err=%v", gotBody, err)
	}

	var out searchOutput
	decodeResult(t, res, &out)
	if out.Count != 1 || len(out.Results) != 1 {
		t.Fatalf("count/results: %d %d", out.Count, len(out.Results))
	}
	h := out.Results[0]
	if h.ID != "kb-presto-tuning" {
		t.Fatalf("id: %q", h.ID)
	}
	title, _ := h.Fields["title"].(string)
	if title != "Presto 调优指南" {
		t.Fatalf("title: %q", title)
	}
	if _, hasVec := h.Fields["embedding"]; hasVec {
		t.Fatalf("vector field should be dropped")
	}
}

// TestSearch_LongFieldTruncated confirms long string fields are capped in
// search results (kb_read returns them in full elsewhere).
func TestSearch_LongFieldTruncated(t *testing.T) {
	long := strings.Repeat("x", searchFieldChars*4)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"hits":[{"id":"d1","score":0.1,"document":{"id":"d1","fields":{"content":{"s":"` + long + `"}}}}]}`))
	}))
	defer srv.Close()

	search, _ := mustNew(t, singleOpts(srv.URL))
	prepared, _ := search.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_search",
		Arguments: json.RawMessage(`{"query":"x"}`),
	})
	res := search.Execute(context.Background(), prepared)
	var out searchOutput
	decodeResult(t, res, &out)
	c := out.Results[0].Fields["content"].(string)
	if !strings.HasSuffix(c, "…") || len(c) > searchFieldChars+10 {
		t.Fatalf("content not truncated: len=%d suffix=%q", len(c), c[len(c)-3:])
	}
}

// TestSearch_ServiceDown verifies a minisearch outage degrades gracefully:
// the tool reports success with an empty result array, never an error.
func TestSearch_ServiceDown(t *testing.T) {
	search, _ := mustNew(t, singleOpts("http://127.0.0.1:1")) // unreachable
	prepared, err := search.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_search",
		Arguments: json.RawMessage(`{"query":"x"}`),
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	res := search.Execute(context.Background(), prepared)
	if res.Status != domain.ToolStatusSuccess {
		t.Fatalf("expected graceful success, got status=%v err=%v", res.Status, res.Error)
	}
	var out searchOutput
	decodeResult(t, res, &out)
	if out.Count != 0 || len(out.Results) != 0 {
		t.Fatalf("expected empty results, got count=%d", out.Count)
	}
	// Degradation must be explicit: an un-note'd empty array would read as
	// "nothing relevant exists" even though the query was never answered.
	if out.Note == "" || !strings.Contains(out.Note, "NOT answered") {
		t.Fatalf("expected an explicit unavailability note, got %q", out.Note)
	}
}

// TestSearch_500Status also degrades to empty results.
func TestSearch_500Status(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer srv.Close()
	search, _ := mustNew(t, singleOpts(srv.URL))
	prepared, _ := search.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_search",
		Arguments: json.RawMessage(`{"query":"x"}`),
	})
	res := search.Execute(context.Background(), prepared)
	if res.Status != domain.ToolStatusSuccess {
		t.Fatalf("expected graceful success, got %v", res.Status)
	}
	var out searchOutput
	decodeResult(t, res, &out)
	if out.Count != 0 {
		t.Fatalf("expected empty results")
	}
	if out.Note == "" {
		t.Fatalf("expected an explicit unavailability note on 5xx")
	}
}

// TestSearch_PrepareValidation covers argument validation paths.
func TestSearch_PrepareValidation(t *testing.T) {
	srch, _ := mustNew(t, singleOptions())
	cases := []struct {
		name string
		args string
	}{
		{"empty query", `{"query":""}`},
		{"oversized query", `{"query":"` + strings.Repeat("x", maxQueryBytes+1) + `"}`},
		{"top_k too large", `{"query":"x","top_k":21}`},
		{"unknown collection", `{"query":"x","collection":"nope"}`},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := srch.Prepare(context.Background(), domain.ToolCall{
				ID: callID(), Name: "kb_search",
				Arguments: json.RawMessage(c.args),
			})
			if err == nil {
				t.Fatalf("expected error")
			}
		})
	}
}

// TestSearch_CollectionEnumInSchema verifies the collection argument is
// emitted only when multiple collections are configured.
func TestSearch_CollectionEnumInSchema(t *testing.T) {
	single, _ := mustNew(t, singleOptions())
	if strings.Contains(string(single.Definition().InputSchema), "collection") {
		t.Fatalf("single collection must not advertise collection arg")
	}
	multi, _ := mustNew(t, multiOpts("http://example.invalid"))
	def := multi.Definition()
	if !strings.Contains(string(def.InputSchema), "loom-kb") ||
		!strings.Contains(string(def.InputSchema), "presto-oncall") {
		t.Fatalf("multi-collection schema missing enum values:\n%s", def.InputSchema)
	}
	if !strings.Contains(def.Description, "loom docs") {
		t.Fatalf("collection description not surfaced in tool description: %q", def.Description)
	}
}

func singleOptions() Options { return singleOpts("http://example.invalid") }

// TestRead_Found verifies kb_read fetches and unwraps a document.
func TestRead_Found(t *testing.T) {
	var gotPath, gotMethod string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotMethod = r.Method
		_, _ = w.Write([]byte(`{"found":true,"document":{"id":"kb-presto-tuning","fields":{"title":{"s":"Presto 调优指南"},"content":{"s":"全文内容"}}}}`))
	}))
	defer srv.Close()

	_, read := mustNew(t, singleOpts(srv.URL))
	prepared, err := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"kb-presto-tuning"}`),
	})
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	res := read.Execute(context.Background(), prepared)
	if res.Status != domain.ToolStatusSuccess {
		t.Fatalf("status=%v err=%v", res.Status, res.Error)
	}
	if gotPath != "/api/v2/loom-kb/documents/kb-presto-tuning" || gotMethod != http.MethodGet {
		t.Fatalf("path/method: %s %s", gotMethod, gotPath)
	}
	var out readOutput
	decodeResult(t, res, &out)
	if !out.Found {
		t.Fatalf("found=false")
	}
	if out.Fields["title"] != "Presto 调优指南" {
		t.Fatalf("title: %v", out.Fields["title"])
	}
}

// TestRead_NotFound verifies a 404 (or found=false) surfaces as found=false
// so the model picks another result instead of retrying the same id.
func TestRead_NotFound(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	}))
	defer srv.Close()
	_, read := mustNew(t, singleOpts(srv.URL))
	prepared, _ := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"missing"}`),
	})
	res := read.Execute(context.Background(), prepared)
	var out readOutput
	decodeResult(t, res, &out)
	if out.Found {
		t.Fatalf("expected found=false on 404")
	}
	// A genuine 404 is a verified negative: it must NOT carry the
	// unavailability note, or the model could not tell missing from down.
	if out.Note != "" {
		t.Fatalf("genuine 404 must not carry a degradation note, got %q", out.Note)
	}
}

// TestRead_FoundFalseBody covers the 200-with-found:false variant.
func TestRead_FoundFalseBody(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"found":false}`))
	}))
	defer srv.Close()
	_, read := mustNew(t, singleOpts(srv.URL))
	prepared, _ := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"x"}`),
	})
	res := read.Execute(context.Background(), prepared)
	var out readOutput
	decodeResult(t, res, &out)
	if out.Found {
		t.Fatalf("expected found=false")
	}
}

// TestRead_ServiceDown degrades to found=false, not an error.
func TestRead_ServiceDown(t *testing.T) {
	_, read := mustNew(t, singleOpts("http://127.0.0.1:1"))
	prepared, _ := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"x"}`),
	})
	res := read.Execute(context.Background(), prepared)
	if res.Status != domain.ToolStatusSuccess {
		t.Fatalf("expected graceful success, got %v", res.Status)
	}
	var out readOutput
	decodeResult(t, res, &out)
	if out.Found {
		t.Fatalf("expected found=false on outage")
	}
	if out.Note == "" || !strings.Contains(out.Note, "not verified") {
		t.Fatalf("expected an explicit unavailability note, got %q", out.Note)
	}
}

// TestRead_IdWithSlash verifies ids containing "/" are routed correctly
// (the server treats the whole path tail as the id, so "/" stays literal).
// TestRead_IdWithSlash keeps "/" segments escaped individually (the server
// treats the whole tail as the id).
func TestRead_IdWithSlash(t *testing.T) {
	var gotPath string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.EscapedPath()
		_, _ = w.Write([]byte(`{"found":true,"document":{"id":"docs/a.md","fields":{}}}`))
	}))
	defer srv.Close()
	_, read := mustNew(t, singleOpts(srv.URL))
	prepared, _ := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"docs/a.md"}`),
	})
	read.Execute(context.Background(), prepared)
	if gotPath != "/api/v2/loom-kb/documents/docs/a.md" {
		t.Fatalf("path: %q", gotPath)
	}
}

// TestRead_ChunkIdWithHash escapes "#" in imported-markdown chunk ids
// (e.g. presto-tuning#chunk_0); a raw "#" would be parsed as a URL fragment.
func TestRead_ChunkIdWithHash(t *testing.T) {
	var gotPath string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.EscapedPath()
		_, _ = w.Write([]byte(`{"found":true,"document":{"id":"presto-tuning#chunk_0","fields":{}}}`))
	}))
	defer srv.Close()
	_, read := mustNew(t, singleOpts(srv.URL))
	prepared, _ := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":"presto-tuning#chunk_0"}`),
	})
	read.Execute(context.Background(), prepared)
	if gotPath != "/api/v2/loom-kb/documents/presto-tuning%23chunk_0" {
		t.Fatalf("path: %q", gotPath)
	}
}

// TestRead_ChunkLevelSemantics locks the minisearch-aligned id contract into
// the tool surface: ids are chunk-level and must be copied verbatim from
// kb_search results.
func TestRead_ChunkLevelSemantics(t *testing.T) {
	_, read := mustNew(t, singleOptions())
	def := read.Definition()
	if !strings.Contains(def.Description, "#chunk_N") ||
		!strings.Contains(def.Description, "verbatim") {
		t.Fatalf("read description must state chunk-level verbatim ids: %q", def.Description)
	}
	if !strings.Contains(string(def.InputSchema), "#chunk_N") {
		t.Fatalf("id schema must mention chunk-level ids:\n%s", def.InputSchema)
	}
}

// TestRead_EmptyId rejects an empty id at Prepare time.
func TestRead_EmptyId(t *testing.T) {
	_, read := mustNew(t, singleOptions())
	_, err := read.Prepare(context.Background(), domain.ToolCall{
		ID: callID(), Name: "kb_read",
		Arguments: json.RawMessage(`{"id":""}`),
	})
	if err == nil {
		t.Fatalf("expected error for empty id")
	}
}

// TestNew_InvalidOptions covers constructor validation.
func TestNew_InvalidOptions(t *testing.T) {
	cases := []struct {
		name string
		opts Options
	}{
		{"no base_url", Options{Timeout: time.Second, DefaultTopK: 5, Collections: []Collection{{Name: "c"}}}},
		{"no collections", Options{BaseURL: "http://x", Timeout: time.Second, DefaultTopK: 5}},
		{"bad top_k", Options{BaseURL: "http://x", Timeout: time.Second, DefaultTopK: 99, Collections: []Collection{{Name: "c"}}}},
		{"zero timeout", Options{BaseURL: "http://x", DefaultTopK: 5, Collections: []Collection{{Name: "c"}}}},
		{"default_collection not in collections", Options{BaseURL: "http://x", Timeout: time.Second, DefaultTopK: 5, DefaultCollection: "missing", Collections: []Collection{{Name: "c"}}}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, _, err := New(c.opts); err == nil {
				t.Fatalf("expected error")
			}
		})
	}
}
