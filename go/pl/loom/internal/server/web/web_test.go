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
		if !strings.Contains(string(body), `id="root"`) {
			t.Fatalf("index missing the SPA mount point")
		}
	})

	t.Run("assets carry ETag and revalidate", func(t *testing.T) {
		// favicon.svg 是构建产物中的稳定命名资产（JS/CSS 均带内容散列）。
		resp := get("/favicon.svg")
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("status = %d", resp.StatusCode)
		}
		etag := resp.Header.Get("ETag")
		if etag == "" {
			t.Fatalf("missing ETag")
		}

		resp2 := get("/favicon.svg", "If-None-Match", etag)
		resp2.Body.Close()
		if resp2.StatusCode != http.StatusNotModified {
			t.Fatalf("revalidation status = %d, want 304", resp2.StatusCode)
		}
		if len(body) == 0 {
			t.Fatalf("empty favicon.svg")
		}
	})

	t.Run("built js bundles are javascript", func(t *testing.T) {
		// Vite 输出文件名带内容散列，从嵌入资产表里发现一个 .js 路径。
		var jsPath string
		for p := range buildAssets() {
			if strings.HasSuffix(p, ".js") {
				jsPath = p
				break
			}
		}
		if jsPath == "" {
			t.Fatalf("no .js asset embedded (webui dist not built?)")
		}
		resp := get(jsPath)
		resp.Body.Close()
		if ct := resp.Header.Get("Content-Type"); !strings.Contains(ct, "javascript") {
			t.Fatalf("Content-Type = %q", ct)
		}
	})

	t.Run("share page embedded", func(t *testing.T) {
		resp := get("/share.html")
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Fatalf("GET /share.html status = %d", resp.StatusCode)
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
