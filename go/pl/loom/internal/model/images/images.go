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

// Package images implements image generation against the OpenAI Images API
// (POST {base_url}/images/generations). It is deliberately separate from the
// chat model providers: text-to-image is a request/response capability
// invoked by the generate_image tool, not a streaming chat concern, so it
// must not leak into domain.ModelRequest / domain.ModelEvent.
package images

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	// defaultTimeout bounds one generation call. Image models are slow
	// (tens of seconds), so this is far looser than the chat defaults.
	defaultTimeout = 180 * time.Second
	// maxResponseBytes bounds the wire body (base64 inflates raw bytes by
	// 4/3, so 64MB on the wire covers ~48MB of image data).
	maxResponseBytes = 64 << 20
	// MaxDecodedBytes bounds the decoded image payload.
	MaxDecodedBytes = 32 << 20
)

// validSizes/validQualities are the OpenAI Images API vocabulary — the
// single home for both config validation and the generate_image tool.
var (
	validSizes     = map[string]bool{"auto": true, "1024x1024": true, "1536x1024": true, "1024x1536": true}
	validQualities = map[string]bool{"auto": true, "low": true, "medium": true, "high": true}
)

// ValidSize reports whether s is a supported size value.
func ValidSize(s string) bool { return validSizes[s] }

// ValidQuality reports whether s is a supported quality value.
func ValidQuality(s string) bool { return validQualities[s] }

// Generator produces one image from a text prompt. Implementations must be
// safe for concurrent use.
type Generator interface {
	Generate(ctx context.Context, req GenerateRequest) (GenerateResult, error)
}

// GenerateRequest is one text-to-image call. Size and Quality use the
// OpenAI Images API vocabulary ("auto", "1024x1024", "low"/"medium"/"high",
// ...); empty means the server default.
type GenerateRequest struct {
	Model   string
	Prompt  string
	Size    string
	Quality string
}

// GenerateResult carries the decoded image bytes plus metadata.
type GenerateResult struct {
	Data      []byte
	MediaType string
	// RevisedPrompt is the provider's rewritten prompt when reported
	// (OpenAI gpt-image models may revise for safety/quality).
	RevisedPrompt string
}

// Config configures the OpenAI Images API client.
type Config struct {
	// BaseURL is the provider root (e.g. "https://api.openai.com/v1");
	// required, mirroring the chat provider rule that an empty base URL
	// must never silently fall back to a foreign host.
	BaseURL string
	// APIKey is the resolved secret (Bearer credential); required.
	APIKey string
	// Timeout bounds one generation call; zero selects defaultTimeout.
	Timeout time.Duration
	// Client overrides the HTTP client (tests); nil builds a default one.
	Client *http.Client
}

// OpenAI is a Generator against OpenAI-compatible Images API endpoints.
type OpenAI struct {
	baseURL string
	apiKey  string
	client  *http.Client
}

// NewOpenAI validates the config and returns the client.
func NewOpenAI(cfg Config) (*OpenAI, error) {
	base := strings.TrimRight(strings.TrimSpace(cfg.BaseURL), "/")
	if base == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "images provider: base_url is required")
	}
	u, err := url.Parse(base)
	if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("images provider: base_url must be an absolute http(s) URL, got %q", cfg.BaseURL))
	}
	if cfg.APIKey == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "images provider: api key is required")
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = defaultTimeout
	}
	client := cfg.Client
	if client == nil {
		client = &http.Client{Timeout: timeout}
	}
	return &OpenAI{baseURL: base, apiKey: cfg.APIKey, client: client}, nil
}

// wire request/response shapes (OpenAI Images API).
type generateWireRequest struct {
	Model   string `json:"model"`
	Prompt  string `json:"prompt"`
	Size    string `json:"size,omitempty"`
	Quality string `json:"quality,omitempty"`
}

type generateWireResponse struct {
	Data []struct {
		B64JSON       string `json:"b64_json"`
		RevisedPrompt string `json:"revised_prompt"`
	} `json:"data"`
	OutputFormat string `json:"output_format"`
	Error        *struct {
		Message string `json:"message"`
		Type    string `json:"type"`
	} `json:"error"`
}

