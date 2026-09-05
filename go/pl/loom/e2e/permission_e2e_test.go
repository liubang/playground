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
// Created: 2026/07/31

package e2e

import (
	"context"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
	"github.com/liubang/playground/go/pl/loom/internal/prompt"
	"github.com/liubang/playground/go/pl/loom/internal/tool/command"
	"github.com/liubang/playground/go/pl/loom/internal/tool/edit"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// permissionFixture wires the REAL process sandbox + run_cmd/write tools +
// the live decision pipeline into an agent loop driven by a
// scripted FakeModel — the same shape as the skills e2e tests.
type permissionFixture struct {
	ws        string
	home      string
	registry  *agent.ToolRegistry
	validator *workspacepkg.PathValidator
}

func newPermissionFixture(t *testing.T) *permissionFixture {
	t.Helper()
	// The sandbox treats the system scratch dirs ($TMPDIR, /tmp) as
	// writable and the policy roots mirror that (ExtraWritableDirs), so a
	// plain t.TempDir()-based HOME would land INSIDE the writable roots and
	// every "external" write asserted below would silently count as
	// confined. Redirect $TMPDIR into a private scratch and make HOME a
	// SIBLING of that scratch: HOME then lies genuinely outside the
	// writable roots while the workspace still sits under scratch.
	base := t.TempDir()
	scratch := filepath.Join(base, "tmp")
	home := filepath.Join(base, "home")
	ws := filepath.Join(base, "ws")
	for _, dir := range []string{scratch, home, ws} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("TMPDIR", scratch)
	t.Setenv("HOME", home)
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
	// Same guard as the skills e2e: nested sandbox-exec is refused under
	// some test runners; skip there instead of failing.
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
	writeTool, err := edit.NewWriteTool(validator)
	if err != nil {
		t.Fatal(err)
	}
	registry := agent.NewToolRegistry()
	if err := registry.Register(runCmd); err != nil {
		t.Fatal(err)
	}
	if err := registry.Register(writeTool); err != nil {
		t.Fatal(err)
	}
	return &permissionFixture{ws: ws, home: home, registry: registry, validator: validator}
}

// drive runs one scripted turn under the given policy and returns the
// finished run plus the approver's request log.
func (f *permissionFixture) drive(t *testing.T, policy permission.Policy, mode permission.ApprovalMode, call domain.ToolCall) (*agent.Run, *fakes.FakeApprover) {
	t.Helper()
	model := fakes.NewFakeModel(
		fakes.ScriptEntry{ToolCalls: []domain.ToolCall{call}, StopReason: domain.StopToolUse, UsageIn: 10, UsageOut: 5},
		fakes.ScriptEntry{Text: "done", StopReason: domain.StopEndTurn, UsageIn: 20, UsageOut: 8},
	)
	approver := fakes.NewFakeApprover(domain.DecisionAllow)
	run := newRun(t, "run the scripted step")
	policy.Mode = mode
	policy.Env = permission.DeriveEnv{
		Roots: append([]string{f.validator.Root()}, process.ExtraWritableDirs()...),
	}
	loop := &agent.Loop{
		Run: run, Model: model, ModelName: "fake", Approver: approver,
		Policy:   policy,
		Registry: f.registry, Logger: slog.Default(),
		SystemPrompt: prompt.NewBuilder(f.ws),
	}
	if err := loop.Execute(context.Background()); err != nil {
		t.Fatalf("Execute() error = %v", err)
	}
	if run.State.Outcome != domain.OutcomeSucceeded {
		t.Fatalf("outcome = %s, want succeeded", run.State.Outcome)
	}
	return run, approver
}

// TestEscalationNeverSilentlyDowngraded reproduces the sess_09538cef
// failure mode end to end: a session memory carrying only a network grant
// must NOT answer a require_escalated call: on-request keeps the ask.
func TestEscalationNeverSilentlyDowngraded(t *testing.T) {
	f := newPermissionFixture(t)
	outside := filepath.Join(f.home, "escalation-proof")

	// A session package carrying only a network grant for this prefix
	// must NOT answer a require_escalated call.
	policy := permission.DefaultPolicy()
	policy.Packages.RememberSession(permission.Package{
		Bind:           permission.Binding{Kind: permission.BindArgv, Argv: []string{"touch", outside}},
		Decision:       domain.DecisionAllow,
		Grant:          permission.PackageGrant{NetworkFull: true},
		MaxConsequence: permission.ConsequenceConfined,
	})

	run, approver := f.drive(t, policy, permission.ModeOnRequest, toolCall(t, "run_cmd", map[string]any{
		"command":             "touch " + outside,
		"sandbox_permissions": "require_escalated",
		"justification":       "needs to write outside the workspace",
	}))

	if got := len(approver.Requests()); got != 1 {
		t.Fatalf("approver requests = %d, want 1 (the lesser grant must not silently cover the escalation)", got)
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatalf("escalated command did not run unsandboxed: %v", err)
	}
	if text := transcriptText(run); !strings.Contains(text, process.ProcessGroupIsolation.Name()) {
		t.Fatalf("transcript missing unsandboxed isolation marker:\n%s", text)
	}
}

// TestDangerOnlyBaselineFlow proves the danger-only baseline end
// to end: everything without a danger signal runs without a prompt,
// while the danger list keeps asking.
func TestDangerOnlyBaselineFlow(t *testing.T) {
	policy := permission.DefaultPolicy()

	t.Run("simple command needs no approval", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "mkdir -p .myapp_logs",
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0", got)
		}
		if info, err := os.Stat(filepath.Join(f.ws, ".myapp_logs")); err != nil || !info.IsDir() {
			t.Fatalf("simple command did not execute: %v", err)
		}
	})

	t.Run("compound command needs no approval", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "mkdir -p .myapp_logs && echo created",
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0 (composition is not a risk; the sandbox confines the script)", got)
		}
		if _, err := os.Stat(filepath.Join(f.ws, ".myapp_logs")); err != nil {
			t.Fatalf("compound command did not execute: %v", err)
		}
	})

	t.Run("danger-listed compound command still asks", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "echo hi && sudo make install",
		}))
		if got := len(approver.Requests()); got != 1 {
			t.Fatalf("approver requests = %d, want 1 (the AST danger screen must see inside the script)", got)
		}
	})

	t.Run("declared network need is granted without a prompt", func(t *testing.T) {
		f := newPermissionFixture(t)
		run, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command":       "ls",
			"needs_network": true,
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0", got)
		}
		if text := transcriptText(run); !strings.Contains(text, `"exit_code":0`) {
			t.Fatalf("needs_network command did not succeed: %s", text)
		}
	})

	t.Run("workspace write tool needs no approval", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "write", map[string]any{
			"path":    "note.txt",
			"content": "hello",
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0", got)
		}
		data, err := os.ReadFile(filepath.Join(f.ws, "note.txt"))
		if err != nil || string(data) != "hello" {
			t.Fatalf("write tool did not create the file: %v %q", err, data)
		}
	})

	// A write outside the workspace roots crosses the confinement
	// boundary: it prompts in the default on-request mode (danger-only
	// auto-allows it), and the approval executes the write.
	t.Run("external write asks and executes on approval", func(t *testing.T) {
		f := newPermissionFixture(t)
		outside := filepath.Join(f.home, "notes", "a.txt")
		_, approver := f.drive(t, policy, permission.ModeOnRequest, toolCall(t, "write", map[string]any{
			"path":    outside,
			"content": "external",
		}))
		if got := len(approver.Requests()); got != 1 {
			t.Fatalf("approver requests = %d, want 1 (boundary-crossing writes always ask)", got)
		}
		data, err := os.ReadFile(outside)
		if err != nil || string(data) != "external" {
			t.Fatalf("approved external write did not land: %v %q", err, data)
		}
	})

	// A writable-path package answers the same call without prompting.
	t.Run("path package auto-allows external write", func(t *testing.T) {
		f := newPermissionFixture(t)
		outside := filepath.Join(f.home, "notes", "b.txt")
		rulesDir := t.TempDir()
		ruleJSON := `{"packages":[{"bind":{"path":` + strconv.Quote(filepath.Join(f.home, "notes")) + `},"decision":"allow","justification":"notes vault"}]}`
		if err := os.WriteFile(filepath.Join(rulesDir, "paths.json"), []byte(ruleJSON), 0o644); err != nil {
			t.Fatal(err)
		}
		pkgs, errs := permission.LoadPackageSets(rulesDir, "", permission.LoadOptions{})
		if len(errs) != 0 {
			t.Fatalf("load packages: %v", errs)
		}
		rulePolicy := permission.DefaultPolicy()
		rulePolicy.Packages.Add(pkgs...)

		_, approver := f.drive(t, rulePolicy, permission.ModeDangerOnly, toolCall(t, "write", map[string]any{
			"path":    outside,
			"content": "ruled",
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0 (the path rule covers the write)", got)
		}
		if data, err := os.ReadFile(outside); err != nil || string(data) != "ruled" {
			t.Fatalf("rule-covered external write did not land: %v %q", err, data)
		}
	})

	// Sensitive locations never reach the approver: the write tool refuses
	// at Prepare, before any policy evaluation.
	t.Run("sensitive external write fails before approval", func(t *testing.T) {
		f := newPermissionFixture(t)
		run, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "write", map[string]any{
			"path":    filepath.Join(f.home, ".ssh", "config"),
			"content": "Host evil",
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0 (sensitive paths must fail at Prepare)", got)
		}
		if text := transcriptText(run); !strings.Contains(text, "sensitive") {
			t.Fatalf("transcript missing the sensitive-location denial:\n%s", text)
		}
	})

	t.Run("danger-listed commands still ask", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "git reset --hard",
		}))
		if got := len(approver.Requests()); got != 1 {
			t.Fatalf("approver requests = %d, want 1 (git reset --hard is danger-listed)", got)
		}
	})

	t.Run("danger screen sees through the shell", func(t *testing.T) {
		f := newPermissionFixture(t)
		_, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "sudo echo hi",
		}))
		if got := len(approver.Requests()); got != 1 {
			t.Fatalf("approver requests = %d, want 1 (sudo is danger-listed)", got)
		}
	})

	t.Run("skill env namespace survives the sandbox filter", func(t *testing.T) {
		f := newPermissionFixture(t)
		run, approver := f.drive(t, policy, permission.ModeDangerOnly, toolCall(t, "run_cmd", map[string]any{
			"command": "printenv SKILL_REGION",
			"env":     map[string]string{"SKILL_REGION": "cn", "NODE_OPTIONS": "--no-warnings"},
		}))
		if got := len(approver.Requests()); got != 0 {
			t.Fatalf("approver requests = %d, want 0", got)
		}
		text := transcriptText(run)
		if !strings.Contains(text, "cn") {
			t.Fatalf("SKILL_REGION did not reach the sandboxed command: %s", text)
		}
		if strings.Contains(text, "env keys dropped") {
			t.Fatalf("SKILL_*/NODE_OPTIONS must not be dropped by the allowlist: %s", text)
		}
	})
}
