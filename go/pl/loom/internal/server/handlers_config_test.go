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
// Created: 2026/08/08

package server

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// newConfigTestServer serves the adapter with ConfigPath pointed at path.
func newConfigTestServer(t *testing.T, cfgPath string) *httptest.Server {
	t.Helper()
	// Config validation resolves the default storage base (~/.loom) via
	// the process environment; the test sandbox has no $HOME.
	t.Setenv("HOME", t.TempDir())
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc, ConfigPath: cfgPath})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return ts
}

// validConfigBody is a minimal saveable configuration (one provider).
const validConfigBody = `{
	"default": "deepseek/deepseek-chat",
	"providers": [{
		"name": "deepseek",
		"type": "openai",
		"base_url": "https://api.deepseek.com/v1",
		"api_key": "sk-test",
		"models": [{"name": "deepseek-chat", "context_window": 65536}]
	}]
}`

func putConfig(t *testing.T, ts *httptest.Server, revision, configJSON string) (int, map[string]any) {
	t.Helper()
	body, err := json.Marshal(map[string]any{
		"revision": revision,
		"config":   json.RawMessage(configJSON),
	})
	if err != nil {
		t.Fatalf("marshal put body: %v", err)
	}
	return doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/config", string(body))
}

func TestGetConfigMissingFile(t *testing.T) {
	ts := newConfigTestServer(t, filepath.Join(t.TempDir(), "config.yaml"))
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/config", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	if body["exists"] != false || body["revision"] != "" {
		t.Fatalf("body = %v, want exists=false revision=\"\"", body)
	}
	if cfg, ok := body["config"].(map[string]any); !ok || len(cfg) != 0 {
		t.Fatalf("config = %v, want an empty object", body["config"])
	}
}

func TestPutThenGetConfig(t *testing.T) {
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	ts := newConfigTestServer(t, cfgPath)

	status, put := putConfig(t, ts, "", validConfigBody)
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, put)
	}
	revision, _ := put["revision"].(string)
	if revision == "" {
		t.Fatalf("PUT response missing revision: %v", put)
	}

	raw, err := os.ReadFile(cfgPath)
	if err != nil {
		t.Fatalf("config not written: %v", err)
	}
	if !strings.Contains(string(raw), "sk-test") {
		t.Fatalf("written file missing api_key:\n%s", raw)
	}
	if info, err := os.Stat(cfgPath); err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("file mode = %v, want 600", info.Mode().Perm())
	}

	status, got := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/config", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, got)
	}
	if got["revision"] != revision {
		t.Fatalf("GET revision = %v, want the PUT revision %q", got["revision"], revision)
	}
	cfg := got["config"].(map[string]any)
	providers := cfg["providers"].([]any)
	apiKey := providers[0].(map[string]any)["api_key"]
	if apiKey != config.SecretMask {
		t.Fatalf("api_key = %v, want the secret mask", apiKey)
	}
}

func TestPutConfigConflict(t *testing.T) {
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	ts := newConfigTestServer(t, cfgPath)

	status, put := putConfig(t, ts, "", validConfigBody)
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, put)
	}
	stale, _ := put["revision"].(string)

	// An external edit invalidates the issued revision.
	if err := os.WriteFile(cfgPath, []byte("providers: []\n"), 0o600); err != nil {
		t.Fatalf("external edit: %v", err)
	}
	status, body := putConfig(t, ts, stale, validConfigBody)
	if status != http.StatusConflict {
		t.Fatalf("PUT status = %d, want 409 (%v)", status, body)
	}
	if code := body["error"].(map[string]any)["code"]; code != "config_conflict" {
		t.Fatalf("error code = %v, want config_conflict", code)
	}
}

func TestPutConfigInvalid(t *testing.T) {
	ts := newConfigTestServer(t, filepath.Join(t.TempDir(), "config.yaml"))

	status, body := putConfig(t, ts, "", `{"providers": []}`)
	if status != http.StatusBadRequest {
		t.Fatalf("PUT status = %d, want 400 (%v)", status, body)
	}
	if code := body["error"].(map[string]any)["code"]; code != "invalid_input" {
		t.Fatalf("error code = %v, want invalid_input", code)
	}

	status, body = putConfig(t, ts, "", `{"provideers": []}`)
	if status != http.StatusBadRequest {
		t.Fatalf("PUT with unknown key status = %d, want 400 (%v)", status, body)
	}
}

