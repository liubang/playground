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
// Created: 2026/08/17

package kbsearch

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// maxResponseBodyBytes bounds one minisearch response body; a healthy
// search/read response is far below this, so a larger body means a
// misbehaving (or spoofed) endpoint.
const maxResponseBodyBytes = 4 << 20

// Client is a thin read-only client for the minisearch v2 REST API
// (POST /api/v2/{col}/search, GET /api/v2/{col}/documents/{id}). The base
// URL comes from operator configuration, so requests always carry the
// configured Bearer credential to a fixed, trusted endpoint.
type Client struct {
	baseURL string
	apiKey  string
	http    *http.Client
}

func newClient(baseURL, apiKey string, timeout time.Duration) *Client {
	return &Client{
		baseURL: strings.TrimRight(baseURL, "/"),
		apiKey:  apiKey,
		http:    &http.Client{Timeout: timeout},
	}
}

// Hit is one search result with its typed field values unwrapped
// ({"s": "..."} / {"n": 1.5} → plain values). Vector fields are dropped:
// embeddings are meaningless to the model and dominate the token budget.
type Hit struct {
	ID     string
	Score  float64
	Fields map[string]any
}

// wireFieldValue mirrors minisearch's FieldValue oneof in JSON form.
// The vector variant ("v") is deliberately not declared: unknown keys are
// ignored on decode, which drops embeddings from every response for free.
type wireFieldValue struct {
	S *string  `json:"s,omitempty"`
	N *float64 `json:"n,omitempty"`
}

type wireDocument struct {
	ID     string                    `json:"id"`
	Fields map[string]wireFieldValue `json:"fields"`
}

type wireHit struct {
	ID       string       `json:"id"`
	Score    float64      `json:"score"`
	Document wireDocument `json:"document"`
}

type wireSearchResponse struct {
	Hits  []wireHit `json:"hits"`
	Error string    `json:"error"`
}

type wireGetDocumentResponse struct {
	Found    bool         `json:"found"`
	Document wireDocument `json:"document"`
}

// Search runs a hybrid (BM25 + vector, RRF-fused) query against one
// collection. Weights/rerank stay server-side collection settings: they
// are tuning knobs for the operator, not decisions for the model.
func (c *Client) Search(ctx context.Context, collection, text string, topK int) ([]Hit, error) {
	body, err := json.Marshal(struct {
		Text string `json:"text"`
		TopK int    `json:"top_k"`
	}{Text: text, TopK: topK})
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to encode search request", domain.WithCause(err))
	}
	resp, err := c.do(ctx, http.MethodPost, "/api/v2/"+escapeSegment(collection)+"/search", body)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("minisearch search: unexpected status %s", resp.Status)
	}
	var decoded wireSearchResponse
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxResponseBodyBytes)).Decode(&decoded); err != nil {
		return nil, fmt.Errorf("minisearch search: decode response: %w", err)
	}
	if decoded.Error != "" {
		return nil, fmt.Errorf("minisearch search: %s", decoded.Error)
	}
	hits := make([]Hit, 0, len(decoded.Hits))
	for _, h := range decoded.Hits {
		hits = append(hits, Hit{ID: h.ID, Score: h.Score, Fields: unwrapFields(h.Document.Fields)})
	}
	return hits, nil
}

// GetDocument fetches one document by id. found=false means the document
// (or collection) does not exist; transport/5xx failures are errors.
func (c *Client) GetDocument(ctx context.Context, collection, id string) (fields map[string]any, found bool, err error) {
	resp, err := c.do(ctx, http.MethodGet, "/api/v2/"+escapeSegment(collection)+"/documents/"+escapeDocID(id), nil)
	if err != nil {
		return nil, false, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, false, nil
	}
	if resp.StatusCode != http.StatusOK {
		return nil, false, fmt.Errorf("minisearch get document: unexpected status %s", resp.Status)
	}
	var decoded wireGetDocumentResponse
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxResponseBodyBytes)).Decode(&decoded); err != nil {
		return nil, false, fmt.Errorf("minisearch get document: decode response: %w", err)
	}
	if !decoded.Found {
		return nil, false, nil
	}
	return unwrapFields(decoded.Document.Fields), true, nil
}

func (c *Client) do(ctx context.Context, method, path string, body []byte) (*http.Response, error) {
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.baseURL+path, reader)
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to build minisearch request", domain.WithCause(err))
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+c.apiKey)
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("minisearch request: %w", err)
	}
	return resp, nil
}

// unwrapFields converts minisearch typed field values to plain JSON
// values, dropping fields whose value is neither string nor numeric
// (embeddings).
func unwrapFields(in map[string]wireFieldValue) map[string]any {
	out := make(map[string]any, len(in))
	for name, v := range in {
		switch {
		case v.S != nil:
			out[name] = *v.S
		case v.N != nil:
			out[name] = *v.N
		}
	}
	return out
}

// escapeSegment escapes one path segment (collection name).
func escapeSegment(s string) string {
	return url.PathEscape(s)
}

// escapeDocID escapes a document id for use as the path tail. Ids may
// legitimately contain "/" (the server treats the whole tail as the id),
// so escaping is per segment; "#"/"?" and friends are always escaped.
func escapeDocID(id string) string {
	segments := strings.Split(id, "/")
	for i, seg := range segments {
		segments[i] = url.PathEscape(seg)
	}
	return strings.Join(segments, "/")
}
