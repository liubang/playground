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

package client

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func artifactRef(t *testing.T, rawID string) domain.ArtifactRef {
	t.Helper()
	id, err := domain.ParseArtifactID(rawID)
	if err != nil {
		t.Fatalf("artifact id %q: %v", rawID, err)
	}
	return domain.ArtifactRef{ID: id, Size: int64(len("png-bytes"))}
}

// TestHTTPReadArtifactRoundTrip verifies the http transport's ReadArtifact:
// path carries the content-derived id, the size query is forwarded (the
// server needs it for hash verification), and raw bytes come back untouched.
func TestHTTPReadArtifactRoundTrip(t *testing.T) {
	var sawPath string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sawPath = r.URL.String()
		if r.Header.Get("Authorization") != "Bearer tok" {
			t.Errorf("missing bearer token")
		}
		w.Header().Set("Content-Type", "image/png")
		_, _ = w.Write([]byte("png-bytes"))
	}))
	defer ts.Close()

	c := NewHTTP(ts.URL, "tok").(*httpClient)
	ref := artifactRef(t, "art_sha256_"+strings.Repeat("ab", 32))
	data, err := c.ReadArtifact(context.Background(), ref)
	if err != nil {
		t.Fatalf("ReadArtifact: %v", err)
	}
	if string(data) != "png-bytes" {
		t.Fatalf("bytes = %q, want png-bytes", data)
	}
	if want := "/v1/artifacts/" + ref.ID.String() + "?size=9"; sawPath != want {
		t.Fatalf("request path = %q, want %q", sawPath, want)
	}
}

// TestHTTPReadArtifactErrorMapping verifies non-2xx responses surface as
// errors rather than empty bytes.
func TestHTTPReadArtifactErrorMapping(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"error":{"code":"not_found","message":"nope"}}`))
	}))
	defer ts.Close()

	c := NewHTTP(ts.URL, "tok").(*httpClient)
	if _, err := c.ReadArtifact(context.Background(), artifactRef(t, "art_sha256_"+strings.Repeat("cd", 32))); err == nil {
		t.Fatal("expected error for 404 response")
	}
}
