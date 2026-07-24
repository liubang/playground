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
// Created: 2026/07/24

// Package webfetch implements the web_fetch tool: read-only HTTP/HTTPS GET
// with SSRF protection, redirect bounding, size caps, HTML-to-markdown
// conversion, artifact overflow, and a short-lived response cache.
package webfetch

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	defaultTimeoutMs    = 20000
	maxTimeoutMs        = 60000
	minMaxBytes         = 1024
	defaultMaxBytes     = 256 << 10
	maxOutputBytes      = 1 << 20
	hardFetchByteCap    = 10 << 20
	maxRedirects        = 5
	dialTimeout         = 10 * time.Second
	userAgent           = "loom-webfetch/0.1 (+https://github.com/liubang/playground)"
	cacheTTL            = 15 * time.Minute
	cacheMaxEntries     = 64
	cacheMaxBodyBytes   = 1 << 20
	truncateBoundaryGap = 1024
)

var (
	errBlockedAddress   = errors.New("destination address blocked by network policy")
	errTooManyRedirects = errors.New("too many redirects")
	errRedirectScheme   = errors.New("redirect to unsupported scheme")
	errRedirectUserinfo = errors.New("redirect to URL with userinfo")
)

type fetchArgs struct {
	URL          string `json:"url"`
	Format       string `json:"format,omitempty"`
	MaxBytes     int    `json:"max_bytes,omitempty"`
	TimeoutMs    int    `json:"timeout_ms,omitempty"`
	AllowPrivate bool   `json:"allow_private,omitempty"`
}

type fetchOutput struct {
	URL         string              `json:"url"`
	FinalURL    string              `json:"final_url"`
	Status      int                 `json:"status"`
	ContentType string              `json:"content_type"`
	Format      string              `json:"format"`
	Bytes       int                 `json:"bytes"`
	Truncated   bool                `json:"truncated"`
	Cache       string              `json:"cache"`
	FetchedAt   time.Time           `json:"fetched_at"`
	Content     string              `json:"content"`
	Artifact    *domain.ArtifactRef `json:"artifact,omitempty"`
}

// WebFetchTool fetches web resources over HTTP(S) GET. A nil artifact store
// disables overflow externalization (truncated content is then lost).
type WebFetchTool struct {
	base      baseTool
	artifacts domain.ArtifactStore
	cache     *responseCache
	resolver  *net.Resolver
	now       func() time.Time
}

