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

package images

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// pngPixel is a minimal valid PNG (1x1) used as the fake image payload.
var pngPixel = []byte{
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
	0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
	0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
	0x54, 0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00,
	0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
	0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
	0x42, 0x60, 0x82,
}

func TestNewOpenAIValidatesConfig(t *testing.T) {
	t.Parallel()
	if _, err := NewOpenAI(Config{}); err == nil {
		t.Fatal("expected error for empty base_url")
	}
	if _, err := NewOpenAI(Config{BaseURL: "not-a-url"}); err == nil {
		t.Fatal("expected error for invalid base_url")
	}
	if _, err := NewOpenAI(Config{BaseURL: "https://example.com/v1"}); err == nil {
		t.Fatal("expected error for missing api key")
	}
	if _, err := NewOpenAI(Config{BaseURL: "https://example.com/v1", APIKey: "k"}); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestGeneratePostsRequestAndParsesImage(t *testing.T) {
	t.Parallel()
	var sawRequest atomic.Bool

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sawRequest.Store(true)
		if got := r.Header.Get("Authorization"); got != "Bearer secret-key" {
			t.Fatalf("unexpected Authorization header: %q", got)
		}
		if got := r.URL.Path; got != "/v1/images/generations" {
			t.Fatalf("unexpected request path: %q", got)
		}
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Fatalf("ReadAll: %v", err)
		}
		if strings.Contains(string(body), "secret-key") {
			t.Fatal("API key leaked into request body")
		}
		var payload struct {
			Model   string `json:"model"`
			Prompt  string `json:"prompt"`
			Size    string `json:"size"`
			Quality string `json:"quality"`
		}
		if err := json.Unmarshal(body, &payload); err != nil {
			t.Fatalf("json.Unmarshal: %v", err)
		}
		if payload.Model != "gpt-image-2" || payload.Prompt != "a red fox" ||
			payload.Size != "1024x1024" || payload.Quality != "high" {
			t.Fatalf("unexpected payload: %+v", payload)
		}
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"created":1,"output_format":"png","data":[{"b64_json":%q,"revised_prompt":"a red fox in a field"}]}`,
			base64.StdEncoding.EncodeToString(pngPixel))
	}))
	defer server.Close()

	gen, err := NewOpenAI(Config{BaseURL: server.URL + "/v1", APIKey: "secret-key", Client: server.Client()})
	if err != nil {
		t.Fatalf("NewOpenAI: %v", err)
	}
	res, err := gen.Generate(context.Background(), GenerateRequest{
		Model:   "gpt-image-2",
		Prompt:  "a red fox",
		Size:    "1024x1024",
		Quality: "high",
	})
	if err != nil {
		t.Fatalf("Generate: %v", err)
	}
	if !sawRequest.Load() {
		t.Fatal("expected server to receive request")
	}
	if !strings.EqualFold(string(res.Data[:8]), string(pngPixel[:8])) {
		t.Fatal("decoded data does not look like the PNG payload")
	}
	if len(res.Data) != len(pngPixel) {
		t.Fatalf("unexpected data length: %d", len(res.Data))
	}
	if res.MediaType != "image/png" {
		t.Fatalf("unexpected media type: %q", res.MediaType)
	}
	if res.RevisedPrompt != "a red fox in a field" {
		t.Fatalf("unexpected revised prompt: %q", res.RevisedPrompt)
	}
}

func TestGenerateValidatesInput(t *testing.T) {
	t.Parallel()
	gen, err := NewOpenAI(Config{BaseURL: "https://example.com/v1", APIKey: "k"})
	if err != nil {
		t.Fatalf("NewOpenAI: %v", err)
	}
	if _, err := gen.Generate(context.Background(), GenerateRequest{Model: "m"}); err == nil {
		t.Fatal("expected error for empty prompt")
	}
	if _, err := gen.Generate(context.Background(), GenerateRequest{Prompt: "p"}); err == nil {
		t.Fatal("expected error for empty model")
	}
}

func TestGenerateMapsErrorStatuses(t *testing.T) {
	t.Parallel()
	cases := []struct {
		status      int
		wantCode    domain.ErrorCode
		wantRetries bool
	}{
		{http.StatusTooManyRequests, domain.ErrRateLimited, true},
		{http.StatusUnauthorized, domain.ErrPermission, false},
		{http.StatusBadRequest, domain.ErrInvalidInput, false},
		{http.StatusInternalServerError, domain.ErrUnavailable, true},
		{http.StatusBadGateway, domain.ErrUnavailable, true},
	}
	for _, tc := range cases {
		t.Run(fmt.Sprintf("status_%d", tc.status), func(t *testing.T) {
			t.Parallel()
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(tc.status)
				fmt.Fprint(w, `{"error":{"message":"boom","type":"server_error"}}`)
			}))
			defer server.Close()

			gen, err := NewOpenAI(Config{BaseURL: server.URL, APIKey: "k", Client: server.Client()})
			if err != nil {
				t.Fatalf("NewOpenAI: %v", err)
			}
			_, err = gen.Generate(context.Background(), GenerateRequest{Model: "m", Prompt: "p"})
			if err == nil {
				t.Fatal("expected error")
			}
			var agentErr *domain.AgentError
			if !errors.As(err, &agentErr) {
				t.Fatalf("expected AgentError, got %T: %v", err, err)
			}
			if agentErr.Code != tc.wantCode {
				t.Fatalf("code = %q, want %q", agentErr.Code, tc.wantCode)
			}
			if agentErr.Retryable != tc.wantRetries {
				t.Fatalf("retryable = %v, want %v", agentErr.Retryable, tc.wantRetries)
			}
			if !strings.Contains(agentErr.Message, "boom") {
				t.Fatalf("provider error message not surfaced: %q", agentErr.Message)
			}
		})
	}
}

func TestGenerateRejectsEmptyData(t *testing.T) {
	t.Parallel()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprint(w, `{"created":1}`)
	}))
	defer server.Close()

	gen, err := NewOpenAI(Config{BaseURL: server.URL, APIKey: "k", Client: server.Client()})
	if err != nil {
		t.Fatalf("NewOpenAI: %v", err)
	}
	if _, err := gen.Generate(context.Background(), GenerateRequest{Model: "m", Prompt: "p"}); err == nil {
		t.Fatal("expected error for response without image data")
	}
}

func TestDetectMediaType(t *testing.T) {
	t.Parallel()
	if got := detectMediaType(pngPixel, ""); got != "image/png" {
		t.Fatalf("png sniff: %q", got)
	}
	if got := detectMediaType([]byte{0xFF, 0xD8, 0xFF, 0x00}, ""); got != "image/jpeg" {
		t.Fatalf("jpeg sniff: %q", got)
	}
	if got := detectMediaType([]byte("nonsense"), "webp"); got != "image/webp" {
		t.Fatalf("output_format fallback: %q", got)
	}
	if got := detectMediaType([]byte("nonsense"), ""); got != "image/png" {
		t.Fatalf("default fallback: %q", got)
	}
}
