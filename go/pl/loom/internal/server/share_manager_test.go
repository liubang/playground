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
// Created: 2026/08/09

package server

import (
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

func TestPublicBaseFor(t *testing.T) {
	cases := []struct {
		name  string
		bound string
		want  string
	}{
		{"loopback", "127.0.0.1:7681", "http://127.0.0.1:7681"},
		{"localhost name", "localhost:7681", "http://127.0.0.1:7681"},
		{"specific LAN addr", "192.168.1.5:7681", "http://192.168.1.5:7681"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := publicBaseFor(tc.bound)
			if err != nil {
				t.Fatalf("publicBaseFor: %v", err)
			}
			if got != tc.want {
				t.Fatalf("publicBaseFor(%q) = %q, want %q", tc.bound, got, tc.want)
			}
		})
	}
	// Unspecified bind: derived from the outbound interface (or an error
	// when no route exists); never empty-without-error on a routed host.
	got, err := publicBaseFor("0.0.0.0:7681")
	if err == nil && !strings.HasPrefix(got, "http://") {
		t.Fatalf("unspecified bind base = %q, want http:// URL", got)
	}
	if _, err := publicBaseFor("bad"); err == nil {
		t.Fatal("malformed bound addr: want error")
	}
}

// newTestShareManager builds a manager whose factory creates share-only
// servers over a fake-model service.
func newTestShareManager(t *testing.T) *ShareManager {
	t.Helper()
	svc := newTestService(t, fakes.NewFakeModel())
	mgr := NewShareManager(func(listen string) (*Server, error) {
		return New(Config{
			Listen: listen, Token: testToken, Version: "test", Service: svc, ShareOnly: true,
		})
	}, nil)
	t.Cleanup(mgr.Close)
	return mgr
}

func TestShareManagerLifecycle(t *testing.T) {
	mgr := newTestShareManager(t)

	if got := mgr.PublicBase(); got != "" {
		t.Fatalf("PublicBase before enable = %q, want empty", got)
	}
	if err := mgr.Apply(true, "127.0.0.1:0"); err != nil {
		t.Fatalf("enable: %v", err)
	}
	base := mgr.PublicBase()
	if !strings.HasPrefix(base, "http://127.0.0.1:") {
		t.Fatalf("PublicBase = %q, want loopback URL", base)
	}
	st := mgr.State()
	if !st.Enabled || st.URL != base || st.Error != "" {
		t.Fatalf("State = %+v, want enabled with url and no error", st)
	}

	// Same address again: no rebind, same URL.
	if err := mgr.Apply(true, "127.0.0.1:0"); err != nil {
		t.Fatalf("re-apply: %v", err)
	}
	if got := mgr.PublicBase(); got != base {
		t.Fatalf("PublicBase after idempotent apply = %q, want %q", got, base)
	}

	// A different bind address rebinds.
	if err := mgr.Apply(true, "localhost:0"); err != nil {
		t.Fatalf("rebind: %v", err)
	}
	if got := mgr.PublicBase(); got == "" {
		t.Fatal("PublicBase after rebind is empty")
	}

	// Disable: listener down, URL cleared.
	if err := mgr.Apply(false, "127.0.0.1:0"); err != nil {
		t.Fatalf("disable: %v", err)
	}
	st = mgr.State()
	if st.Enabled || st.URL != "" {
		t.Fatalf("State after disable = %+v, want disabled", st)
	}
}

func TestShareManagerBindFailure(t *testing.T) {
	// Occupy a port so the manager's bind fails.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("occupy port: %v", err)
	}
	defer ln.Close()

	mgr := newTestShareManager(t)
	if err := mgr.Apply(true, ln.Addr().String()); err == nil {
		t.Fatal("Apply on an occupied port: want error")
	}
	st := mgr.State()
	if st.Enabled || st.Error == "" {
		t.Fatalf("State after bind failure = %+v, want disabled with error", st)
	}
	// The manager recovers on the next successful Apply.
	if err := mgr.Apply(true, "127.0.0.1:0"); err != nil {
		t.Fatalf("Apply after failure: %v", err)
	}
	if st := mgr.State(); !st.Enabled || st.Error != "" {
		t.Fatalf("State after recovery = %+v, want enabled without error", st)
	}
}

