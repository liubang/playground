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
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"time"
)

// basicAuthHeader builds the HTTP Basic authorization value Langfuse expects
// (base64 of "public:secret") for both OTLP and REST endpoints.
func basicAuthHeader(publicKey, secretKey string) string {
	return "Basic " + base64.StdEncoding.EncodeToString([]byte(publicKey+":"+secretKey))
}

// scoreRequest is the Langfuse scores API payload.
type scoreRequest struct {
	TraceID string  `json:"traceId"`
	Name    string  `json:"name"`
	Value   float64 `json:"value"`
	Comment string  `json:"comment,omitempty"`
}

// scoreClient posts numeric trace scores to Langfuse's scores API. All
// submissions are fire-and-forget: the client never blocks the caller for
// more than the submission timeout, and failures are logged, never returned.
type scoreClient struct {
	host      string
	basicAuth string
	http      *http.Client
	logger    *slog.Logger
}

func newScoreClient(host, publicKey, secretKey string) *scoreClient {
	return &scoreClient{
		host:      host,
		basicAuth: basicAuthHeader(publicKey, secretKey),
		http:      &http.Client{Timeout: 5 * time.Second},
		logger:    slog.Default(),
	}
}

// submit queues a score report in a background goroutine. It always returns
// immediately; panics inside the goroutine are recovered and logged.
func (c *scoreClient) submit(name, traceID string, value float64, comment string) {
	go func() {
		defer func() {
			if r := recover(); r != nil {
				c.logger.Warn("langfuse score report panicked", "panic", r)
			}
		}()
		ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
		defer cancel()
		if err := c.post(ctx, scoreRequest{TraceID: traceID, Name: name, Value: value, Comment: comment}); err != nil {
			c.logger.Warn("langfuse score report failed", "score", name, "error", err)
		}
	}()
}

func (c *scoreClient) post(ctx context.Context, score scoreRequest) error {
	body, err := json.Marshal(score)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.host+"/api/public/scores", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", c.basicAuth)
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("scores API returned %s", resp.Status)
	}
	return nil
}
