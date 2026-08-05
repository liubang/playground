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
// Created: 2026/08/05

package web

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestStaticServing(t *testing.T) {
	ts := httptest.NewServer(Handler())
	t.Cleanup(ts.Close)

	get := func(path string, headers ...string) *http.Response {
		t.Helper()
		req, err := http.NewRequest(http.MethodGet, ts.URL+path, nil)
		if err != nil {
			t.Fatalf("NewRequest: %v", err)
		}
		for i := 0; i+1 < len(headers); i += 2 {
			req.Header.Set(headers[i], headers[i+1])
		}
		resp, err := ts.Client().Do(req)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		return resp
	}

	t.Run("index at root is no-store html", func(t *testing.T) {
		resp := get("/")
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("status = %d", resp.StatusCode)
		}
		if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
			t.Fatalf("Content-Type = %q", ct)
		}
		if cc := resp.Header.Get("Cache-Control"); cc != "no-store" {
			t.Fatalf("Cache-Control = %q, want no-store", cc)
		}
		body, _ := io.ReadAll(resp.Body)
		if !strings.Contains(string(body), `id="gate"`) {
			t.Fatalf("index missing the token gate")
		}
	})

	t.Run("assets carry ETag and revalidate", func(t *testing.T) {
		resp := get("/app.css")
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("status = %d", resp.StatusCode)
		}
		if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/css") {
			t.Fatalf("Content-Type = %q", ct)
		}
		etag := resp.Header.Get("ETag")
		if etag == "" {
			t.Fatalf("missing ETag")
		}

		resp2 := get("/app.css", "If-None-Match", etag)
		resp2.Body.Close()
		if resp2.StatusCode != http.StatusNotModified {
			t.Fatalf("revalidation status = %d, want 304", resp2.StatusCode)
		}
		if len(body) == 0 {
			t.Fatalf("empty app.css")
		}
	})

	t.Run("js modules are javascript", func(t *testing.T) {
		resp := get("/js/main.js")
		resp.Body.Close()
		if ct := resp.Header.Get("Content-Type"); !strings.Contains(ct, "javascript") {
			t.Fatalf("Content-Type = %q", ct)
		}
	})

	t.Run("vendored deps embedded", func(t *testing.T) {
		for _, p := range []string{"/vendor/marked.esm.js", "/vendor/purify.es.mjs"} {
			resp := get(p)
			resp.Body.Close()
			if resp.StatusCode != http.StatusOK {
				t.Fatalf("GET %s status = %d", p, resp.StatusCode)
			}
		}
	})

	t.Run("unknown and traversal paths 404", func(t *testing.T) {
		for _, p := range []string{"/nope.js", "/../web.go", "/js/../../web.go"} {
			resp := get(p)
			resp.Body.Close()
			if resp.StatusCode != http.StatusNotFound {
				t.Fatalf("GET %s status = %d, want 404", p, resp.StatusCode)
			}
		}
	})
}
