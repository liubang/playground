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
// Created: 2026/08/06

package server

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// newArtifactTestServer builds a test server with a directly accessible
// artifact store, so tests can store blobs and fetch them via HTTP.
func newArtifactTestServer(t *testing.T) (*httptest.Server, *artifact.Store) {
	t.Helper()
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	resolved := &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:         "test",
			Model:        fakes.NewFakeModel(),
			Models:       []config.Model{{Name: "model-a", ContextWindow: 128000}},
			DefaultModel: "model-a",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "model-a"},
		Limits:  domain.DefaultLimits(),
	}
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), resolved.Limits.MaxArtifactBytes)
	if err != nil {
		t.Fatalf("open artifact store: %v", err)
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	svc := app.NewSingletonWorkspaceService(&app.Bootstrap{
		ProcessRuntime: &app.ProcessRuntime{
			Resolved:   resolved,
			Current:    resolved.Default,
			Store:      store,
			Artifact:   artStore,
			Questioner: domain.AutonomousQuestioner{},
		},
		Registry: agent.NewToolRegistry(),
	}, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	srv, err := New(Config{
		Token:   testToken,
		Version: "test",
		Service: svc,
		NoWeb:   true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.http.Handler)
	t.Cleanup(ts.Close)
	return ts, artStore
}

// TestHandleArtifactPNG stores a small PNG via the artifact store and
// fetches it through the HTTP endpoint, verifying content type sniffing,
// cache headers, and body correctness.
func TestHandleArtifactPNG(t *testing.T) {
	ts, artStore := newArtifactTestServer(t)

	// A minimal 1×1 PNG.
	pngData := []byte{
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
		0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
		0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
		0x00, 0x00, 0x03, 0x00, 0x01, 0x5B, 0x70, 0x61,
		0x1B, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
		0x44, 0xAE, 0x42, 0x60, 0x82,
	}

	ref, err := artStore.PutBytes(context.Background(), pngData)
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}

	url := ts.URL + "/v1/artifacts/" + ref.ID.String() + "?size=" + strconv.FormatInt(ref.Size, 10)
	req, _ := http.NewRequest("GET", url, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET artifact: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}

	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "image/png") {
		t.Fatalf("Content-Type = %q, want image/png", ct)
	}

	if cc := resp.Header.Get("Cache-Control"); !strings.Contains(cc, "immutable") {
		t.Fatalf("Cache-Control = %q, want immutable", cc)
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	if string(body) != string(pngData) {
		t.Fatalf("body mismatch: got %d bytes, want %d bytes", len(body), len(pngData))
	}
}

// TestHandleArtifactMissingSize verifies the endpoint rejects requests
// without the required size query parameter.
func TestHandleArtifactMissingSize(t *testing.T) {
	ts, artStore := newArtifactTestServer(t)
	ref, err := artStore.PutBytes(context.Background(), []byte("hello"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}

	url := ts.URL + "/v1/artifacts/" + ref.ID.String()
	req, _ := http.NewRequest("GET", url, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET artifact: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
}

// TestHandleArtifactInvalidID verifies the endpoint rejects malformed IDs.
func TestHandleArtifactInvalidID(t *testing.T) {
	ts, _ := newArtifactTestServer(t)

	url := ts.URL + "/v1/artifacts/not-a-valid-id?size=100"
	req, _ := http.NewRequest("GET", url, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET artifact: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
}

// TestHandleArtifactSizeMismatch verifies that a wrong size (hash
// verification failure) is rejected.
func TestHandleArtifactSizeMismatch(t *testing.T) {
	ts, artStore := newArtifactTestServer(t)
	ref, err := artStore.PutBytes(context.Background(), []byte("hello world"))
	if err != nil {
		t.Fatalf("PutBytes: %v", err)
	}

	// Use a wrong size to trigger hash verification failure.
	url := ts.URL + "/v1/artifacts/" + ref.ID.String() + "?size=999"
	req, _ := http.NewRequest("GET", url, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET artifact: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusOK {
		t.Fatalf("status = 200 with wrong size, want error")
	}
}

// TestHandleArtifactNotFound verifies that a well-formed reference whose
// blob is absent from the store maps to 404 (not a generic 5xx).
func TestHandleArtifactNotFound(t *testing.T) {
	ts, _ := newArtifactTestServer(t)

	// A syntactically valid reference (art_sha256_ + 64 lowercase hex
	// chars) that was never committed.
	url := ts.URL + "/v1/artifacts/art_sha256_" + strings.Repeat("0", 64) + "?size=10"
	req, _ := http.NewRequest("GET", url, nil)
	authed(t, req)
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("GET artifact: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", resp.StatusCode)
	}
}

// TestDetectArtifactContentType verifies content type sniffing for
// common image formats and the fallback to octet-stream.
func TestDetectArtifactContentType(t *testing.T) {
	tests := []struct {
		name string
		data []byte
		want string
	}{
		{
			"png",
			[]byte{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A},
			"image/png",
		},
		{
			"jpeg",
			[]byte{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46},
			"image/jpeg",
		},
		{
			"gif",
			[]byte("GIF89a"),
			"image/gif",
		},
		{
			"unknown binary",
			[]byte{0x00, 0x01, 0x02, 0x03},
			"application/octet-stream",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := detectArtifactContentType(tt.data)
			if got != tt.want {
				t.Errorf("detectArtifactContentType() = %q, want %q", got, tt.want)
			}
		})
	}
}
