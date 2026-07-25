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

	client := newScoreClient(server.URL, "pk", "sk")
	err := client.post(context.Background(), scoreRequest{
		TraceID: "abc123", Name: "run_success", Value: 1, Comment: "ok",
	})
	if err != nil {
		t.Fatalf("post: %v", err)
	}
	if got["traceId"] != "abc123" || got["name"] != "run_success" || got["value"] != float64(1) {
		t.Fatalf("score payload = %v", got)
	}
}
