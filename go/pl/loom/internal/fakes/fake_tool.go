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
// Created: 2026/07/22 21:10

package fakes

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// FakeTool is a test double for domain.Tool.
type FakeTool struct {
	mu        sync.Mutex
	def       domain.ToolDefinition
	result    domain.ToolResult
	prepareFn func(context.Context, domain.ToolCall) (domain.PreparedCall, error)
	prepared  []domain.ToolCall
	executed  []domain.PreparedCall
}

// NewFakeTool creates a FakeTool with the given definition and default success result.
func NewFakeTool(def domain.ToolDefinition, result domain.ToolResult) *FakeTool {
	return &FakeTool{def: def, result: result}
}

// WithPrepareFn overrides the default Prepare behavior.
func (t *FakeTool) WithPrepareFn(fn func(context.Context, domain.ToolCall) (domain.PreparedCall, error)) *FakeTool {
	t.prepareFn = fn
	return t
}

func (t *FakeTool) Definition() domain.ToolDefinition { return t.def }

func (t *FakeTool) Prepare(ctx context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.prepared = append(t.prepared, call)

	if t.prepareFn != nil {
		return t.prepareFn(ctx, call)
	}

	// Default: hash the arguments like production tools do (sha256, first
	// 16 hex chars) so repeat-detection and audit assertions observe
	// realistic fingerprints — the previous raw-hex prefix collapsed every
	// call sharing an 8-byte argument prefix into one signature.
	argsHash := "fake_hash"
	if len(call.Arguments) > 0 {
		sum := sha256.Sum256(call.Arguments)
		argsHash = hex.EncodeToString(sum[:])[:16]
	}

	return domain.PreparedCall{
		Call:         call,
		Definition:   t.def,
		Risk:         t.def.Risk(),
		ApprovalDesc: fmt.Sprintf("Execute %s", call.Name),
		ArgsHash:     argsHash,
	}, nil
}

func (t *FakeTool) Execute(ctx context.Context, prepared domain.PreparedCall) domain.ToolResult {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.executed = append(t.executed, prepared)
	result := t.result
	// Real tools always stamp the result with the owning call ID; the loop
	// pairs calls with results by it (isToolResultRecorded). Mirror that so
	// test scenarios observe the same pairing invariant as production.
	if result.CallID.IsZero() {
		result.CallID = prepared.Call.ID
	}
	return result
}

// PreparedCalls returns all calls that went through Prepare.
func (t *FakeTool) PreparedCalls() []domain.ToolCall {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]domain.ToolCall, len(t.prepared))
	copy(out, t.prepared)
	return out
}

// ExecutedCalls returns all calls that went through Execute.
func (t *FakeTool) ExecutedCalls() []domain.PreparedCall {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]domain.PreparedCall, len(t.executed))
	copy(out, t.executed)
	return out
}

// EchoTool is a simple tool that echoes back the arguments as text.
func EchoTool() *FakeTool {
	def := domain.ToolDefinition{
		Name:         "echo",
		Description:  "Echo back the input",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}
	return NewFakeTool(def, domain.ToolResult{
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: "echo"}},
		StartedAt:  time.Now(),
		FinishedAt: time.Now(),
	})
}

// ReadFileTool simulates a read-only file tool (R1).
func ReadFileTool() *FakeTool {
	def := domain.ToolDefinition{
		Name:         "read_file",
		Description:  "Read file contents",
		InputSchema:  json.RawMessage(`{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}`),
		Capabilities: []domain.Capability{domain.CapFSRead},
		Source:       domain.ToolSourceBuiltin,
	}
	return NewFakeTool(def, domain.ToolResult{
		Status:     domain.ToolStatusSuccess,
		Content:    []domain.ContentPart{{Kind: domain.PartText, Text: "file contents here"}},
		StartedAt:  time.Now(),
		FinishedAt: time.Now(),
	})
}