// Generate issues one images/generations call and decodes the first image.
func (p *OpenAI) Generate(ctx context.Context, req GenerateRequest) (GenerateResult, error) {
	if strings.TrimSpace(req.Prompt) == "" {
		return GenerateResult{}, domain.NewError(domain.ErrInvalidInput, "images provider: prompt is required")
	}
	if req.Model == "" {
		return GenerateResult{}, domain.NewError(domain.ErrInvalidInput, "images provider: model is required")
	}

	body, err := json.Marshal(generateWireRequest{
		Model:   req.Model,
		Prompt:  req.Prompt,
		Size:    req.Size,
		Quality: req.Quality,
	})
	if err != nil {
		return GenerateResult{}, domain.NewError(domain.ErrInternal, "images provider: marshal request", domain.WithCause(err))
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, p.baseURL+"/images/generations", bytes.NewReader(body))
	if err != nil {
		return GenerateResult{}, domain.NewError(domain.ErrInternal, "images provider: build request", domain.WithCause(err))
	}
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("Authorization", "Bearer "+p.apiKey)

	resp, err := p.client.Do(httpReq)
	if err != nil {
		return GenerateResult{}, mapRequestError(err)
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes+1))
	if err != nil {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: read response", domain.WithCause(err), domain.WithRetryable(true))
	}
	if len(data) > maxResponseBytes {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: response exceeds size limit")
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return GenerateResult{}, statusError(resp.StatusCode, data)
	}

	var wire generateWireResponse
	if err := json.Unmarshal(data, &wire); err != nil {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: decode response", domain.WithCause(err))
	}
	if len(wire.Data) == 0 || wire.Data[0].B64JSON == "" {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: response carries no image data")
	}

	raw, err := base64.StdEncoding.DecodeString(wire.Data[0].B64JSON)
	if err != nil {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: image data is not valid base64", domain.WithCause(err))
	}
	if len(raw) == 0 {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable, "images provider: image data is empty")
	}
	if len(raw) > MaxDecodedBytes {
		return GenerateResult{}, domain.NewError(domain.ErrUnavailable,
			fmt.Sprintf("images provider: image is %d bytes, exceeding the %d byte limit", len(raw), MaxDecodedBytes))
	}

	return GenerateResult{
		Data:          raw,
		MediaType:     detectMediaType(raw, wire.OutputFormat),
		RevisedPrompt: wire.Data[0].RevisedPrompt,
	}, nil
}

// detectMediaType sniffs the image magic bytes, falling back to the
// provider-reported output format and finally to png.
func detectMediaType(data []byte, outputFormat string) string {
	switch {
	case len(data) >= 8 && string(data[:8]) == "\x89PNG\r\n\x1a\n":
		return "image/png"
	case len(data) >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF:
		return "image/jpeg"
	case len(data) >= 6 && (string(data[:6]) == "GIF87a" || string(data[:6]) == "GIF89a"):
		return "image/gif"
	case len(data) >= 12 && string(data[:4]) == "RIFF" && string(data[8:12]) == "WEBP":
		return "image/webp"
	}
	switch strings.ToLower(strings.TrimSpace(outputFormat)) {
	case "jpeg", "jpg":
		return "image/jpeg"
	case "webp":
		return "image/webp"
	}
	return "image/png"
}

// statusError maps a non-2xx response onto the domain error vocabulary,
// extracting the provider's error message when present. The body snippet is
// sanitized: it never contains the API key (the key travels in headers).
func statusError(status int, body []byte) error {
	message := ""
	var wire generateWireResponse
	if err := json.Unmarshal(body, &wire); err == nil && wire.Error != nil {
		message = wire.Error.Message
	}
	if message == "" {
		snippet := strings.TrimSpace(string(body))
		if len(snippet) > 200 {
			snippet = snippet[:200]
		}
		message = snippet
	}
	msg := fmt.Sprintf("images provider: request failed with status %d", status)
	if message != "" {
		msg += ": " + message
	}
	switch {
	case status == http.StatusTooManyRequests:
		return domain.NewError(domain.ErrRateLimited, msg, domain.WithRetryable(true))
	case status == http.StatusUnauthorized || status == http.StatusForbidden:
		return domain.NewError(domain.ErrPermission, msg)
	case status == http.StatusBadRequest:
		return domain.NewError(domain.ErrInvalidInput, msg)
	case status == http.StatusRequestTimeout || status >= 500:
		return domain.NewError(domain.ErrUnavailable, msg, domain.WithRetryable(true))
	default:
		return domain.NewError(domain.ErrUnavailable, msg)
	}
}

func mapRequestError(err error) error {
	switch {
	case errors.Is(err, context.DeadlineExceeded):
		return domain.NewError(domain.ErrTimeout, "images provider: request timed out", domain.WithRetryable(true), domain.WithCause(err))
	case errors.Is(err, context.Canceled):
		return err
	}
	var urlErr *url.Error
	if errors.As(err, &urlErr) && urlErr.Timeout() {
		return domain.NewError(domain.ErrTimeout, "images provider: request timed out", domain.WithRetryable(true), domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "images provider: request failed", domain.WithCause(err), domain.WithRetryable(true))
}
