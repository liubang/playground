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
// Created: 2026/07/27

// Package httpc is the shared HTTP plumbing for streaming model providers.
// It wraps net/http with the retry discipline streaming LLM APIs demand:
// retries happen only while establishing the stream — once a 2xx response
// arrives the caller owns the body and no replay is possible. Backoff is
// exponential with full jitter, and a Retry-After hint on 429/503 is
// honored up to a bounded wait.
package httpc

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/rand/v2"
	"mime"
	"net/http"
	"strconv"
	"strings"
	"time"
)

const (
	defaultInitialBackoff = 200 * time.Millisecond
	defaultMaxBackoff     = 2 * time.Second
	defaultMaxRetryAfter  = 30 * time.Second
	statusBodyLimit       = 4096
	retryAfterHeader      = "Retry-After"
	maxRetryAfterDateSkew = time.Hour
)

// Config controls a Client's retry behavior.
type Config struct {
	// HTTPClient is the transport; nil selects http.DefaultClient.
	HTTPClient *http.Client
	// MaxRetries bounds retry attempts after the initial try (0 = none).
	// Negative values are rejected.
	MaxRetries int
	// InitialBackoff seeds the exponential backoff (default 200ms).
	InitialBackoff time.Duration
	// MaxBackoff caps the exponential backoff (default 2s).
	MaxBackoff time.Duration
	// MaxRetryAfterWait caps how long a Retry-After hint is honored; a hint
	// beyond this makes the status error final instead of retryable
	// (default 30s).
	MaxRetryAfterWait time.Duration
}

// Client issues streaming POSTs with bounded establishment-phase retries.
type Client struct {
	httpClient     *http.Client
	maxRetries     int
	initialBackoff time.Duration
	maxBackoff     time.Duration
	maxRetryAfter  time.Duration
}

// New builds a Client, validating the retry configuration.
func New(cfg Config) (*Client, error) {
	if cfg.MaxRetries < 0 {
		return nil, fmt.Errorf("httpc: max retries must be >= 0")
	}
	initial := cfg.InitialBackoff
	if initial <= 0 {
		initial = defaultInitialBackoff
	}
	maxBackoff := cfg.MaxBackoff
	if maxBackoff <= 0 {
		maxBackoff = defaultMaxBackoff
	}
	if maxBackoff < initial {
		maxBackoff = initial
	}
	maxRetryAfter := cfg.MaxRetryAfterWait
	if maxRetryAfter <= 0 {
		maxRetryAfter = defaultMaxRetryAfter
	}
	httpClient := cfg.HTTPClient
	if httpClient == nil {
		httpClient = http.DefaultClient
	}
	return &Client{
		httpClient:     httpClient,
		maxRetries:     cfg.MaxRetries,
		initialBackoff: initial,
		maxBackoff:     maxBackoff,
		maxRetryAfter:  maxRetryAfter,
	}, nil
}

// Post issues a POST expecting a 2xx streaming response; on success the
// caller owns resp.Body. Retries cover only the establishment phase:
// transport errors and retryable statuses (429, 5xx) before any body byte
// is consumed. A Retry-After hint within MaxRetryAfterWait replaces the
// computed backoff for that attempt; a larger hint fails fast with the
// StatusError.
func (c *Client) Post(ctx context.Context, url string, body []byte, headers http.Header) (*http.Response, error) {
	var lastErr error
	for attempt := 0; attempt <= c.maxRetries; attempt++ {
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
		if err != nil {
			return nil, fmt.Errorf("httpc: create request: %w", err)
		}
		for key, values := range headers {
			for _, value := range values {
				req.Header.Add(key, value)
			}
		}

		resp, err := c.httpClient.Do(req)
		if err != nil {
			if !retryableTransportError(ctx, err) || attempt == c.maxRetries {
				return nil, fmt.Errorf("httpc: request failed: %w", err)
			}
			lastErr = err
			if err := sleepContext(ctx, c.jitteredBackoff(attempt)); err != nil {
				return nil, err
			}
			continue
		}

		if !retryableStatus(resp.StatusCode) || attempt == c.maxRetries {
			if resp.StatusCode < 200 || resp.StatusCode >= 300 {
				statusErr := readStatusError(resp)
				_ = resp.Body.Close()
				if lastErr != nil {
					return nil, fmt.Errorf("httpc: request failed after retry: %w", statusErr)
				}
				return nil, statusErr
			}
			return resp, nil
		}

		retryAfter := parseRetryAfter(resp.Header.Get(retryAfterHeader), time.Now())
		_ = drainAndClose(resp.Body)
		if retryAfter > c.maxRetryAfter {
			return nil, &StatusError{
				Code:       resp.StatusCode,
				Status:     resp.Status,
				Message:    fmt.Sprintf("server asked to retry after %s (beyond the %s cap)", retryAfter, c.maxRetryAfter),
				RetryAfter: retryAfter,
			}
		}
		wait := c.jitteredBackoff(attempt)
		if retryAfter > wait {
			wait = retryAfter
		}
		if err := sleepContext(ctx, wait); err != nil {
			return nil, err
		}
	}

	if lastErr != nil {
		return nil, fmt.Errorf("httpc: request failed after retry: %w", lastErr)
	}
	return nil, fmt.Errorf("httpc: request failed")
}

