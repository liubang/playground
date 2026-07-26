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
// Created: 2026/07/25

package trace

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync/atomic"
	"testing"
)

func TestConfigFromEnvExtendedFields(t *testing.T) {
	t.Setenv("LOOM_LANGFUSE_HOST", "http://lf:3100")
	t.Setenv("LOOM_LANGFUSE_PUBLIC_KEY", "pk")
	t.Setenv("LOOM_LANGFUSE_SECRET_KEY", "sk")
	t.Setenv("LOOM_TRACE_USER", "dev@example.com")
	t.Setenv("LOOM_VERSION", "0.2.0-dev")
	t.Setenv("LOOM_COST_INPUT_USD_PER_MTOK", "0.5")
	t.Setenv("LOOM_COST_OUTPUT_USD_PER_MTOK", "2.0")

	cfg := ConfigFromEnv()
	if cfg.UserID != "dev@example.com" {
		t.Fatalf("UserID = %q", cfg.UserID)
	}
	if cfg.Release != "0.2.0-dev" {
		t.Fatalf("Release = %q", cfg.Release)
	}
	if cfg.CostInputPerMTok != 0.5 || cfg.CostOutputPerMTok != 2.0 {
		t.Fatalf("costs = %v/%v", cfg.CostInputPerMTok, cfg.CostOutputPerMTok)
	}
}

func TestParseFloatEnv(t *testing.T) {
	t.Setenv("LOOM_COST_INPUT_USD_PER_MTOK", "bogus")
	if got := parseFloatEnv("LOOM_COST_INPUT_USD_PER_MTOK"); got != 0 {
		t.Fatalf("malformed rate = %v, want 0", got)
	}
	t.Setenv("LOOM_COST_INPUT_USD_PER_MTOK", "-1")
	if got := parseFloatEnv("LOOM_COST_INPUT_USD_PER_MTOK"); got != 0 {
		t.Fatalf("negative rate = %v, want 0", got)
	}
}

func TestFlattenPrompt(t *testing.T) {
	text, err := flattenPrompt(json.RawMessage(`"you are loom"`))
	if err != nil || text != "you are loom" {
		t.Fatalf("text prompt = %q, %v", text, err)
	}
	chat, err := flattenPrompt(json.RawMessage(`[{"role":"system","content":"a"},{"role":"user","content":"b"}]`))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(chat, "[system]") || !strings.Contains(chat, "b") {
		t.Fatalf("chat flatten = %q", chat)
	}
	if _, err := flattenPrompt(json.RawMessage(`""`)); err == nil {
		t.Fatal("empty prompt must error")
	}
}

// TestPromptClientFetchAndCacheFallback drives the client against a mock API:
// first fetch populates the cache; after the API dies the cache still serves.
func TestPromptClientFetchAndCacheFallback(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if !strings.HasPrefix(r.URL.Path, "/api/public/v2/prompts/") {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		if got := r.URL.Query().Get("label"); got != "production" {
			t.Errorf("label = %q, want production", got)
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"name": "loom-system", "version": 3, "prompt": "managed instructions",
		})
	}))
	defer server.Close()

	client := NewPromptClient(Config{Host: server.URL, PublicKey: "pk", SecretKey: "sk"}, t.TempDir())
	prompt, err := client.Get(context.Background(), "loom-system", "production")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if prompt.Version != 3 || prompt.Content != "managed instructions" {
		t.Fatalf("prompt = %+v", prompt)
	}

	// Kill the API: the cache must serve subsequent Gets.
	server.Close()
	cached, err := client.Get(context.Background(), "loom-system", "production")
	if err != nil {
		t.Fatalf("Get from cache: %v", err)
	}
	if cached.Content != "managed instructions" || cached.Version != 3 {
		t.Fatalf("cached prompt = %+v", cached)
	}

	// Unknown prompt with no cache must fail.
	if _, err := client.Get(context.Background(), "never-fetched", "production"); err == nil {
		t.Fatal("uncached prompt with dead API must error")
	}

	// A different label of the SAME prompt must not be served from the
	// production cache: serving staging content for a production request is
	// worse than failing over to the built-in prompt.
	if _, err := client.Get(context.Background(), "loom-system", "staging"); err == nil {
		t.Fatal("dead API must not serve another label's cache entry")
	}
}

// TestPromptCacheIsLabelKeyed verifies cache files are isolated per label
// and a label-mismatched entry is rejected even when read directly.
func TestPromptCacheIsLabelKeyed(t *testing.T) {
	dir := t.TempDir()
	client := NewPromptClient(Config{Host: "http://unused", PublicKey: "pk", SecretKey: "sk"}, dir)
	prod := &ManagedPrompt{Name: "p", Version: 1, Content: "prod content", Label: "production"}
	staging := &ManagedPrompt{Name: "p", Version: 2, Content: "staging content", Label: "staging"}
	if err := client.writeCache(prod); err != nil {
		t.Fatal(err)
	}
	if err := client.writeCache(staging); err != nil {
		t.Fatal(err)
	}
	got, err := client.readCache("p", "production")
	if err != nil || got.Content != "prod content" {
		t.Fatalf("production cache = %+v, %v", got, err)
	}
	got, err = client.readCache("p", "staging")
	if err != nil || got.Content != "staging content" {
		t.Fatalf("staging cache = %+v, %v", got, err)
	}
	// Forged entry: right filename, wrong embedded label.
	forged := &ManagedPrompt{Name: "p", Version: 9, Content: "forged", Label: "staging"}
	data, _ := json.Marshal(forged)
	if err := os.WriteFile(client.cachePath("p", "production"), data, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := client.readCache("p", "production"); err == nil {
		t.Fatal("label-mismatched cache entry must be rejected")
	}
}

func TestPromptVariables(t *testing.T) {
	vars := PromptVariables("Answer in {{language}}. Tone: {{ tone }}. Again {{language}}.")
	if len(vars) != 2 || vars[0] != "language" || vars[1] != "tone" {
		t.Fatalf("vars = %v", vars)
	}
	if vars := PromptVariables("no placeholders here"); len(vars) != 0 {
		t.Fatalf("vars = %v", vars)
	}
	if vars := PromptVariables("not a var: {{123}} but {{real_var}} is"); len(vars) != 1 || vars[0] != "real_var" {
		t.Fatalf("vars = %v", vars)
	}
}

// TestScoreClientPosts verifies the scores API request shape end-to-end.
func TestScoreClientPosts(t *testing.T) {
	var got map[string]any
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/public/scores" || r.Method != http.MethodPost {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		if !strings.HasPrefix(r.Header.Get("Authorization"), "Basic ") {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		_ = json.NewDecoder(r.Body).Decode(&got)
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	client := newScoreClient(server.URL, "pk", "sk", "dev", nil)
	err := client.post(context.Background(), scoreRequest{
		TraceID: "abc123", Name: "run_success", Value: 1, Comment: "ok", Environment: client.env,
	})
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	if got["traceId"] != "abc123" || got["name"] != "run_success" || got["value"] != float64(1) {
		t.Fatalf("score payload = %v", got)
	}
	if got["environment"] != "dev" {
		t.Fatalf("score must carry the trace environment, got %v", got["environment"])
	}
}
