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

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strings"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/model/openai"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/tool/builtin"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	"github.com/liubang/playground/go/pl/loom/internal/tool/gittools"
	"github.com/liubang/playground/go/pl/loom/internal/workspace"
)

const version = "0.2.0-dev"

func main() {
	if err := run(context.Background(), os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "loom:", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string) error {
	if len(args) == 0 {
		return errors.New("usage: loom <run|version> [args]")
	}
	switch args[0] {
	case "version":
		fmt.Println("loom", version)
		return nil
	case "run":
		if len(args) < 2 || strings.TrimSpace(strings.Join(args[1:], " ")) == "" {
			return errors.New("usage: loom run <prompt>")
		}
		return runAgent(ctx, strings.Join(args[1:], " "))
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func runAgent(ctx context.Context, prompt string) error {
	root := strings.TrimSpace(os.Getenv("BUILD_WORKSPACE_DIRECTORY"))
	if root == "" {
		var err error
		root, err = os.Getwd()
		if err != nil {
			return fmt.Errorf("get workspace: %w", err)
		}
	}
	validator, err := workspace.NewPathValidator(root)
	if err != nil {
		return fmt.Errorf("validate workspace: %w", err)
	}
	registry := agent.NewToolRegistry()
	readFile, err := builtin.NewReadFileTool(validator)
	if err != nil {
		return err
	}
	listDirectory, err := builtin.NewListDirectoryTool(validator)
	if err != nil {
		return err
	}
	searchText, err := builtin.NewSearchTextTool(validator)
	if err != nil {
		return err
	}
	replaceText, err := edit.NewReplaceTextTool(validator)
	if err != nil {
		return err
	}
	applyPatch, err := edit.NewApplyPatchTool(validator)
	if err != nil {
		return err
	}
	gitStatus, err := gittools.NewGitStatusTool(validator)
	if err != nil {
		return err
	}
	gitDiff, err := gittools.NewGitDiffTool(validator)
	if err != nil {
		return err
	}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		return err
	}
	runCommand, err := command.NewRunCommandTool(validator, runner)
	if err != nil {
		return err
	}
	for _, tool := range []domain.Tool{
		readFile, listDirectory, searchText, replaceText, applyPatch, gitStatus, gitDiff, runCommand,
	} {
		if err := registry.Register(tool); err != nil {
			return err
		}
	}

	modelName := strings.TrimSpace(os.Getenv("LOOM_MODEL"))
	if modelName == "" {
		return errors.New("LOOM_MODEL is required")
	}
	provider, err := openai.New(openai.Config{
		BaseURL:    os.Getenv("LOOM_BASE_URL"),
		APIKey:     os.Getenv("LOOM_API_KEY"),
		WireAPI:    openai.WireAPI(os.Getenv("LOOM_WIRE_API")),
		MaxRetries: 2,
	})
	if err != nil {
		return err
	}

	run := agent.NewRun(domain.NewSessionID(), domain.DefaultLimits(), domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID: domain.NewMessageID(), Role: domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: prompt}},
		CreatedAt: time.Now().UTC(),
	})
	loop := agent.Loop{
		Run: run, Model: provider, ModelName: modelName,
		Approver: consoleApprover{}, Registry: registry, Logger: slog.Default(),
	}
	if err := loop.Execute(ctx); err != nil {
		return err
	}
	for i := len(run.Messages) - 1; i >= 0; i-- {
		if run.Messages[i].Role == domain.RoleAssistant {
			for _, text := range run.Messages[i].TextParts() {
				fmt.Print(text)
			}
			fmt.Println()
			return nil
		}
	}
	return errors.New("model produced no final answer")
}

type consoleApprover struct{}

func (consoleApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	info, err := os.Stdin.Stat()
	if err != nil {
		return domain.DecisionDeny, fmt.Errorf("inspect stdin: %w", err)
	}
	if info.Mode()&os.ModeCharDevice == 0 {
		return domain.DecisionDeny, nil
	}
	fmt.Fprintf(os.Stderr, "\nApproval required (R%d): %s\nargs hash: %s\nAllow? [y/N] ",
		req.Call.Risk, req.Description, req.Call.ArgsHash)
	answer := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
		answer <- strings.TrimSpace(strings.ToLower(line))
	}()
	select {
	case <-ctx.Done():
		return domain.DecisionDeny, ctx.Err()
	case value := <-answer:
		if value == "y" || value == "yes" {
			return domain.DecisionAllow, nil
		}
		return domain.DecisionDeny, nil
	}
}
