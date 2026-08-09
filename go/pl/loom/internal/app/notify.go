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
// Created: 2026/08/09

package app

import (
	"context"
	"os/exec"
	"runtime"
	"strings"
	"testing"
	"time"
)

// NotifyApproval fires a best-effort desktop notification for a pending
// approval: a run that reaches an interactive prompt is by definition
// blocked on a human, and the human is often away from the terminal
// (long-horizon runs). Failures are swallowed — a notification must never
// break the approval flow.
func NotifyApproval(toolName, description string) {
	if testing.Testing() {
		return // never spam the desktop from test binaries
	}
	body := toolName
	if description != "" {
		body = description
	}
	if len(body) > 120 {
		body = body[:117] + "..."
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		script := `display notification ` + osascriptQuote(body) + ` with title "Loom 审批请求"`
		cmd = exec.CommandContext(ctx, "osascript", "-e", script)
	case "linux":
		if _, err := exec.LookPath("notify-send"); err != nil {
			return
		}
		cmd = exec.CommandContext(ctx, "notify-send", "Loom 审批请求", body)
	default:
		return
	}
	_ = cmd.Run()
}

// osascriptQuote renders s as an AppleScript string literal.
func osascriptQuote(s string) string {
	return `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(s) + `"`
}
