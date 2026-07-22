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
	"encoding/json"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestFakeToolDefinition(t *testing.T) {
	tool := EchoTool()
	def := tool.Definition()
	if def.Name != "echo" {
		t.Fatalf("expected name echo, got %s", def.Name)
	}
	if def.Source != domain.ToolSourceBuiltin {
		t.Fatalf("expected builtin source")
	}
}

func TestFakeToolPrepareNoSideEffects(t *testing.T) {
	tool := ReadFileTool()
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "read_file",
		Arguments: json.RawMessage(`{"path":"/tmp/test.go"}`),
	}

	// Call Prepare twice — should produce identical results (no side effects)
	ctx := context.Background()
	p1, err := tool.Prepare(ctx, call)
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	p2, err := tool.Prepare(ctx, call)
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}

	if p1.ArgsHash != p2.ArgsHash {
		t.Errorf("ArgsHash mismatch: %s vs %s", p1.ArgsHash, p2.ArgsHash)
	}
	if p1.Risk != p2.Risk {
		t.Errorf("Risk mismatch: %d vs %d", p1.Risk, p2.Risk)
	}
}

func TestFakeToolExecute(t *testing.T) {
	tool := ReadFileTool()
	call := domain.ToolCall{
		ID:        domain.NewToolCallID(),
		Name:      "read_file",
		Arguments: json.RawMessage(`{"path":"/tmp/test.go"}`),
	}

	ctx := context.Background()
	prepared, _ := tool.Prepare(ctx, call)
	result := tool.Execute(ctx, prepared)

	if result.Status != domain.ToolStatusSuccess {
		t.Fatalf("expected success, got %s", result.Status)
	}

	// Verify calls were tracked
	if len(tool.PreparedCalls()) != 1 {
		t.Fatalf("expected 1 prepared call, got %d", len(tool.PreparedCalls()))
	}
	if len(tool.ExecutedCalls()) != 1 {
		t.Fatalf("expected 1 executed call, got %d", len(tool.ExecutedCalls()))
	}
}

func TestFakeToolCustomPrepare(t *testing.T) {
	tool := EchoTool()
	tool.WithPrepareFn(func(_ context.Context, call domain.ToolCall) (domain.PreparedCall, error) {
		return domain.PreparedCall{
			Call:         call,
			Definition:   tool.Definition(),
			Risk:         domain.R4, // custom high risk
			ApprovalDesc: "DANGEROUS: " + call.Name,
			ArgsHash:     "custom_hash",
		}, nil
	})

	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{}`)}
	prepared, err := tool.Prepare(context.Background(), call)
	if err != nil {
		t.Fatalf("Prepare error: %v", err)
	}
	if prepared.Risk != domain.R4 {
		t.Fatalf("expected R4, got %d", prepared.Risk)
	}
}

func TestFakeToolRecordsCalls(t *testing.T) {
	tool := ReadFileTool()
	call1 := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"a"}`)}
	call2 := domain.ToolCall{ID: domain.NewToolCallID(), Name: "read_file", Arguments: json.RawMessage(`{"path":"b"}`)}

	ctx := context.Background()
	p1, _ := tool.Prepare(ctx, call1)
	p2, _ := tool.Prepare(ctx, call2)
	tool.Execute(ctx, p1)
	tool.Execute(ctx, p2)

	if len(tool.PreparedCalls()) != 2 {
		t.Fatalf("expected 2 prepared calls, got %d", len(tool.PreparedCalls()))
	}
	if len(tool.ExecutedCalls()) != 2 {
		t.Fatalf("expected 2 executed calls, got %d", len(tool.ExecutedCalls()))
	}
}

func TestEchoToolResultHasText(t *testing.T) {
	tool := EchoTool()
	call := domain.ToolCall{ID: domain.NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{}`)}
	prepared, _ := tool.Prepare(context.Background(), call)
	result := tool.Execute(context.Background(), prepared)

	if result.Status != domain.ToolStatusSuccess {
		t.Fatal("expected success")
	}
	if len(result.Content) == 0 {
		t.Fatal("expected content in result")
	}
}

func TestReadFileToolIsR1(t *testing.T) {
	tool := ReadFileTool()
	if tool.Definition().Risk() != domain.R1 {
		t.Fatalf("expected R1, got %d", tool.Definition().Risk())
	}
}

func TestToolResultTiming(t *testing.T) {
	result := domain.ToolResult{
		CallID:     domain.NewToolCallID(),
		Status:     domain.ToolStatusSuccess,
		StartedAt:  time.Now(),
		FinishedAt: time.Now().Add(100 * time.Millisecond),
	}
	if result.FinishedAt.Before(result.StartedAt) {
		t.Error("FinishedAt should be after StartedAt")
	}
}
