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
// Created: 2026/08/03

package mcp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"mime"
	"net/http"
	"net/url"
	"sync"
	"sync/atomic"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/model/sse"
)

const (
	// sessionHeader carries the server-assigned session id; the client
	// replays it on every request after initialize (spec 2025-06-18).
	sessionHeader = "Mcp-Session-Id"
	// protocolHeader reports the negotiated protocol revision on every
	// post-initialize request.
	protocolHeader = "MCP-Protocol-Version"

	terminateTimeout = 5 * time.Second
)

// httpTransport speaks MCP's streamable HTTP transport: every JSON-RPC
// message is a POST to the single server endpoint; requests answer with
// either one application/json body or a text/event-stream that ends with
// the matching response, notifications answer with a bare 202.
type httpTransport struct {
	endpoint string
	headers  http.Header // user-configured headers (e.g. Authorization)
	client   *httpc.Client
	logger   *slog.Logger
	name     string

	sessionID atomic.Value // string, assigned at initialize

	mu     sync.Mutex // guards closed
	closed bool
}

// newHTTPTransport validates the endpoint and builds the transport; the
// initialize handshake is the Client's job.
func newHTTPTransport(cfg ClientConfig, logger *slog.Logger) (*httpTransport, error) {
	u, err := url.Parse(cfg.URL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("mcp: invalid server url %q (must be http or https)", cfg.URL))
	}
	client, err := httpc.New(httpc.Config{MaxRetries: 2})
	if err != nil {
		return nil, domain.NewError(domain.ErrInternal, "mcp: failed to build http client", domain.WithCause(err))
	}
	headers := make(http.Header, len(cfg.Headers))
	for k, v := range cfg.Headers {
		headers.Set(k, v)
	}
	return &httpTransport{
		endpoint: cfg.URL,
		headers:  headers,
		client:   client,
		logger:   logger,
		name:     cfg.name,
	}, nil
}

// post sends one message and returns the 2xx response; the caller owns
// resp.Body. Establishment-phase retries (429/5xx, Retry-After) come
// from httpc; a 404 with a live session means the server forgot us.
func (t *httpTransport) post(ctx context.Context, msg []byte) (*http.Response, error) {
	t.mu.Lock()
	closed := t.closed
	t.mu.Unlock()
	if closed {
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: client is closed")
	}

	header := t.headers.Clone()
	header.Set("Content-Type", "application/json")
	header.Set("Accept", "application/json, text/event-stream")
	if sid, _ := t.sessionID.Load().(string); sid != "" {
		header.Set(sessionHeader, sid)
		header.Set(protocolHeader, protocolVersion)
	}

	resp, err := t.client.Post(ctx, t.endpoint, msg, header)
	if err != nil {
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		var statusErr *httpc.StatusError
		if errors.As(err, &statusErr) {
			sid, _ := t.sessionID.Load().(string)
			if statusErr.Code == http.StatusNotFound && sid != "" {
				return nil, domain.NewError(domain.ErrUnavailable, "mcp: server no longer recognizes the session (HTTP 404); reconnect required")
			}
			return nil, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: server rejected the request: %s", statusErr), domain.WithRetryable(statusErr.Retryable()))
		}
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: request failed", domain.WithCause(err))
	}
	t.captureSession(resp.Header)
	return resp, nil
}

// captureSession adopts the session id the server assigns (initialize
// response, or a rotation on any later response).
func (t *httpTransport) captureSession(header http.Header) {
	if sid := header.Get(sessionHeader); sid != "" {
		t.sessionID.Store(sid)
	}
}

func (t *httpTransport) roundTrip(ctx context.Context, id int64, request []byte) (rpcMessage, error) {
	resp, err := t.post(ctx, request)
	if err != nil {
		return rpcMessage{}, err
	}
	defer resp.Body.Close()

	mediaType, _, _ := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	switch mediaType {
	case "application/json":
		body, err := readBounded(resp.Body)
		if err != nil {
			return rpcMessage{}, err
		}
		var msg rpcMessage
		if err := json.Unmarshal(body, &msg); err != nil {
			return rpcMessage{}, domain.NewError(domain.ErrUnavailable, "mcp: malformed response body", domain.WithCause(err))
		}
		return msg, nil
	case "text/event-stream":
		return t.readSSEResponse(resp.Body, id)
	default:
		return rpcMessage{}, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: unexpected response Content-Type %q", resp.Header.Get("Content-Type")))
	}
}

// readSSEResponse consumes the event stream until the response matching
// id arrives. Unrelated notifications and server->client requests on the
// stream are logged and dropped, exactly like the stdio read loop.
func (t *httpTransport) readSSEResponse(body io.Reader, id int64) (rpcMessage, error) {
	parser := sse.NewParser(io.LimitReader(body, maxMessageBytes))
	for {
		event, err := parser.Next()
		if errors.Is(err, io.EOF) {
			return rpcMessage{}, domain.NewError(domain.ErrUnavailable, "mcp: event stream ended before the response arrived")
		}
		if err != nil {
			return rpcMessage{}, domain.NewError(domain.ErrUnavailable, "mcp: failed to read event stream", domain.WithCause(err))
		}
		var msg rpcMessage
		if err := json.Unmarshal([]byte(event.Data), &msg); err != nil {
			t.logger.Warn("mcp: dropping malformed SSE message", "server", t.name, "error", err)
			continue
		}
		if msg.ID == nil || *msg.ID != id {
			t.logger.Debug("mcp: dropping interleaved message", "server", t.name, "method", msg.Method)
			continue
		}
		return msg, nil
	}
}

func (t *httpTransport) notify(ctx context.Context, notification []byte) error {
	resp, err := t.post(ctx, notification)
	if err != nil {
		return err
	}
	return resp.Body.Close()
}

// close terminates the HTTP session best-effort (spec: the client SHOULD
// DELETE the endpoint with its session id) and rejects further calls.
func (t *httpTransport) close() error {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return nil
	}
	t.closed = true
	t.mu.Unlock()

	sid, _ := t.sessionID.Load().(string)
	if sid == "" {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), terminateTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, t.endpoint, nil)
	if err != nil {
		return nil
	}
	for k, vs := range t.headers {
		for _, v := range vs {
			req.Header.Add(k, v)
		}
	}
	req.Header.Set(sessionHeader, sid)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.logger.Debug("mcp: session terminate request failed", "server", t.name, "error", err)
		return nil
	}
	_ = resp.Body.Close()
	return nil
}

// readBounded caps one response body at maxMessageBytes.
func readBounded(r io.Reader) ([]byte, error) {
	body, err := io.ReadAll(io.LimitReader(r, maxMessageBytes+1))
	if err != nil {
		return nil, domain.NewError(domain.ErrUnavailable, "mcp: failed to read response body", domain.WithCause(err))
	}
	if len(body) > maxMessageBytes {
		return nil, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp: response exceeds %d bytes", maxMessageBytes))
	}
	return body, nil
}
