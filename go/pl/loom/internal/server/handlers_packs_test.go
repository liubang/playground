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
// Created: 2026/08/11

package server

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/app"
)

// packsServer builds a server over a service with the given rules dir.
func packsServer(t *testing.T, rulesDir string) *httptest.Server {
	t.Helper()
	svc := newTestServiceFull(t, nil, nil, app.SessionServiceConfig{RulesDir: rulesDir})
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	return ts
}

// packByID extracts one pack object from a /v1/rules/packs response.
func packByID(t *testing.T, body map[string]any, id string) map[string]any {
	t.Helper()
	packs, ok := body["packs"].([]any)
	if !ok {
		t.Fatalf("packs missing in %v", body)
	}
	for _, p := range packs {
		pm := p.(map[string]any)
		if pm["id"] == id {
			return pm
		}
	}
	t.Fatalf("pack %q not found in %v", id, body["packs"])
	return nil
}

func TestRulePacksListInstallUninstall(t *testing.T) {
	rulesDir := t.TempDir()
	ts := packsServer(t, rulesDir)

	// List before install: embedded packs, none installed.
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/rules/packs", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	if packs := body["packs"].([]any); len(packs) < 3 {
		t.Fatalf("packs count = %d, want >= 3", len(packs))
	}
	if pm := packByID(t, body, "go-toolchain"); pm["installed"] != false {
		t.Fatalf("go-toolchain installed before install: %v", pm)
	}

	// Install: writes pack-go-toolchain.json; response reports installed.
	status, body = doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/rules/packs/go-toolchain/install", "{}")
	if status != http.StatusOK {
		t.Fatalf("PUT install status = %d (%v)", status, body)
	}
	installed := body["pack"].(map[string]any)
	if installed["installed"] != true {
		t.Fatalf("install response not installed: %v", installed)
	}
	path, _ := installed["path"].(string)
	if path == "" {
		t.Fatal("install response missing path")
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("installed file missing: %v", err)
	}
	if filepath.Base(path) != "pack-go-toolchain.json" {
		t.Fatalf("installed file name = %q, want pack-go-toolchain.json", filepath.Base(path))
	}

	// List after install reflects state.
	status, body = doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/rules/packs", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d", status)
	}
	if pm := packByID(t, body, "go-toolchain"); pm["installed"] != true {
		t.Fatalf("go-toolchain not installed after install: %v", pm)
	}

	// Uninstall removes the file.
	status, body = doJSON(t, ts.Client(), http.MethodDelete, ts.URL+"/v1/rules/packs/go-toolchain", "")
	if status != http.StatusOK {
		t.Fatalf("DELETE status = %d (%v)", status, body)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("installed file still present after uninstall")
	}

	// Unknown pack id → 400.
	status, _ = doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/rules/packs/no-such/install", "{}")
	if status != http.StatusBadRequest {
		t.Fatalf("install unknown pack status = %d, want 400", status)
	}
}

// TestRulePacksRequireRulesDir guards the degraded mode: without a
// configured rules dir, install answers 400 and list still works (packs
// reported uninstalled).
func TestRulePacksRequireRulesDir(t *testing.T) {
	ts := packsServer(t, "")

	status, _ := doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/rules/packs/go-toolchain/install", "{}")
	if status != http.StatusBadRequest {
		t.Fatalf("install without rules dir status = %d, want 400", status)
	}
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/rules/packs", "")
	if status != http.StatusOK {
		t.Fatalf("list status = %d", status)
	}
	if pm := packByID(t, body, "go-toolchain"); pm["installed"] != false {
		t.Fatalf("pack must report uninstalled without rules dir: %v", pm)
	}
}