// TestPutConfigKeepsMaskedSecrets locks the round trip: a config fetched
// with masked secrets can be edited and saved without re-entering keys.
func TestPutConfigKeepsMaskedSecrets(t *testing.T) {
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(cfgPath, []byte(`default: deepseek/deepseek-chat
providers:
  - name: deepseek
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: sk-real
    models:
      - name: deepseek-chat
        context_window: 65536
`), 0o600); err != nil {
		t.Fatalf("seed config: %v", err)
	}
	ts := newConfigTestServer(t, cfgPath)

	status, got := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/config", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, got)
	}
	cfgJSON, err := json.Marshal(got["config"])
	if err != nil {
		t.Fatalf("marshal fetched config: %v", err)
	}
	if !strings.Contains(string(cfgJSON), config.SecretMask) {
		t.Fatalf("fetched config not masked: %s", cfgJSON)
	}
	revision, _ := got["revision"].(string)

	status, put := putConfig(t, ts, revision, string(cfgJSON))
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, put)
	}
	raw, err := os.ReadFile(cfgPath)
	if err != nil {
		t.Fatalf("read config: %v", err)
	}
	if !strings.Contains(string(raw), "sk-real") {
		t.Fatalf("stored key lost after masked round trip:\n%s", raw)
	}
	if strings.Contains(string(raw), config.SecretMask) {
		t.Fatalf("mask leaked into the config file:\n%s", raw)
	}
}

// TestPutConfigHotApplyReport: a successful save hot-applies the config
// and the response classifies every changed section by when it takes
// effect.
func TestPutConfigHotApplyReport(t *testing.T) {
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	ts := newConfigTestServer(t, cfgPath)

	status, put := putConfig(t, ts, "", validConfigBody)
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, put)
	}
	applied, ok := put["applied"].(map[string]any)
	if !ok {
		t.Fatalf("PUT response missing applied report: %v", put)
	}
	// The seeded test runtime has a "test" provider and no explicit
	// storage base; the PUT introduces a deepseek provider (next turn)
	// and a different resolved storage base (restart).
	nextTurn, _ := applied["next_turn"].([]any)
	restart, _ := applied["restart"].([]any)
	if fmt.Sprint(nextTurn) == "[]" || !strings.Contains(fmt.Sprint(nextTurn), "providers") {
		t.Fatalf("next_turn = %v, want providers", nextTurn)
	}
	if !strings.Contains(fmt.Sprint(restart), "storage") {
		t.Fatalf("restart = %v, want storage", restart)
	}
	// The hot-applied config is live: the model catalog now reflects the
	// deepseek provider from the saved file.
	status, cat := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/meta/models", "")
	if status != http.StatusOK {
		t.Fatalf("GET models status = %d", status)
	}
	if !strings.Contains(fmt.Sprint(cat["models"]), "deepseek-chat") {
		t.Fatalf("model catalog = %v, want the hot-applied deepseek-chat", cat["models"])
	}
}

func TestListMCPServersEmpty(t *testing.T) {
	ts := newConfigTestServer(t, filepath.Join(t.TempDir(), "config.yaml"))
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/mcp/servers", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	servers, ok := body["servers"].([]any)
	if !ok || len(servers) != 0 {
		t.Fatalf("servers = %v, want an empty list", body["servers"])
	}
}

func TestReconnectMCPServerUnknown(t *testing.T) {
	ts := newConfigTestServer(t, filepath.Join(t.TempDir(), "config.yaml"))
	status, body := doJSON(t, ts.Client(), http.MethodPost, ts.URL+"/v1/mcp/servers/ghost/reconnect", "{}")
	if status != http.StatusBadRequest {
		t.Fatalf("POST status = %d, want 400 (%v)", status, body)
	}
	if code := body["error"].(map[string]any)["code"]; code != "invalid_input" {
		t.Fatalf("error code = %v, want invalid_input", code)
	}
}

func TestConfigUnavailableWithoutPath(t *testing.T) {
	svc := newTestService(t, fakes.NewFakeModel())
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)

	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/config", "")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("GET status = %d, want 503 (%v)", status, body)
	}
	if code := body["error"].(map[string]any)["code"]; code != "config_unavailable" {
		t.Fatalf("error code = %v, want config_unavailable", code)
	}
	status, body = putConfig(t, ts, "", validConfigBody)
	if status != http.StatusServiceUnavailable {
		t.Fatalf("PUT status = %d, want 503 (%v)", status, body)
	}
}
