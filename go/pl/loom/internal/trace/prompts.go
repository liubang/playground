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
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// ManagedPrompt is a system prompt managed in Langfuse Prompt Management.
// Version links generations to the exact prompt revision that produced them.
type ManagedPrompt struct {
	Name      string    `json:"name"`
	Version   int       `json:"version"`
	Content   string    `json:"content"`
	FetchedAt time.Time `json:"fetched_at"`
	// Label is the release label the prompt was fetched with (e.g.
	// "production"). The disk cache is keyed by (name, label): serving a
	// staging prompt for a production request after a network outage is
	// worse than falling back to the built-in prompt.
	Label string `json:"label"`
}

// promptAPIResponse mirrors the Langfuse prompt API response. The prompt
// field is polymorphic: a string for text prompts, a message array for chat
// prompts (flattened into Content either way).
type promptAPIResponse struct {
	Name    string          `json:"name"`
	Version int             `json:"version"`
	Prompt  json.RawMessage `json:"prompt"`
}

// PromptClient fetches managed prompts with a write-through disk cache.
// The network is tried first; any failure falls back to the last good fetch.
type PromptClient struct {
	host      string
	basicAuth string
	cacheDir  string
	http      *http.Client
	logger    *slog.Logger
}

// NewPromptClient builds a client for the Langfuse prompt API. cacheDir
// persists the last successful fetch per prompt name for offline fallback.
func NewPromptClient(cfg Config, cacheDir string) *PromptClient {
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	return &PromptClient{
		host:      cfg.Host,
		basicAuth: basicAuthHeader(cfg.PublicKey, cfg.SecretKey),
		cacheDir:  cacheDir,
		http:      &http.Client{Timeout: 10 * time.Second},
		logger:    logger,
	}
}

// Get resolves a managed prompt by name and label (e.g. "production").
// It returns an error only when neither the API nor a cache entry for this
// exact (name, label) pair can serve it.
func (c *PromptClient) Get(ctx context.Context, name, label string) (*ManagedPrompt, error) {
	prompt, err := c.fetch(ctx, name, label)
	if err == nil {
		if cacheErr := c.writeCache(prompt); cacheErr != nil {
			c.logger.Warn("langfuse prompt cache write failed", "name", name, "error", cacheErr)
		}
		return prompt, nil
	}
	cached, cacheErr := c.readCache(name, label)
	if cacheErr != nil {
		return nil, fmt.Errorf("fetch %q (label %q): %w (no usable cache: %v)", name, label, err, cacheErr)
	}
	c.logger.Warn("langfuse prompt fetch failed, using cached copy",
		"name", name, "label", label, "version", cached.Version, "error", err)
	return cached, nil
}

// fetch retrieves the prompt from the API.
func (c *PromptClient) fetch(ctx context.Context, name, label string) (*ManagedPrompt, error) {
	endpoint := fmt.Sprintf("%s/api/public/v2/prompts/%s?label=%s",
		c.host, url.PathEscape(name), url.QueryEscape(label))
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", c.basicAuth)
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("prompt API returned %s", resp.Status)
	}
	var decoded promptAPIResponse
	if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
		return nil, fmt.Errorf("decode prompt response: %w", err)
	}
	content, err := flattenPrompt(decoded.Prompt)
	if err != nil {
		return nil, fmt.Errorf("prompt %q: %w", name, err)
	}
	return &ManagedPrompt{
		Name:      decoded.Name,
		Version:   decoded.Version,
		Content:   content,
		FetchedAt: time.Now().UTC(),
		Label:     label,
	}, nil
}

// flattenPrompt renders a prompt API payload into plain text: text prompts
// pass through; chat prompts concatenate their messages with role prefixes.
func flattenPrompt(raw json.RawMessage) (string, error) {
	var text string
	if err := json.Unmarshal(raw, &text); err == nil {
		if strings.TrimSpace(text) == "" {
			return "", fmt.Errorf("prompt is empty")
		}
		return text, nil
	}
	var chat []struct {
		Role    string `json:"role"`
		Content string `json:"content"`
	}
	if err := json.Unmarshal(raw, &chat); err != nil {
		return "", fmt.Errorf("unsupported prompt payload: %w", err)
	}
	var b strings.Builder
	for _, msg := range chat {
		fmt.Fprintf(&b, "[%s]\n%s\n\n", msg.Role, msg.Content)
	}
	out := strings.TrimSpace(b.String())
	if out == "" {
		return "", fmt.Errorf("prompt is empty")
	}
	return out, nil
}

// cachePath keys the disk cache by (name, label): two labels of the same
// prompt are different releases and must never serve each other's content.
func (c *PromptClient) cachePath(name, label string) string {
	sanitize := func(s string) string {
		return strings.Map(func(r rune) rune {
			if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '-' || r == '_' {
				return r
			}
			return '_'
		}, s)
	}
	return filepath.Join(c.cacheDir, sanitize(name)+"."+sanitize(label)+".json")
}

func (c *PromptClient) writeCache(prompt *ManagedPrompt) error {
	if err := os.MkdirAll(c.cacheDir, 0o700); err != nil {
		return err
	}
	data, err := json.Marshal(prompt)
	if err != nil {
		return err
	}
	return os.WriteFile(c.cachePath(prompt.Name, prompt.Label), data, 0o600)
}

func (c *PromptClient) readCache(name, label string) (*ManagedPrompt, error) {
	data, err := os.ReadFile(c.cachePath(name, label))
	if err != nil {
		return nil, err
	}
	var prompt ManagedPrompt
	if err := json.Unmarshal(data, &prompt); err != nil {
		return nil, err
	}
	if strings.TrimSpace(prompt.Content) == "" {
		return nil, fmt.Errorf("cached prompt %q is empty", name)
	}
	// Defense in depth: even if the file was swapped or written by an older
	// loom, never serve a different label's content.
	if prompt.Label != "" && prompt.Label != label {
		return nil, fmt.Errorf("cached prompt %q is label %q, not %q", name, prompt.Label, label)
	}
	return &prompt, nil
}

// promptVariablePattern matches Langfuse template placeholders: {{name}}.
var promptVariablePattern = regexp.MustCompile(`\{\{\s*([a-zA-Z_][a-zA-Z0-9_.-]*)\s*\}\}`)

// PromptVariables returns the sorted, de-duplicated placeholder names found
// in a managed prompt. loom does not substitute variables — callers should
// surface them so a templated prompt never ships silently unrendered.
func PromptVariables(content string) []string {
	seen := map[string]struct{}{}
	for _, m := range promptVariablePattern.FindAllStringSubmatch(content, -1) {
		seen[m[1]] = struct{}{}
	}
	out := make([]string, 0, len(seen))
	for v := range seen {
		out = append(out, v)
	}
	sort.Strings(out)
	return out
}
