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
// Created: 2026/08/01

package mcp

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const (
	// toolNamePrefix mirrors codex's mcp__{server}__{tool} qualification so
	// MCP tools can never collide with built-ins.
	toolNamePrefix    = "mcp"
	toolNameDelimiter = "__"
	maxToolNameBytes  = 64
	maxArgBytes       = 1 << 20
	maxResultBytes    = 1 << 20
)

var toolNameSanitizer = regexp.MustCompile(`[^a-zA-Z0-9_-]+`)

// qualifiedToolName builds the model-visible tool name: sanitize both
// parts, and when the result would exceed 64 bytes (the Responses API
// limit), truncate and suffix a stable hash so distinct tools never merge.
func qualifiedToolName(server, tool string) string {
	clean := func(s string) string {
		s = toolNameSanitizer.ReplaceAllString(s, "_")
		return strings.Trim(s, "_")
	}
	name := toolNamePrefix + toolNameDelimiter + clean(server) + toolNameDelimiter + clean(tool)
	if len(name) <= maxToolNameBytes {
		return name
	}
	sum := sha1.Sum([]byte(name)) //nolint:gosec // hash is for collision-avoidance, not security
	suffix := hex.EncodeToString(sum[:6])
	keep := maxToolNameBytes - len(suffix) - 1
	return name[:keep] + "_" + suffix
}

// riskForSpec maps MCP tool annotations onto loom's risk ladder through the
// capability vocabulary: read-only tools classify R1, destructive/open-world
// tools R3, everything else the R2 default.
func capabilitiesForSpec(spec ToolSpec) []domain.Capability {
	if spec.Annotations != nil {
		switch {
		case spec.Annotations.ReadOnlyHint:
			return []domain.Capability{domain.CapFSRead}
		case spec.Annotations.DestructiveHint, spec.Annotations.OpenWorldHint:
			return []domain.Capability{domain.CapNetworkConnect}
		}
	}
	return []domain.Capability{domain.CapProcessExec}
}

// ToolAdapter exposes one MCP tool as a domain.Tool. Prepare is
// side-effect-free (JSON validation + signing only); the subprocess call
// happens exclusively in Execute, preserving loom's prepare→approve→execute
// contract.
type ToolAdapter struct {
	client     *Client
	serverName string
	toolName   string // server-local name sent on the wire
	def        domain.ToolDefinition
	key        [32]byte
}

type adapterFingerprint struct {
	CallID    string           `json:"call_id"`
	ToolName  string           `json:"tool_name"`
	Arguments json.RawMessage  `json:"arguments"`
	Risk      domain.RiskLevel `json:"risk"`
}

// NewToolAdapter adapts spec into a domain.Tool wired to client.
func NewToolAdapter(client *Client, serverName string, spec ToolSpec) (*ToolAdapter, error) {
	if strings.TrimSpace(spec.Name) == "" {
		return nil, domain.NewError(domain.ErrInvalidInput, "mcp tool name is required")
	}
	inputSchema := spec.InputSchema
	if len(inputSchema) == 0 {
		inputSchema = json.RawMessage(`{"type":"object"}`)
	}
	// OpenAI-family vendors require properties to exist; insert an empty
	// object when the server omitted it (codex does the same).
	inputSchema = ensurePropertiesKey(inputSchema)

	description := strings.TrimSpace(spec.Description)
	if description == "" {
		description = fmt.Sprintf("MCP tool %q from server %q.", spec.Name, serverName)
	} else {
		description = fmt.Sprintf("[MCP server %q] %s", serverName, description)
	}

	def := domain.ToolDefinition{
		Name:         qualifiedToolName(serverName, spec.Name),
		Description:  description,
		InputSchema:  inputSchema,
		Capabilities: capabilitiesForSpec(spec),
		Source:       domain.ToolSourceMCP,
	}
	if err := def.Validate(); err != nil {
		return nil, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("invalid mcp tool definition for %q", spec.Name), domain.WithCause(err))
	}

	var key [32]byte
	if _, err := rand.Read(key[:]); err != nil {
		return nil, domain.NewError(domain.ErrInternal, "failed to initialize tool verifier", domain.WithCause(err))
	}
	return &ToolAdapter{client: client, serverName: serverName, toolName: spec.Name, def: def, key: key}, nil
}

// ensurePropertiesKey inserts "properties":{} into an object schema that
// lacks it, the way codex normalizes MCP schemas for OpenAI vendors.
func ensurePropertiesKey(schema json.RawMessage) json.RawMessage {
	var doc map[string]any
	if err := json.Unmarshal(schema, &doc); err != nil {
		return schema
	}
	if _, ok := doc["properties"]; ok {
		return schema
	}
	doc["properties"] = map[string]any{}
	out, err := json.Marshal(doc)
	if err != nil {
		return schema
	}
	return out
}

func (t *ToolAdapter) Definition() domain.ToolDefinition {
	return t.def
}

// ConcurrentSafe implements domain.ConcurrentSafely: calls on one client
// serialize through its pending-request map and stdin writes are atomic
// per request line, so independent calls may batch. Servers that cannot
// interleave requests simply answer in order.
func (t *ToolAdapter) ConcurrentSafe() bool { return true }

func (t *ToolAdapter) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	if err := ctx.Err(); err != nil {
		return domain.PreparedCall{}, err
	}
	if err := call.Validate(); err != nil {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, "invalid tool call", domain.WithCause(err))
	}
	if call.Name != t.def.Name {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("tool call name must be %q", t.def.Name))
	}
	if len(call.Arguments) > maxArgBytes {
		return domain.PreparedCall{}, domain.NewError(domain.ErrInvalidInput, fmt.Sprintf("arguments exceed %d bytes", maxArgBytes))
	}

	prepared := domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        call.ID,
			Name:      t.def.Name,
			Arguments: append(json.RawMessage(nil), call.Arguments...),
		},
		Definition:   t.def,
		Risk:         t.def.Risk(),
		ApprovalDesc: fmt.Sprintf("Call MCP tool %s on server %s", t.toolName, t.serverName),
	}
	prepared.ArgsHash = t.sign(prepared)
	return prepared, nil
}