// jitteredBackoff returns a random wait in [0, min(cap, base * 2^attempt)].
// Full jitter keeps concurrently throttled clients from retrying in lockstep.
func (c *Client) jitteredBackoff(attempt int) time.Duration {
	backoff := c.initialBackoff
	for i := 0; i < attempt; i++ {
		backoff *= 2
		if backoff >= c.maxBackoff {
			backoff = c.maxBackoff
			break
		}
	}
	if backoff > c.maxBackoff {
		backoff = c.maxBackoff
	}
	return time.Duration(rand.Float64() * float64(backoff))
}

// StatusError is a non-2xx provider response. Message carries the
// provider-supplied error text when one could be extracted; RetryAfter is
// the parsed Retry-After hint (0 when absent or unparsable).
type StatusError struct {
	Code       int
	Status     string
	Message    string
	RetryAfter time.Duration
}

func (e *StatusError) Error() string {
	if e.Message == "" {
		return fmt.Sprintf("HTTP %d %s", e.Code, e.Status)
	}
	return fmt.Sprintf("HTTP %d %s: %s", e.Code, e.Status, e.Message)
}

// Retryable reports whether the status is worth an establishment-phase
// retry (rate limiting or server-side failure).
func (e *StatusError) Retryable() bool {
	return retryableStatus(e.Code)
}

// AsStatusError unwraps err into a StatusError, if it is one.
func AsStatusError(err error) (*StatusError, bool) {
	var se *StatusError
	if errors.As(err, &se) {
		return se, true
	}
	return nil, false
}

// RequireEventStream verifies that resp carries an SSE body.
func RequireEventStream(resp *http.Response) error {
	contentType := resp.Header.Get("Content-Type")
	if contentType == "" {
		return fmt.Errorf("httpc: missing Content-Type")
	}
	mediaType, _, err := mime.ParseMediaType(contentType)
	if err != nil {
		return fmt.Errorf("httpc: invalid Content-Type %q: %w", contentType, err)
	}
	if mediaType != "text/event-stream" {
		return fmt.Errorf("httpc: unexpected Content-Type %q", contentType)
	}
	return nil
}

// readStatusError builds a StatusError from a failed response, extracting
// the provider message from the ubiquitous {"error":{"message":...}}
// envelope (OpenAI, Anthropic, and most compatible gateways share it) and
// falling back to the truncated raw body.
func readStatusError(resp *http.Response) error {
	defer resp.Body.Close()

	body, _ := io.ReadAll(io.LimitReader(resp.Body, statusBodyLimit))
	message := strings.TrimSpace(string(body))

	var envelope struct {
		Error struct {
			Message string `json:"message"`
			Type    string `json:"type"`
			Code    any    `json:"code"`
		} `json:"error"`
	}
	if len(body) > 0 && json.Unmarshal(body, &envelope) == nil && envelope.Error.Message != "" {
		message = envelope.Error.Message
	}

	return &StatusError{
		Code:       resp.StatusCode,
		Status:     resp.Status,
		Message:    message,
		RetryAfter: parseRetryAfter(resp.Header.Get(retryAfterHeader), time.Now()),
	}
}

// parseRetryAfter interprets the header as either delta-seconds or an
// HTTP-date. It returns 0 when the hint is absent, unparsable, or in the
// past, and caps absurd date-based hints to keep waits bounded.
func parseRetryAfter(value string, now time.Time) time.Duration {
	value = strings.TrimSpace(value)
	if value == "" {
		return 0
	}
	if seconds, err := strconv.Atoi(value); err == nil {
		if seconds <= 0 {
			return 0
		}
		return time.Duration(seconds) * time.Second
	}
	if at, err := http.ParseTime(value); err == nil {
		d := at.Sub(now)
		if d <= 0 {
			return 0
		}
		if d > maxRetryAfterDateSkew {
			return maxRetryAfterDateSkew
		}
		return d
	}
	return 0
}

func retryableStatus(code int) bool {
	return code == http.StatusTooManyRequests || code >= 500
}

// retryableTransportError reports whether a transport-layer failure is
// worth retrying; context cancellation and deadlines are never retried.
func retryableTransportError(ctx context.Context, err error) bool {
	if err == nil {
		return false
	}
	if ctx.Err() != nil {
		return false
	}
	return !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded)
}

func sleepContext(ctx context.Context, d time.Duration) error {
	timer := time.NewTimer(d)
	defer timer.Stop()

	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

func drainAndClose(body io.ReadCloser) error {
	_, _ = io.Copy(io.Discard, io.LimitReader(body, statusBodyLimit))
	return body.Close()
}
