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

package httpc

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func testClient(t *testing.T, maxRetries int) *Client {
	t.Helper()
	c, err := New(Config{
		MaxRetries:     maxRetries,
		InitialBackoff: time.Millisecond,
		MaxBackoff:     5 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return c
}

func TestNewRejectsNegativeRetries(t *testing.T) {
	if _, err := New(Config{MaxRetries: -1}); err == nil {
		t.Fatal("expected error for negative max retries")
	}
}

func TestPostSuccess(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("X-Test"); got != "yes" {
			t.Errorf("header X-Test = %q", got)
		}
		body, _ := io.ReadAll(r.Body)
		if string(body) != "payload" {
			t.Errorf("body = %q", body)
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: ok\n\n"))
	}))
	defer server.Close()

	resp, err := testClient(t, 0).Post(context.Background(), server.URL, []byte("payload"), http.Header{"X-Test": {"yes"}})
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	if err := RequireEventStream(resp); err != nil {
		t.Fatalf("RequireEventStream: %v", err)
	}
}

func TestPostRetriesRetryableStatus(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if calls.Add(1) < 3 {
			w.WriteHeader(http.StatusTooManyRequests)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: ok\n\n"))
	}))
	defer server.Close()

	resp, err := testClient(t, 5).Post(context.Background(), server.URL, nil, nil)
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	if calls.Load() != 3 {
		t.Fatalf("calls = %d, want 3", calls.Load())
	}
}

func TestPostDoesNotRetryClientError(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":{"message":"bad input","type":"invalid_request_error"}}`))
	}))
	defer server.Close()

	_, err := testClient(t, 5).Post(context.Background(), server.URL, nil, nil)
	if err == nil {
		t.Fatal("expected error")
	}
	if calls.Load() != 1 {
		t.Fatalf("calls = %d, want 1 (no retry on 400)", calls.Load())
	}
	se, ok := AsStatusError(err)
	if !ok {
		t.Fatalf("err is not a StatusError: %v", err)
	}
	if se.Code != http.StatusBadRequest || se.Message != "bad input" {
		t.Fatalf("StatusError = %+v", se)
	}
	if se.Retryable() {
		t.Fatal("400 must not be retryable")
	}
}

func TestPostHonorsRetryAfterWithinCap(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if calls.Add(1) == 1 {
			w.Header().Set("Retry-After", "1")
			w.WriteHeader(http.StatusTooManyRequests)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: ok\n\n"))
	}))
	defer server.Close()

	c, err := New(Config{
		MaxRetries:        2,
		InitialBackoff:    time.Millisecond,
		MaxBackoff:        5 * time.Millisecond,
		MaxRetryAfterWait: 2 * time.Second,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	started := time.Now()
	resp, err := c.Post(context.Background(), server.URL, nil, nil)
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	if elapsed := time.Since(started); elapsed < time.Second {
		t.Fatalf("returned after %s; Retry-After of 1s was not honored", elapsed)
	}
}

func TestPostFailsFastWhenRetryAfterExceedsCap(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.Header().Set("Retry-After", "120")
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer server.Close()

	_, err := testClient(t, 5).Post(context.Background(), server.URL, nil, nil)
	if err == nil {
		t.Fatal("expected error")
	}
	if calls.Load() != 1 {
		t.Fatalf("calls = %d, want 1 (fail fast beyond cap)", calls.Load())
	}
	se, ok := AsStatusError(err)
	if !ok || se.RetryAfter != 120*time.Second {
		t.Fatalf("StatusError = %+v (ok=%v)", se, ok)
	}
}

func TestPostRespectsContextCancel(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusTooManyRequests)
	}))
	defer server.Close()

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := testClient(t, 5).Post(ctx, server.URL, nil, nil)
	if err == nil {
		t.Fatal("expected cancellation error")
	}
}

func TestRequireEventStream(t *testing.T) {
	cases := []struct {
		contentType string
		wantErr     bool
	}{
		{"text/event-stream", false},
		{"text/event-stream; charset=utf-8", false},
		{"", true},
		{"application/json", true},
	}
	for _, tc := range cases {
		resp := &http.Response{Header: http.Header{}}
		if tc.contentType != "" {
			resp.Header.Set("Content-Type", tc.contentType)
		}
		err := RequireEventStream(resp)
		if (err != nil) != tc.wantErr {
			t.Errorf("RequireEventStream(%q) err = %v, wantErr %v", tc.contentType, err, tc.wantErr)
		}
	}
}

