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
// Created: 2026/05/16 20:56

package command

import (
	"context"
	"strings"
	"testing"
	"time"
)

func TestCommandOutput_Echo(t *testing.T) {
	ctx := context.Background()
	output, err := CommandOutput(ctx, "", "echo", []string{"hello"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	got := strings.TrimSpace(output)
	if got != "hello" {
		t.Errorf("CommandOutput echo = %q, want %q", got, "hello")
	}
}

func TestCommandOutput_WithDir(t *testing.T) {
	ctx := context.Background()
	output, err := CommandOutput(ctx, "/tmp", "pwd", nil)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// /tmp may resolve to /private/tmp on macOS
	got := strings.TrimSpace(output)
	if got != "/tmp" && got != "/private/tmp" {
		t.Errorf("CommandOutput pwd in /tmp = %q, want /tmp or /private/tmp", got)
	}
}

func TestCommandOutput_EmptyDir(t *testing.T) {
	ctx := context.Background()
	output, err := CommandOutput(ctx, "", "echo", []string{"test"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	got := strings.TrimSpace(output)
	if got != "test" {
		t.Errorf("CommandOutput echo with empty dir = %q, want %q", got, "test")
	}
}

func TestCommandOutput_InvalidCommand(t *testing.T) {
	ctx := context.Background()
	_, err := CommandOutput(ctx, "", "nonexistent_command_xyz", nil)
	if err == nil {
		t.Error("expected error for nonexistent command, got nil")
	}
}

func TestCommandOutput_ContextCancel(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()

	_, err := CommandOutput(ctx, "", "sleep", []string{"10"})
	if err == nil {
		t.Error("expected error from context cancellation, got nil")
	}
	if !strings.Contains(err.Error(), "context deadline exceeded") {
		t.Errorf("expected context deadline exceeded error, got: %v", err)
	}
}

func TestCommandOutput_MultipleArgs(t *testing.T) {
	ctx := context.Background()
	output, err := CommandOutput(ctx, "", "printf", []string{"%s %s", "hello", "world"})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	got := strings.TrimSpace(output)
	if got != "hello world" {
		t.Errorf("CommandOutput printf = %q, want %q", got, "hello world")
	}
}

func TestCommandOutput_ExitCode(t *testing.T) {
	ctx := context.Background()
	_, err := CommandOutput(ctx, "", "bash", []string{"-c", "exit 1"})
	if err == nil {
		t.Error("expected error for non-zero exit code, got nil")
	}
}
