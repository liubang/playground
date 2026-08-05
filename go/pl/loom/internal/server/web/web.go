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

// Package web serves the embedded SPA (docs/WEB_DESIGN.md §7.1): static
// assets live in the loom binary via embed.FS, so `loom serve` is the only
// distribution artifact. Caching: index.html is no-store; asset bytes are
// content-hashed into strong ETags, so revalidation returns 304 after
// upgrades — no filename stamping build step required.
package web

import (
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"io/fs"
	"mime"
	"net/http"
	"path"
	"strings"
)

//go:embed static
var staticFS embed.FS

type asset struct {
	content []byte
	etag    string
	mime    string
}

// Handler returns the SPA static file handler for the site root.
func Handler() http.Handler {
	assets := map[string]asset{}
	root, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic(err) // embed layout is compile-time fixed
	}
	err = fs.WalkDir(root, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		content, err := fs.ReadFile(root, p)
		if err != nil {
			return err
		}
		sum := sha256.Sum256(content)
		mimeType := mime.TypeByExtension(path.Ext(p))
		if mimeType == "" {
			mimeType = "application/octet-stream"
		}
		assets["/"+p] = asset{content: content, etag: `"` + hex.EncodeToString(sum[:12]) + `"`, mime: mimeType}
		return nil
	})
	if err != nil {
		panic(err)
	}
	index := assets["/index.html"]

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/" || r.URL.Path == "/index.html" {
			serve(w, r, index, "no-store")
			return
		}
		// embed FS 内容固定，路径穿越查不到条目即 404。
		a, ok := assets[r.URL.Path]
		if !ok {
			http.NotFound(w, r)
			return
		}
		serve(w, r, a, "no-cache")
	})
}

func serve(w http.ResponseWriter, r *http.Request, a asset, cacheControl string) {
	header := w.Header()
	header.Set("Content-Type", a.mime)
	header.Set("ETag", a.etag)
	header.Set("Cache-Control", cacheControl)
	if match := r.Header.Get("If-None-Match"); match != "" && strings.Contains(match, a.etag) {
		w.WriteHeader(http.StatusNotModified)
		return
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(a.content)
}