func TestParseRetryAfter(t *testing.T) {
	now := time.Date(2026, 7, 27, 12, 0, 0, 0, time.UTC)
	cases := []struct {
		value string
		want  time.Duration
	}{
		{"", 0},
		{"0", 0},
		{"-5", 0},
		{"3", 3 * time.Second},
		{"not-a-number", 0},
		{now.Add(2 * time.Second).UTC().Format(http.TimeFormat), 2 * time.Second},
		{now.Add(-time.Minute).UTC().Format(http.TimeFormat), 0},
		{now.Add(2 * time.Hour).UTC().Format(http.TimeFormat), maxRetryAfterDateSkew},
	}
	for _, tc := range cases {
		if got := parseRetryAfter(tc.value, now); got != tc.want {
			t.Errorf("parseRetryAfter(%q) = %s, want %s", tc.value, got, tc.want)
		}
	}
}

func TestStatusErrorMessageFallbackToBody(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte("upstream exploded"))
	}))
	defer server.Close()

	_, err := testClient(t, 0).Post(context.Background(), server.URL, nil, nil)
	se, ok := AsStatusError(err)
	if !ok {
		t.Fatalf("err is not a StatusError: %v", err)
	}
	if !strings.Contains(se.Error(), "upstream exploded") {
		t.Fatalf("StatusError.Error() = %q", se.Error())
	}
}

func TestToDomainErrorClassifiesFailures(t *testing.T) {
	statusErr := func(code int) error {
		return &StatusError{Code: code, Status: fmt.Sprintf("%d status", code), Message: "provider says no"}
	}
	cases := []struct {
		name      string
		err       error
		code      domain.ErrorCode
		retryable bool
	}{
		{"429 is rate limited and retryable", statusErr(http.StatusTooManyRequests), domain.ErrRateLimited, true},
		{"401 is permission", statusErr(http.StatusUnauthorized), domain.ErrPermission, false},
		{"403 is permission", statusErr(http.StatusForbidden), domain.ErrPermission, false},
		{"400 is invalid input", statusErr(http.StatusBadRequest), domain.ErrInvalidInput, false},
		{"408 is unavailable and retryable", statusErr(http.StatusRequestTimeout), domain.ErrUnavailable, true},
		{"500 is unavailable and retryable", statusErr(http.StatusInternalServerError), domain.ErrUnavailable, true},
		{"418 is unavailable and not retryable", statusErr(http.StatusTeapot), domain.ErrUnavailable, false},
		{"transport error is retryable", errors.New("connection reset by peer"), domain.ErrUnavailable, true},
		{
			"wrapped status error keeps its class",
			fmt.Errorf("anthropic provider: %w", statusErr(http.StatusTooManyRequests)), domain.ErrRateLimited, true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := ToDomainError("test provider", tc.err)
			var ae *domain.AgentError
			if !errors.As(got, &ae) {
				t.Fatalf("ToDomainError() = %v, want *domain.AgentError", got)
			}
			if ae.Code != tc.code || ae.Retryable != tc.retryable {
				t.Fatalf("ToDomainError() = [%s] retryable=%v, want [%s] retryable=%v",
					ae.Code, ae.Retryable, tc.code, tc.retryable)
			}
			if !strings.Contains(ae.Error(), "test provider") {
				t.Fatalf("error text %q misses the provider prefix", ae.Error())
			}
			if !errors.Is(got, tc.err) && !errors.Is(ae.Unwrap(), tc.err) {
				t.Fatalf("cause chain lost the original error: %v", got)
			}
		})
	}
}

func TestToDomainErrorPassesContextSentinelsThrough(t *testing.T) {
	if got := ToDomainError("p", nil); got != nil {
		t.Fatalf("ToDomainError(nil) = %v, want nil", got)
	}
	for _, sentinel := range []error{context.Canceled, context.DeadlineExceeded} {
		got := ToDomainError("p", fmt.Errorf("wrap: %w", sentinel))
		if !errors.Is(got, sentinel) {
			t.Fatalf("ToDomainError(%v) = %v, want the sentinel preserved", sentinel, got)
		}
		var ae *domain.AgentError
		if errors.As(got, &ae) {
			t.Fatalf("ToDomainError(%v) must not wrap the sentinel into AgentError", sentinel)
		}
	}
}

func TestToDomainErrorKeepsStatusErrorDiscoverable(t *testing.T) {
	// The 413 detection in the agent loop unwraps StatusError through the
	// domain error's cause chain; the mapping must not hide it.
	orig := &StatusError{Code: http.StatusRequestEntityTooLarge, Status: "413 Request Entity Too Large"}
	got := ToDomainError("p", orig)
	se, ok := AsStatusError(got)
	if !ok || se.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("AsStatusError(ToDomainError()) = %v, %v; want the 413 StatusError", se, ok)
	}
}