// TestShareEndpointToggle exercises the write-through switch: POST
// persists share.enabled into the config file and hot-applies it (the
// listener follows via the reconcile), GET reports the live state.
func TestShareEndpointToggle(t *testing.T) {
	// A concrete free port: config validation rejects :0 (share links
	// must survive restarts), so probe one and write it into the file.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("probe free port: %v", err)
	}
	freeAddr := ln.Addr().String()
	_ = ln.Close()
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	cfgBody := fmt.Sprintf(`
default: deepseek/deepseek-chat
providers:
  - name: deepseek
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: sk-test
    models:
      - name: deepseek-chat
share:
  listen: %s
`, freeAddr)
	if err := os.WriteFile(cfgPath, []byte(cfgBody), 0o600); err != nil {
		t.Fatalf("write config: %v", err)
	}

	// The manager is wired into the service (hot-apply reconcile) and the
	// UI server (endpoint routes); the factory dereferences svc only when
	// a listener actually starts, always after the assignment below.
	var svc *app.SessionService
	mgr := NewShareManager(func(listen string) (*Server, error) {
		return New(Config{
			Listen: listen, Token: testToken, Version: "test", Service: svc, ShareOnly: true,
		})
	}, nil)
	t.Cleanup(mgr.Close)
	svc, _ = newTestServiceFull(t, fakes.NewFakeModel(), nil, app.SessionServiceConfig{ShareEndpoint: mgr})
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc, ConfigPath: cfgPath, Share: mgr})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.http.Handler)
	t.Cleanup(ts.Close)

	status, state := doJSON(t, ts.Client(), "GET", ts.URL+"/v1/share/endpoint", "")
	if status != http.StatusOK || state["enabled"] != false {
		t.Fatalf("initial state = (%d, %v), want 200 disabled", status, state)
	}

	// Enable: persisted into the file and live immediately.
	status, resp := doJSON(t, ts.Client(), "POST", ts.URL+"/v1/share/endpoint", `{"enabled":true}`)
	if status != http.StatusOK {
		t.Fatalf("enable status = %d (%v), want 200", status, resp)
	}
	endpoint, _ := resp["endpoint"].(map[string]any)
	url, _ := endpoint["url"].(string)
	if endpoint["enabled"] != true || !strings.HasPrefix(url, "http://127.0.0.1:") {
		t.Fatalf("endpoint after enable = %v, want enabled with loopback url", endpoint)
	}
	if imm := fmt.Sprint(resp["applied"]); !strings.Contains(imm, "share") {
		t.Fatalf("applied = %v, want share in immediate", resp["applied"])
	}
	raw, err := os.ReadFile(cfgPath)
	if err != nil || !strings.Contains(string(raw), "enabled: true") {
		t.Fatalf("config file = %q, %v; want share.enabled: true persisted", raw, err)
	}

	// Disable: persisted and the listener stops.
	_, resp = doJSON(t, ts.Client(), "POST", ts.URL+"/v1/share/endpoint", `{"enabled":false}`)
	endpoint, _ = resp["endpoint"].(map[string]any)
	if endpoint["enabled"] != false {
		t.Fatalf("endpoint after disable = %v, want disabled", endpoint)
	}
	raw, err = os.ReadFile(cfgPath)
	if err != nil || !strings.Contains(string(raw), "enabled: false") {
		t.Fatalf("config file = %q, %v; want share.enabled: false persisted", raw, err)
	}
}

// TestShareEndpointUnmanaged: a server without a ShareManager (`loom
// serve`) has no management routes, so the frontend hides the toggle.
func TestShareEndpointUnmanaged(t *testing.T) {
	ts, _ := newTestServer(t, fakes.NewFakeModel())
	req, _ := http.NewRequest("GET", ts.URL+"/v1/share/endpoint", nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET share endpoint: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("GET share endpoint without manager status = %d, want 404", resp.StatusCode)
	}
}
