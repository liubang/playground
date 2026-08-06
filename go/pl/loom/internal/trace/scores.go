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
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"
)

// basicAuthHeader builds the HTTP Basic authorization value Langfuse expects
// (base64 of "public:secret") for both OTLP and REST endpoints.
func basicAuthHeader(publicKey, secretKey string) string {
	return "Basic " + base64.StdEncoding.EncodeToString([]byte(publicKey+":"+secretKey))
}

// scoreRequest is the Langfuse scores API payload. Environment must match
// the trace's langfuse.environment or the score lands in the "default"
// environment and disappears from environment-filtered dashboards.
type scoreRequest struct {
	// ID, when set, makes ingestion idempotent: re-posting the same ID
	// overwrites the previous score instead of piling up duplicates
	// (user re-votes, retries).
	ID          string  `json:"id,omitempty"`
	TraceID     string  `json:"traceId"`
	Name        string  `json:"name"`
	Value       float64 `json:"value"`
	DataType    string  `json:"dataType,omitempty"`
	Comment     string  `json:"comment,omitempty"`
	Environment string  `json:"environment,omitempty"`
}

// scoreDataTypeBoolean marks 0/1 scores as BOOLEAN so Langfuse renders
// thumbs-style votes as true/false rather than a numeric axis.
const scoreDataTypeBoolean = "BOOLEAN"

// scoreClient posts numeric trace scores to Langfuse's scores API. All
// submissions are fire-and-forget: the client never blocks the caller for
// more than the submission timeout, and failures are logged, never returned.
type scoreClient struct {
	host      string
	basicAuth string
	env       string
	http      *http.Client
	logger    *slog.Logger
	// wg tracks in-flight submissions so Shutdown can wait for them; a
	// score queued right before process exit (the common case — runs are
	// scored at the end) must not be silently dropped.
	wg sync.WaitGroup
}

func newScoreClient(host, publicKey, secretKey, environment string, logger *slog.Logger) *scoreClient {
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	return &scoreClient{
		host:      host,
		basicAuth: basicAuthHeader(publicKey, secretKey),
		env:       environment,
		http:      &http.Client{Timeout: 5 * time.Second},
		logger:    logger,
	}
}

// submit queues a score report in a background goroutine. It always returns
// immediately; panics inside the goroutine are recovered and logged. Callers
// must not invoke submit concurrently with waitIdle (loom scores runs before
// shutdown starts, so this holds by construction).
func (c *scoreClient) submit(name, traceID string, value float64, comment string) {
	c.submitRequest(scoreRequest{TraceID: traceID, Name: name, Value: value, Comment: comment, Environment: c.env})
}

// submitFeedback queues a BOOLEAN user-feedback score with a deterministic
// ID derived from (traceID, name): a re-vote on the same run overwrites the
// previous score in place instead of accumulating duplicates.
func (c *scoreClient) submitFeedback(traceID, name string, value float64, comment string) {
	sum := sha256.Sum256([]byte(traceID + ":" + name))
	id := "loom-fb-" + hex.EncodeToString(sum[:])[:24]
	c.submitRequest(scoreRequest{
		ID: id, TraceID: traceID, Name: name, Value: value,
		DataType: scoreDataTypeBoolean, Comment: comment, Environment: c.env,
	})
}

func (c *scoreClient) submitRequest(req scoreRequest) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		defer func() {
			if r := recover(); r != nil {
				c.logger.Warn("langfuse score report panicked", "panic", r)
			}
		}()
		ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
		defer cancel()
		if err := c.post(ctx, req); err != nil {
			c.logger.Warn("langfuse score report failed", "score", req.Name, "error", err)
		}
	}()
}

// waitIdle blocks until in-flight submissions finish or the timeout elapses.
func (c *scoreClient) waitIdle(timeout time.Duration) {
	done := make(chan struct{})
	go func() {
		c.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(timeout):
		c.logger.Warn("langfuse score flush timed out; some scores may be lost", "timeout", timeout)
	}
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