// NewWebFetchTool creates the web_fetch tool.
func NewWebFetchTool(artifacts domain.ArtifactStore) (*WebFetchTool, error) {
	base, err := newBaseTool(domain.ToolDefinition{
		Name: "web_fetch",
		Description: "Fetch a web page or text resource via HTTP/HTTPS GET and return its content. " +
			"HTML is converted to markdown by default; use format=text for plain text or format=raw for the untouched body. " +
			"Private/loopback/link-local destinations are blocked unless allow_private=true. " +
			"Successful responses are cached for 15 minutes; large content is truncated with the full text stored as an artifact.",
		InputSchema:  json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"url":{"type":"string","minLength":1,"maxLength":2048},"format":{"type":"string","enum":["markdown","text","raw"]},"max_bytes":{"type":"integer","minimum":1024,"maximum":1048576},"timeout_ms":{"type":"integer","minimum":1000,"maximum":60000},"allow_private":{"type":"boolean"}},"required":["url"]}`),
		OutputSchema: json.RawMessage(`{"type":"object","properties":{"url":{"type":"string"},"final_url":{"type":"string"},"status":{"type":"integer"},"content_type":{"type":"string"},"format":{"type":"string"},"bytes":{"type":"integer"},"truncated":{"type":"boolean"},"cache":{"type":"string"},"fetched_at":{"type":"string"},"content":{"type":"string"},"artifact":{"type":"object"}},"required":["url","final_url","status","content_type","format","bytes","truncated","cache","fetched_at","content"]}`),
		Capabilities: []domain.Capability{domain.CapNetworkConnect},
		Source:       domain.ToolSourceBuiltin,
	})
	if err != nil {
		return nil, err
	}
	return &WebFetchTool{
		base:      base,
		artifacts: artifacts,
		cache:     newResponseCache(cacheMaxEntries, cacheMaxBodyBytes, cacheTTL, nil),
		resolver:  net.DefaultResolver,
		now:       time.Now,
	}, nil
}

func (t *WebFetchTool) Definition() domain.ToolDefinition {
	return t.base.def
}

func (t *WebFetchTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	args, err := decodeStrict[fetchArgs](call.Arguments)
	if err != nil {
		return domain.PreparedCall{}, err
	}
	args, err = validateFetchArgs(args)
	if err != nil {
		return domain.PreparedCall{}, err
	}

	canonical, err := json.Marshal(args)
	if err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInternal, "failed to encode canonical arguments", domain.WithCause(err))
	}
	approvalDesc := fmt.Sprintf("Fetch %s (GET)", args.URL)
	return t.base.prepareCall(ctx, call, canonical, approvalDesc)
}

func (t *WebFetchTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.base.verifyPreparedCall(prepared); err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	args, err := decodeStrict[fetchArgs](prepared.Call.Arguments)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}

	cacheKey := args.URL + "\x1f" + args.Format
	if entry, ok := t.cache.get(cacheKey); ok {
		return successResult(prepared.Call.ID, startedAt, t.buildOutput(ctx, args, entry, "hit"))
	}

	entry, err := t.fetch(ctx, args)
	if err != nil {
		return errorResult(prepared.Call.ID, startedAt, err)
	}
	if entry.Status >= 200 && entry.Status < 300 && !entry.noStore {
		t.cache.put(cacheKey, entry.cachedResponse)
	}
	return successResult(prepared.Call.ID, startedAt, t.buildOutput(ctx, args, entry.cachedResponse, "miss"))
}

// validateFetchArgs normalizes and validates the call. It is side-effect-free:
// only lexical URL checks plus a literal-IP private-address pre-check (DNS
// names are verified at dial time in Execute).
func validateFetchArgs(args fetchArgs) (fetchArgs, error) {
	if args.Format == "" {
		args.Format = "markdown"
	}
	switch args.Format {
	case "markdown", "text", "raw":
	default:
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("format must be one of markdown, text, raw; got %q", args.Format))
	}
	if args.MaxBytes == 0 {
		args.MaxBytes = defaultMaxBytes
	}
	if args.MaxBytes < minMaxBytes || args.MaxBytes > maxOutputBytes {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("max_bytes must be between %d and %d", minMaxBytes, maxOutputBytes))
	}
	if args.TimeoutMs == 0 {
		args.TimeoutMs = defaultTimeoutMs
	}
	if args.TimeoutMs < 1000 || args.TimeoutMs > maxTimeoutMs {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("timeout_ms must be between 1000 and %d", maxTimeoutMs))
	}

	u, err := url.Parse(strings.TrimSpace(args.URL))
	if err != nil {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, "url is not a valid URL", domain.WithCause(err))
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("url scheme must be http or https; got %q", u.Scheme))
	}
	if u.User != nil {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, "url must not contain userinfo")
	}
	host := u.Hostname()
	if host == "" {
		return fetchArgs{}, domain.NewError(domain.ErrInvalidInput, "url must include a host")
	}
	if ip, parseErr := netip.ParseAddr(strings.TrimSuffix(strings.TrimPrefix(host, "["), "]")); parseErr == nil {
		if !args.AllowPrivate && !isPublicIP(ip) {
			return fetchArgs{}, domain.NewError(domain.ErrSecurity, fmt.Sprintf("destination %s is a private, loopback, or otherwise special address; set allow_private=true to override", host))
		}
	}
	// Fragments are client-side only; strip for a canonical cache key.
	u.Fragment = ""
	u.RawFragment = ""
	args.URL = u.String()
	return args, nil
}

type fetchOutcome struct {
	cachedResponse
	noStore bool
}

// fetch performs the HTTP GET with a dial-time IP guard. The guard checks
// every resolved address immediately before connecting and pins the first
// checked address for the connection, which closes the DNS-rebinding window
// and covers every redirect hop.
func (t *WebFetchTool) fetch(ctx context.Context, args fetchArgs) (fetchOutcome, error) {
	transport := &http.Transport{
		DialContext:         t.guardedDial(args.AllowPrivate),
		TLSHandshakeTimeout: dialTimeout,
		ForceAttemptHTTP2:   true,
	}
	client := &http.Client{
		Transport: transport,
		Timeout:   time.Duration(args.TimeoutMs) * time.Millisecond,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= maxRedirects {
				return errTooManyRedirects
			}
			if req.URL.Scheme != "http" && req.URL.Scheme != "https" {
				return errRedirectScheme
			}
			if req.URL.User != nil {
				return errRedirectUserinfo
			}
			return nil
		},
	}
	defer transport.CloseIdleConnections()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, args.URL, nil)
	if err != nil {
		return fetchOutcome{}, domain.NewError(domain.ErrInvalidInput, "failed to build request", domain.WithCause(err))
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "text/html,application/xhtml+xml,text/*,application/json,application/xml;q=0.9,*/*;q=0.1")

	resp, err := client.Do(req)
	if err != nil {
		return fetchOutcome{}, mapRequestError(err)
	}
	defer resp.Body.Close()

	finalURL := resp.Request.URL.String()
	if resp.StatusCode >= 400 {
		return fetchOutcome{}, httpStatusError(resp.StatusCode, finalURL)
	}

	data, readErr := io.ReadAll(io.LimitReader(resp.Body, hardFetchByteCap+1))
	if readErr != nil {
		return fetchOutcome{}, domain.NewError(domain.ErrUnavailable, "failed to read response body", domain.WithCause(readErr), domain.WithRetryable(true))
	}
	if len(data) > hardFetchByteCap {
		data = data[:hardFetchByteCap]
	}

	contentType := resp.Header.Get("Content-Type")
	mediaType, charset := parseContentType(contentType, data)
	body, err := convertBody(mediaType, charset, data, resp.Request.URL, args.Format)
	if err != nil {
		return fetchOutcome{}, err
	}

	return fetchOutcome{
		cachedResponse: cachedResponse{
			FinalURL:    finalURL,
			Status:      resp.StatusCode,
			ContentType: mediaType,
			FetchedAt:   t.now().UTC(),
			Body:        body,
		},
		noStore: strings.Contains(strings.ToLower(resp.Header.Get("Cache-Control")), "no-store"),
	}, nil
}

// guardedDial resolves the host, applies the IP policy to every answer, then
// dials the first accepted address directly so the checked answer is the one
// actually used.
func (t *WebFetchTool) guardedDial(allowPrivate bool) func(ctx context.Context, network, addr string) (net.Conn, error) {
	return func(ctx context.Context, network, addr string) (net.Conn, error) {
		host, port, err := net.SplitHostPort(addr)
		if err != nil {
			return nil, err
		}
		ips, err := t.resolver.LookupIPAddr(ctx, host)
		if err != nil {
			return nil, fmt.Errorf("dns lookup failed for %s: %w", host, err)
		}
		if len(ips) == 0 {
			return nil, fmt.Errorf("dns lookup returned no addresses for %s", host)
		}
		for _, ipa := range ips {
			ip, ok := netip.AddrFromSlice(ipa.IP)
			if !ok {
				return nil, fmt.Errorf("dns lookup returned an invalid address for %s", host)
			}
			if !allowPrivate && !isPublicIP(ip.Unmap()) {
				return nil, fmt.Errorf("%w: %s resolves to %s", errBlockedAddress, host, ip.Unmap())
			}
		}
		// Dial the checked addresses in order (e.g. localhost may resolve to
		// ::1 first while the server only listens on 127.0.0.1).
		dialer := &net.Dialer{Timeout: dialTimeout}
		var lastErr error
		for _, ipa := range ips {
			conn, err := dialer.DialContext(ctx, network, net.JoinHostPort(ipa.IP.String(), port))
			if err == nil {
				return conn, nil
			}
			lastErr = err
		}
		return nil, lastErr
	}
}

// buildOutput applies the output byte budget to a (possibly cached) body and
// externalizes the full text into an artifact when truncation drops content.
func (t *WebFetchTool) buildOutput(ctx context.Context, args fetchArgs, entry cachedResponse, cacheState string) fetchOutput {
	out := fetchOutput{
		URL:         args.URL,
		FinalURL:    entry.FinalURL,
		Status:      entry.Status,
		ContentType: entry.ContentType,
		Format:      args.Format,
		Bytes:       len(entry.Body),
		Cache:       cacheState,
		FetchedAt:   entry.FetchedAt,
	}
	content := entry.Body
	if len(content) > args.MaxBytes {
		content = truncateAtBoundary(content, args.MaxBytes)
		out.Truncated = true
		out.Artifact = t.storeOverflow(ctx, entry.Body)
	}
	out.Content = content
	return out
}

func (t *WebFetchTool) storeOverflow(ctx context.Context, full string) *domain.ArtifactRef {
	if t.artifacts == nil {
		return nil
	}
	stage, err := t.artifacts.Begin(ctx)
	if err != nil {
		return nil
	}
	if _, err := stage.Write([]byte(full)); err != nil {
		_ = stage.Abort()
		return nil
	}
	ref, err := stage.Commit(ctx)
	if err != nil {
		return nil
	}
	return &ref
}

func truncateAtBoundary(s string, max int) string {
	if len(s) <= max {
		return s
	}
	cut := s[:max]
	// Prefer ending at a line boundary when one is near the cut point.
	if idx := strings.LastIndexByte(cut, '\n'); idx >= 0 && max-idx <= truncateBoundaryGap {
		cut = cut[:idx]
	}
	return cut
}

func parseContentType(header string, data []byte) (mediaType, charset string) {
	mediaType, params, err := mime.ParseMediaType(header)
	if err != nil || mediaType == "" {
		sample := data
		if len(sample) > 512 {
			sample = sample[:512]
		}
		mediaType, _, _ = mime.ParseMediaType(http.DetectContentType(sample))
		return mediaType, "utf-8"
	}
	return strings.ToLower(mediaType), strings.ToLower(params["charset"])
}

// convertBody renders the response body per content type and requested
// format. Non-HTML text types ignore format except that raw always yields
// the untouched body.
func convertBody(mediaType, charset string, data []byte, finalURL *url.URL, format string) (string, error) {
	if format == "raw" {
		if !utf8.Valid(data) {
			return "", unsupportedContentError(mediaType, len(data), "body is not valid UTF-8")
		}
		return string(data), nil
	}

	switch {
	case mediaType == "text/html" || mediaType == "application/xhtml+xml":
		if !utf8.Valid(data) {
			return "", unsupportedContentError(mediaType, len(data), "body is not valid UTF-8")
		}
		if format == "text" {
			return htmlToText(string(data))
		}
		return htmlToMarkdown(string(data), finalURL)
	case isTextual(mediaType):
		if charset != "" && charset != "utf-8" && charset != "us-ascii" {
			return "", unsupportedContentError(mediaType, len(data), fmt.Sprintf("unsupported charset %q (only utf-8 is handled)", charset))
		}
		if !utf8.Valid(data) {
			return "", unsupportedContentError(mediaType, len(data), "body is not valid UTF-8")
		}
		return string(data), nil
	default:
		return "", unsupportedContentError(mediaType, len(data), "")
	}
}

func isTextual(mediaType string) bool {
	if strings.HasPrefix(mediaType, "text/") {
		return true
	}
	switch mediaType {
	case "application/json", "application/xml", "application/javascript",
		"application/x-javascript", "application/yaml", "application/x-yaml",
		"application/toml", "application/x-ndjson", "image/svg+xml":
		return true
	}
	return strings.HasSuffix(mediaType, "+json") || strings.HasSuffix(mediaType, "+xml")
}

func unsupportedContentError(mediaType string, size int, detail string) error {
	msg := fmt.Sprintf("unsupported content type %q (%d bytes)", mediaType, size)
	if detail != "" {
		msg += ": " + detail
	}
	return domain.NewError(domain.ErrInvalidInput, msg)
}

func httpStatusError(status int, finalURL string) error {
	msg := fmt.Sprintf("request failed with status %d (%s)", status, finalURL)
	switch {
	case status == http.StatusTooManyRequests:
		return domain.NewError(domain.ErrRateLimited, msg, domain.WithRetryable(true))
	case status == http.StatusRequestTimeout || status >= 500:
		return domain.NewError(domain.ErrUnavailable, msg, domain.WithRetryable(true))
	default:
		return domain.NewError(domain.ErrUnavailable, msg)
	}
}

func mapRequestError(err error) error {
	switch {
	case errors.Is(err, errBlockedAddress):
		return domain.NewError(domain.ErrSecurity, "destination resolves to a blocked (private or special) address; set allow_private=true to override", domain.WithCause(err))
	case errors.Is(err, errTooManyRedirects):
		return domain.NewError(domain.ErrUnavailable, fmt.Sprintf("too many redirects (limit %d)", maxRedirects))
	case errors.Is(err, errRedirectScheme):
		return domain.NewError(domain.ErrUnavailable, "redirect target uses an unsupported scheme")
	case errors.Is(err, errRedirectUserinfo):
		return domain.NewError(domain.ErrUnavailable, "redirect target must not contain userinfo")
	case errors.Is(err, context.DeadlineExceeded):
		return domain.NewError(domain.ErrTimeout, "request timed out", domain.WithRetryable(true), domain.WithCause(err))
	case errors.Is(err, context.Canceled):
		return err
	}
	var urlErr *url.Error
	if errors.As(err, &urlErr) && urlErr.Timeout() {
		return domain.NewError(domain.ErrTimeout, "request timed out", domain.WithRetryable(true), domain.WithCause(err))
	}
	return domain.NewError(domain.ErrUnavailable, "request failed", domain.WithCause(err), domain.WithRetryable(true))
}

// isPublicIP reports whether the address is a globally routable unicast IP.
// Loopback, private (RFC1918/ULA), link-local, multicast, unspecified,
// broadcast, CGNAT, benchmarking, documentation, and reserved ranges are all
// treated as non-public.
func isPublicIP(ip netip.Addr) bool {
	if !ip.IsValid() {
		return false
	}
	if ip.IsLoopback() || ip.IsPrivate() || ip.IsLinkLocalUnicast() ||
		ip.IsLinkLocalMulticast() || ip.IsMulticast() || ip.IsUnspecified() {
		return false
	}
	if ip.Is4() && ip.As4() == [4]byte{255, 255, 255, 255} {
		return false
	}
	if ip.Is4() {
		b := ip.As4()
		switch {
		case b[0] == 100 && b[1]&0xC0 == 0x40: // CGNAT 100.64.0.0/10
			return false
		case b[0] == 198 && b[1]&0xFE == 18: // benchmarking 198.18.0.0/15
			return false
		case b[0] == 192 && b[1] == 0 && b[2] == 0: // IETF protocol assignments 192.0.0.0/24
			return false
		case b[0] == 192 && b[1] == 0 && b[2] == 2: // documentation TEST-NET-1
			return false
		case b[0] == 198 && b[1] == 51 && b[2] == 100: // documentation TEST-NET-2
			return false
		case b[0] == 203 && b[1] == 0 && b[2] == 113: // documentation TEST-NET-3
			return false
		case b[0]&0xF0 == 240: // reserved 240.0.0.0/4
			return false
		}
		return true
	}
	// IPv6 documentation prefix 2001:db8::/32.
	if b := ip.As16(); b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8 {
		return false
	}
	return true
}
