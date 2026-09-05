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
// Created: 2026/09/05

// Package wireutil holds the provider-neutral wire helpers shared by the
// anthropic and openai providers (REVIEW R5): SSE stream bootstrap,
// tool-result rendering, input-schema decoding, and read-error
// classification. Wire-protocol semantics (event mapping, role
// normalization, stop reasons) stay in the providers.
package wireutil

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/httpc"
	"github.com/liubang/playground/go/pl/loom/internal/model/stream"
)

// NewHTTPClient builds the shared retrying HTTP client, prefixing config
// errors with the provider name.
func NewHTTPClient(provider string, cfg httpc.Config) (*httpc.Client, error) {
	client, err := httpc.New(cfg)
	if err != nil {
		return nil, fmt.Errorf("%s provider: %w", provider, err)
	}
	return client, nil
}

// StreamHeaders returns the shared SSE request headers; authentication is
// provider-specific and set by the caller.
func StreamHeaders() http.Header {
	return http.Header{
		"Content-Type":  {"application/json"},
		"Accept":        {"text/event-stream"},
		"Cache-Control": {"no-cache"},
	}
}

// StartStream posts one streaming request and launches the shared pump
// runner. provider is the error/log prefix ("anthropic", "openai").
func StartStream(ctx context.Context, client *httpc.Client, endpoint string, body []byte, headers http.Header, provider string, pump stream.Pump) (domain.ModelStream, error) {
	resp, err := client.Post(ctx, endpoint, body, headers)
	if err != nil {
		// Classify the failure (rate limit / permission / transient) so the
		// agent loop can wait out retryable ones instead of killing the run.
		return nil, httpc.ToDomainError(provider+" provider", err)
	}
	if err := httpc.RequireEventStream(resp); err != nil {
		_ = resp.Body.Close()
		return nil, fmt.Errorf("%s provider: %w", provider, err)
	}
	return stream.Start(ctx, resp.Body, pump), nil
}

// MessageText concatenates the text parts of a message; any non-text part
// is a protocol error for callers that use it.
func MessageText(provider string, msg domain.Message) (string, error) {
	var b strings.Builder
	for _, part := range msg.Parts {
		if part.Kind != domain.PartText {
			return "", fmt.Errorf("%s provider: role %q only supports text parts", provider, msg.Role)
		}
		b.WriteString(part.Text)
	}
	return b.String(), nil
}

// ToolInputSchema decodes a tool's input schema for the wire; an empty
// schema defaults to an unconstrained object.
func ToolInputSchema(provider string, def domain.ToolDefinition) (any, error) {
	if len(def.InputSchema) == 0 {
		return map[string]any{"type": "object"}, nil
	}
	var schema any
	if err := json.Unmarshal(def.InputSchema, &schema); err != nil {
		return nil, fmt.Errorf("%s provider: decode tool schema for %q: %w", provider, def.Name, err)
	}
	return schema, nil
}

// ToolResultHasImages reports whether a tool result carries image parts;
// wires that accept image blocks render them separately.
func ToolResultHasImages(result domain.ToolResult) bool {
	for _, part := range result.Content {
		if part.Kind == domain.PartImage {
			return true
		}
	}
	return false
}

// ToolResultText renders a tool result as a single string: a structured
// error envelope for failures, the raw concatenated text when the result is
// text-only, and a structured JSON envelope otherwise. Artifact references
// stay in the canonical ToolResult — tools embed model-safe reference
// metadata in their bounded text payload, so refs are not duplicated here
// (accidentally re-inlining them would blow context budgets).
func ToolResultText(result domain.ToolResult) string {
	if result.Error != nil {
		payload, err := json.Marshal(map[string]any{
			"status": result.Status,
			"error":  result.Error,
		})
		if err == nil {
			return string(payload)
		}
		return result.Error.Message
	}

	pureText := true
	var text strings.Builder
	for _, part := range result.Content {
		switch part.Kind {
		case domain.PartText:
			text.WriteString(part.Text)
		case domain.PartArtifact:
			// References stay in the canonical result only.
		default:
			pureText = false
		}
	}
	if pureText && text.Len() > 0 {
		return text.String()
	}

	payload, err := json.Marshal(map[string]any{
		"status":  result.Status,
		"content": result.Content,
		"meta":    result.Metadata,
	})
	if err == nil {
		return string(payload)
	}
	return string(result.Status)
}

// EmitStreamFailure terminates the canonical event stream on a failure:
// any provider-open blocks are closed defensively (closeOpen, may be nil),
// then the StreamError + ResponseEnd pair is emitted.
func EmitStreamFailure(emit stream.Emitter, err error, stop domain.StopReason, retryable bool, closeOpen func()) {
	if closeOpen != nil {
		closeOpen()
	}
	emit(domain.ModelEvent{Kind: domain.ModelEventStreamError, Error: err.Error(), Retryable: retryable})
	emit(domain.ModelEvent{Kind: domain.ModelEventResponseEnd, StopReason: stop})
}

// FinishReadError routes a terminal read error from an SSE pump:
//   - context cancellation reports StopCancelled (non-retryable);
//   - io.EOF goes to handleEOF first — a gateway may close the connection
//     right after the terminal event instead of sending the sentinel, and a
//     completed generation must not turn into a failure; unclaimed EOFs are
//     transient "stream closed before <terminal>" errors;
//   - anything else is a transient "stream read failed" error.
//
// Each case is handed to finish, which providers bind to their own
// close-and-emit failure finisher (see EmitStreamFailure).
func FinishReadError(ctx context.Context, err error, provider, terminal string,
	handleEOF func() bool,
	finish func(err error, stop domain.StopReason, retryable bool),
) {
	switch {
	case ctx.Err() != nil:
		finish(ctx.Err(), domain.StopCancelled, false)
	case errors.Is(err, io.EOF):
		if handleEOF != nil && handleEOF() {
			return
		}
		finish(fmt.Errorf("%s provider: stream closed before %s", provider, terminal), domain.StopProviderError, true)
	default:
		finish(fmt.Errorf("%s provider: stream read failed: %w", provider, err), domain.StopProviderError, true)
	}
}
