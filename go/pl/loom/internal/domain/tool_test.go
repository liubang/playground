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

package domain

import (
	"encoding/json"
	"testing"
	"time"
)

func TestToolDefinitionValidation(t *testing.T) {
	tests := []struct {
		name    string
		def     ToolDefinition
		wantErr bool
	}{
		{
			"valid definition",
			ToolDefinition{
				Name:        "echo",
				Description: "Echo input",
				InputSchema: json.RawMessage(`{"type":"object"}`),
				Source:      ToolSourceBuiltin,
			},
			false,
		},
		{
			"empty name",
			ToolDefinition{
				InputSchema: json.RawMessage(`{}`),
			},
			true,
		},
		{
			"invalid input schema",
			ToolDefinition{
				Name:        "test",
				InputSchema: json.RawMessage(`not json`),
			},
			true,
		},
		{
			"invalid output schema",
			ToolDefinition{
				Name:         "test",
				InputSchema:  json.RawMessage(`{}`),
				OutputSchema: json.RawMessage(`not json`),
			},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.def.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestToolDefinitionRisk(t *testing.T) {
	tests := []struct {
		name     string
		caps     []Capability
		wantRisk RiskLevel
	}{
		{"R0 no caps", nil, R0},
		{"R1 fs.read", []Capability{CapFSRead}, R1},
		{"R1 git.read", []Capability{CapGitRead}, R1},
		{"R2 fs.write", []Capability{CapFSWrite}, R2},
		{"R2 process.exec", []Capability{CapProcessExec}, R2},
		{"R3 fs.delete", []Capability{CapFSDelete}, R3},
		{"R3 network.connect", []Capability{CapNetworkConnect}, R3},
		{"R4 git.remote_write", []Capability{CapGitRemoteWrite}, R4},
		{"R4 secret.use", []Capability{CapSecretUse}, R4},
		{"mixed R1+R3", []Capability{CapFSRead, CapFSDelete}, R3},
		{"mixed R1+R2+R4", []Capability{CapFSRead, CapFSWrite, CapSecretUse}, R4},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			def := ToolDefinition{Name: "test", Capabilities: tt.caps}
			if got := def.Risk(); got != tt.wantRisk {
				t.Errorf("Risk() = %d, want %d", got, tt.wantRisk)
			}
		})
	}
}

func TestToolCallValidation(t *testing.T) {
	tests := []struct {
		name    string
		call    ToolCall
		wantErr bool
	}{
		{
			"valid call",
			ToolCall{ID: NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`{}`)},
			false,
		},
		{
			"empty ID",
			ToolCall{Name: "echo", Arguments: json.RawMessage(`{}`)},
			true,
		},
		{
			"empty name",
			ToolCall{ID: NewToolCallID(), Arguments: json.RawMessage(`{}`)},
			true,
		},
		{
			"invalid arguments",
			ToolCall{ID: NewToolCallID(), Name: "echo", Arguments: json.RawMessage(`not json`)},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.call.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestToolResultValidation(t *testing.T) {
	tests := []struct {
		name    string
		result  ToolResult
		wantErr bool
	}{
		{
			"valid success",
			ToolResult{CallID: NewToolCallID(), Status: ToolStatusSuccess, StartedAt: time.Now(), FinishedAt: time.Now()},
			false,
		},
		{
			"empty call ID",
			ToolResult{Status: ToolStatusSuccess},
			true,
		},
		{
			"invalid status",
			ToolResult{CallID: NewToolCallID(), Status: "unknown"},
			true,
		},
		{
			"error without ToolError",
			ToolResult{CallID: NewToolCallID(), Status: ToolStatusError},
			true,
		},
		{
			"error with ToolError",
			ToolResult{CallID: NewToolCallID(), Status: ToolStatusError, Error: &ToolError{Code: "test", Message: "failed"}},
			false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.result.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

func TestToolResultIsRetryable(t *testing.T) {
	r := ToolResult{CallID: NewToolCallID(), Status: ToolStatusError, Error: &ToolError{Code: "timeout", Message: "timed out", Retryable: true}}
	if !r.IsRetryable() {
		t.Error("expected retryable")
	}

	r2 := ToolResult{CallID: NewToolCallID(), Status: ToolStatusError, Error: &ToolError{Code: "fatal", Message: "fatal", Retryable: false}}
	if r2.IsRetryable() {
		t.Error("expected not retryable")
	}

	r3 := ToolResult{CallID: NewToolCallID(), Status: ToolStatusSuccess}
	if r3.IsRetryable() {
		t.Error("expected not retryable for success")
	}
}