func (t *ToolAdapter) sign(prepared domain.PreparedCall) string {
	fingerprint := adapterFingerprint{
		CallID:    prepared.Call.ID.String(),
		ToolName:  prepared.Call.Name,
		Arguments: append(json.RawMessage(nil), prepared.Call.Arguments...),
		Risk:      prepared.Risk,
	}
	payload, _ := json.Marshal(fingerprint)
	h := hmac.New(sha256.New, t.key[:])
	_, _ = h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}

func (t *ToolAdapter) verify(prepared domain.PreparedCall) error {
	if prepared.Call.Name != t.def.Name || prepared.Definition.Name != t.def.Name {
		return domain.NewError(domain.ErrSecurity, "prepared call tool name mismatch")
	}
	if prepared.Definition.Source != t.def.Source {
		return domain.NewError(domain.ErrSecurity, "prepared call source mismatch")
	}
	if prepared.Risk != t.def.Risk() {
		return domain.NewError(domain.ErrSecurity, "prepared call risk mismatch")
	}
	expected := t.sign(prepared)
	if !hmac.Equal([]byte(prepared.ArgsHash), []byte(expected)) {
		return domain.NewError(domain.ErrSecurity, "prepared call verification failed")
	}
	return nil
}

func (t *ToolAdapter) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	startedAt := time.Now()
	if err := t.verify(prepared); err != nil {
		return adapterErrorResult(prepared.Call.ID, startedAt, err)
	}

	result, err := t.client.CallTool(ctx, t.toolName, prepared.Call.Arguments)
	if err != nil {
		return adapterErrorResult(prepared.Call.ID, startedAt, err)
	}

	content, err := mapToolContent(result)
	if err != nil {
		return adapterErrorResult(prepared.Call.ID, startedAt, err)
	}
	if result.IsError {
		return domain.ToolResult{
			CallID:     prepared.Call.ID,
			Status:     domain.ToolStatusError,
			Error:      &domain.ToolError{Code: string(domain.ErrUnavailable), Message: flattenText(content), Retryable: false},
			StartedAt:  startedAt,
			FinishedAt: time.Now(),
		}
	}
	return domain.ToolResult{
		CallID:     prepared.Call.ID,
		Status:     domain.ToolStatusSuccess,
		Content:    content,
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}

// mapToolContent converts MCP content items into domain parts: text stays
// text, images become image parts (loom's multimodal pipeline renders them
// for vision models), and unsupported types degrade to a text description.
func mapToolContent(result CallToolResult) ([]domain.ContentPart, error) {
	if len(result.Content) == 0 {
		return []domain.ContentPart{{Kind: domain.PartText, Text: "(no content)"}}, nil
	}
	parts := make([]domain.ContentPart, 0, len(result.Content))
	total := 0
	for i, item := range result.Content {
		switch item.Type {
		case "text":
			total += len(item.Text)
			if total > maxResultBytes {
				return nil, domain.NewError(domain.ErrUnavailable, fmt.Sprintf("mcp tool result exceeds %d bytes", maxResultBytes))
			}
			parts = append(parts, domain.ContentPart{PartIndex: i, Kind: domain.PartText, Text: item.Text})
		case "image":
			if item.MimeType == "" || item.Data == "" {
				parts = append(parts, domain.ContentPart{PartIndex: i, Kind: domain.PartText, Text: "[image content with missing mimeType/data omitted]"})
				continue
			}
			parts = append(parts, domain.ContentPart{PartIndex: i, Kind: domain.PartImage, Image: &domain.ImageContent{MediaType: item.MimeType, Data: item.Data}})
		case "resource":
			text := item.Text
			if text == "" {
				text = "[binary resource content omitted]"
			}
			parts = append(parts, domain.ContentPart{PartIndex: i, Kind: domain.PartText, Text: text})
		default:
			parts = append(parts, domain.ContentPart{PartIndex: i, Kind: domain.PartText, Text: fmt.Sprintf("[unsupported mcp content type %q]", item.Type)})
		}
	}
	return parts, nil
}

func flattenText(parts []domain.ContentPart) string {
	var b strings.Builder
	for _, part := range parts {
		if part.Kind == domain.PartText {
			b.WriteString(part.Text)
		}
	}
	msg := b.String()
	if msg == "" {
		msg = "mcp tool reported an error"
	}
	return msg
}

func adapterErrorResult(callID domain.ToolCallID, startedAt time.Time, err error) domain.ToolResult {
	status := domain.ToolStatusError
	code := string(domain.ErrInternal)
	message := "internal tool error"
	retryable := false

	switch {
	case errors.Is(err, context.Canceled):
		status = domain.ToolStatusCancelled
		code = string(domain.ErrCancelled)
		message = "operation cancelled"
	case errors.Is(err, context.DeadlineExceeded):
		status = domain.ToolStatusTimeout
		code = string(domain.ErrTimeout)
		message = "operation timed out"
	default:
		var agentErr *domain.AgentError
		if errors.As(err, &agentErr) {
			code = string(agentErr.Code)
			message = agentErr.Message
			retryable = agentErr.Retryable
		} else if err != nil {
			message = err.Error()
		}
	}
	return domain.ToolResult{
		CallID:     callID,
		Status:     status,
		Error:      &domain.ToolError{Code: code, Message: message, Retryable: retryable},
		StartedAt:  startedAt,
		FinishedAt: time.Now(),
	}
}
