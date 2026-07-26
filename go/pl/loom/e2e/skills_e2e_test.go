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
// Created: 2026/07/26

// Package e2e holds in-process end-to-end tests: scripted FakeModel
// responses drive the real agent loop with the real tool registry, prompt
// builder, skills loader, and (where needed) the real process sandbox.
// See docs/SKILL_DESIGN.md §8.2.
package e2e

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/skill"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func newRun(t *testing.T, userText string) *agent.Run {
	t.Helper()
	run := agent.NewRun(domain.NewSessionID(), domain.DefaultLimits(), domain.RealClock{})
	run.AddUserMessage(domain.Message{
		ID:        domain.NewMessageID(),
		Role:      domain.RoleUser,
		Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: userText}},
		CreatedAt: time.Now().UTC(),
	})
	return run
}

func systemPromptOf(t *testing.T, model *fakes.FakeModel, call int) string {
	t.Helper()
	calls := model.Calls()
	if len(calls) <= call {
		t.Fatalf("model received %d calls, want at least %d", len(calls), call+1)
	}
	first := calls[call].Messages[0]
	if first.Role != domain.RoleSystem {
		t.Fatalf("first message role = %s, want system", first.Role)
	}
	var sb strings.Builder
	for _, p := range first.Parts {
		sb.WriteString(p.Text)
	}
	return sb.String()
}

func transcriptText(run *agent.Run) string {
	var sb strings.Builder
	for _, m := range run.Messages {
		for _, p := range m.Parts {
			sb.WriteString(p.Text)
			if p.ToolResult != nil {
				for _, c := range p.ToolResult.Content {
					sb.WriteString(c.Text)
				}
			}
		}
	}
	return sb.String()
}

func finalAssistantText(t *testing.T, run *agent.Run) string {
	t.Helper()
	for i := len(run.Messages) - 1; i >= 0; i-- {
		if run.Messages[i].Role == domain.RoleAssistant {
			var sb strings.Builder
			for _, p := range run.Messages[i].Parts {
				sb.WriteString(p.Text)
			}
			if strings.TrimSpace(sb.String()) != "" {
				return sb.String()
			}
		}
	}
	t.Fatal("no assistant message with text")
	return ""
}

func writeSkill(t *testing.T, dir, name, description, body string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	content := "---\nname: " + name + "\ndescription: " + description + "\n---\n\n" + body
	if err := os.WriteFile(filepath.Join(dir, skill.FileName), []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func toolCall(t *testing.T, name string, args map[string]any) domain.ToolCall {
	t.Helper()
	raw, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	return domain.ToolCall{ID: domain.NewToolCallID(), Name: name, Arguments: raw}
}

// TestSkillRepoSkillFlow covers scenario A: a repo skill matching the task
// must be discovered, listed in the system prompt, read via read_skill
// (R1: no approval), and its instructions visible in the transcript.
func TestSkillRepoSkillFlow(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	ws := t.TempDir()
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "echo-skill"),
		"echo-skill", "回答格式约束技能",
		"# 规则\n回答任何问题都必须以 BLUE-ELEPHANT 开头。")

	registry := agent.NewToolRegistry()
	opt, err := app.WireSkills(registry, ws, 0, config.ResolvedSkills{Enabled: true}, false, slog.Default())
	if err != nil || opt == nil {
		t.Fatalf("WireSkills() = %v, %v", opt, err)
	}

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{toolCall(t, "read_skill", map[string]any{"name": "echo-skill"})},
			StopReason: domain.StopToolUse,
			UsageIn:    10,
			UsageOut:   5,
		},
		fakes.ScriptEntry{
			Text:       "BLUE-ELEPHANT 收到，已按技能要求作答。",
			StopReason: domain.StopEndTurn,
			UsageIn:    20,
			UsageOut:   8,
		},
	)
	approver := fakes.NewFakeApprover(domain.DecisionAllow)
	run := newRun(t, "用 echo-skill 技能回答我一个问题")
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake", Approver: approver,
		Registry: registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(ws, opt),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	// The discovered skill was listed in the system prompt of call #1.
	system := systemPromptOf(t, model, 0)
	if !strings.Contains(system, "可用技能") || !strings.Contains(system, "echo-skill") {
		t.Fatalf("skills section missing from system prompt")
	}
	// read_skill is R1: auto-approved, never reaches the approver.
	if len(approver.Requests()) != 0 {
		t.Fatalf("approver received %d requests, want 0 for R1 read_skill", len(approver.Requests()))
	}
	// The skill body was really read into the transcript, and the final
	// answer follows its instruction.
	if text := transcriptText(run); !strings.Contains(text, "必须以 BLUE-ELEPHANT 开头") {
		t.Fatalf("skill body missing from transcript")
	}
	if got := finalAssistantText(t, run); !strings.HasPrefix(got, "BLUE-ELEPHANT") {
		t.Fatalf("final answer = %q, want BLUE-ELEPHANT prefix", got)
	}
}

