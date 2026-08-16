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
// Created: 2026/08/16

package main

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"

	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// consoleApprover prompts on the terminal. A single background reader feeds
// a line channel for the process lifetime: recreating bufio.Reader per
// approval used to drop bytes it had already buffered, and each cancelled
// approval leaked a goroutine that then raced the next one on stdin.
type consoleApprover struct {
	once  sync.Once
	lines chan string
}

// start launches the one stdin reader goroutine. It exits on stdin EOF.
func (a *consoleApprover) start(r io.Reader) {
	a.once.Do(func() {
		a.lines = make(chan string)
		go func() {
			defer close(a.lines)
			reader := bufio.NewReader(r)
			for {
				line, err := reader.ReadString('\n')
				if err != nil {
					return
				}
				a.lines <- strings.TrimSpace(strings.ToLower(line))
			}
		}()
	})
}

// awaitAnswer waits for the next input line or ctx cancellation.
func (a *consoleApprover) awaitAnswer(ctx context.Context) (domain.Decision, error) {
	select {
	case <-ctx.Done():
		return domain.DecisionDeny, ctx.Err()
	case value, ok := <-a.lines:
		if !ok {
			return domain.DecisionDeny, nil
		}
		if value == "y" || value == "yes" {
			return domain.DecisionAllow, nil
		}
		return domain.DecisionDeny, nil
	}
}

func (a *consoleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	info, err := os.Stdin.Stat()
	if err != nil {
		return domain.DecisionDeny, fmt.Errorf("inspect stdin: %w", err)
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		// Unattended (piped stdin): the request can never be answered, so
		// it is denied — but the desktop notification still goes out: the
		// whole point of a headless long run is that the user is elsewhere.
		app.NotifyApproval(req.Call.Call.Name, req.Description+" (无人值守，已自动拒绝)")
		return domain.DecisionDeny, nil
	}
	a.start(os.Stdin)
	app.NotifyApproval(req.Call.Call.Name, req.Description)
	fmt.Fprintf(os.Stderr, "\nApproval required (R%d): %s\nargs hash: %s\nAllow? [y/N] ",
		req.Call.Risk, req.Description, req.Call.ArgsHash)
	return a.awaitAnswer(ctx)
}