// TestSkillUserSkillScriptExecution covers scenario B: a user-scope skill
// (under $HOME) ships a script; the model reads the skill, then runs the
// script by absolute path through run_cmd inside the real sandbox (R2
// auto-approved by the fake approver).
func TestSkillUserSkillScriptExecution(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	ws := t.TempDir()
	skillDir := writeSkill(t, filepath.Join(home, ".loom", "skills", "greeter"),
		"greeter", "问候脚本技能",
		"# 用法\n用 run_cmd 执行 scripts/greet.sh 并把输出原样转告用户。")
	script := filepath.Join(skillDir, "scripts", "greet.sh")
	if err := os.MkdirAll(filepath.Dir(script), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(script, []byte("#!/bin/sh\necho GREETING-FROM-SKILL\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	validator, err := workspacepkg.NewPathValidator(ws)
	if err != nil {
		t.Fatal(err)
	}
	runner, err := process.NewRunner(validator, process.RunnerOptions{
		Sandbox: process.NewPlatformSandbox(process.PlatformSandboxOptions{}),
	})
	if err != nil {
		t.Fatal(err)
	}
	// Skip where the platform sandbox cannot run: unavailable (fail-closed
	// design) or nested sandbox-exec denied (e.g. under `bazel test`, which
	// already runs tests inside a darwin sandbox — macOS refuses a nested
	// sandbox_apply with EPERM).
	probe, probeErr := runner.Run(context.Background(), process.CommandSpec{
		Program: "true", Cwd: ws, Timeout: 5 * time.Second,
	})
	switch {
	case errors.Is(probeErr, process.ErrSandboxUnavailable):
		t.Skip("platform sandbox unavailable on this machine")
	case probeErr != nil:
		t.Fatalf("sandbox probe error = %v", probeErr)
	case probe.ExitCode != 0 || strings.Contains(string(probe.Stderr), "sandbox_apply"):
		t.Skipf("nested sandbox-exec not permitted (stderr=%q)", probe.Stderr)
	}
	runCmd, err := command.NewRunCmdTool(validator, runner)
	if err != nil {
		t.Fatal(err)
	}

	registry := agent.NewToolRegistry()
	if err := registry.Register(runCmd); err != nil {
		t.Fatal(err)
	}
	opt, err := app.WireSkills(registry, ws, 0, config.ResolvedSkills{Enabled: true}, false, slog.Default())
	if err != nil || opt == nil {
		t.Fatalf("WireSkills() = %v, %v", opt, err)
	}

	model := fakes.NewFakeModel(
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{toolCall(t, "read_skill", map[string]any{"name": "greeter"})},
			StopReason: domain.StopToolUse,
			UsageIn:    10,
			UsageOut:   5,
		},
		fakes.ScriptEntry{
			ToolCalls:  []domain.ToolCall{toolCall(t, "run_cmd", map[string]any{"program": script})},
			StopReason: domain.StopToolUse,
			UsageIn:    15,
			UsageOut:   6,
		},
		fakes.ScriptEntry{
			Text:       "脚本输出：GREETING-FROM-SKILL",
			StopReason: domain.StopEndTurn,
			UsageIn:    25,
			UsageOut:   9,
		},
	)
	approver := fakes.NewFakeApprover(domain.DecisionAllow)
	run := newRun(t, "用 greeter 技能问候我")
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake", Approver: approver,
		Registry: registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(ws, opt),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}

	// The user-scope skill was listed; its script executed for real inside
	// the sandbox and stdout reached the transcript.
	system := systemPromptOf(t, model, 0)
	if !strings.Contains(system, "greeter") {
		t.Fatalf("user skill missing from system prompt")
	}
	text := transcriptText(run)
	if !strings.Contains(text, "GREETING-FROM-SKILL") {
		t.Fatalf("script stdout missing from transcript")
	}
	if !strings.Contains(text, `"exit_code":0`) {
		t.Fatalf("run_cmd did not succeed: %s", text)
	}
	// Exactly one approval: run_cmd (R2). read_skill (R1) never prompts.
	if got := len(approver.Requests()); got != 1 {
		t.Fatalf("approver requests = %d, want 1 (run_cmd R2 only)", got)
	}
}

// TestSkillsDisabledFlow covers scenario C: skills.enabled=false means no
// read_skill tool and no skills section.
func TestSkillsDisabledFlow(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	ws := t.TempDir()
	writeSkill(t, filepath.Join(ws, ".loom", "skills", "echo-skill"),
		"echo-skill", "不应出现", "body")

	registry := agent.NewToolRegistry()
	opt, err := app.WireSkills(registry, ws, 0, config.ResolvedSkills{Enabled: false}, false, slog.Default())
	if err != nil {
		t.Fatal(err)
	}
	if opt != nil {
		t.Fatal("option = non-nil with skills.enabled=false")
	}
	if _, ok := registry.Lookup("read_skill"); ok {
		t.Fatal("read_skill registered with skills.enabled=false")
	}

	model := fakes.NewFakeModel(fakes.ScriptEntry{
		Text:       "好的。",
		StopReason: domain.StopEndTurn,
		UsageIn:    5,
		UsageOut:   2,
	})
	run := newRun(t, "你好")
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake",
		Approver: fakes.NewFakeApprover(domain.DecisionAllow),
		Registry: registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(ws),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if system := systemPromptOf(t, model, 0); strings.Contains(system, "可用技能") || strings.Contains(system, "echo-skill") {
		t.Fatalf("skills must not appear with LOOM_SKILLS=0")
	}
}
