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
// Created: 2026/07/25

package app

import (
	"context"
	"encoding/json"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

func argsJSON(t *testing.T, command string) json.RawMessage {
	t.Helper()
	raw, err := json.Marshal(map[string]any{"command": command})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func preparedRunCmd(t *testing.T, command string) domain.PreparedCall {
	t.Helper()
	return domain.PreparedCall{
		Call: domain.ToolCall{
			ID:        domain.NewToolCallID(),
			Name:      "run_cmd",
			Arguments: argsJSON(t, command),
		},
	}
}

// staticPolicy returns a policy accessor over the given capability set
// (on-request mode, no roots) for the approver tests.
func staticPolicy(set *permission.PackageSet) func() permission.Policy {
	return func() permission.Policy {
		return permission.Policy{Packages: set, Mode: permission.ModeOnRequest}
	}
}

type recordingApprover struct {
	calls int
	last  domain.ApprovalRequest
}

func (r *recordingApprover) RequestApproval(ctx context.Context, req domain.ApprovalRequest) (domain.Decision, error) {
	r.calls++
	r.last = req
	return domain.DecisionAsk, nil
}

func TestRuleApproverAutoAllowsRememberedPrefix(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	rule, ok := rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "go test ./pl/loom/..."), "")
	if !ok {
		t.Fatal("expected go test to be persistable")
	}
	if rule.Label != "go test" {
		t.Fatalf("label = %q, want 'go test'", rule.Label)
	}

	// A matching later call is auto-approved without touching the user.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "go test ./go/pl/other/... -count=1"),
	})
	if err != nil || decision != domain.DecisionAllow {
		t.Fatalf("decision = %v err = %v, want allow", decision, err)
	}
	if inner.calls != 0 {
		t.Fatalf("inner approver was consulted for a rule-matching call")
	}

	// A boundary-crossing call the memory does not cover still reaches
	// the user (git push needs network + shared-state; the remembered
	// go test package answers neither).
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "git push"),
	})
	if err != nil || decision != domain.DecisionAsk {
		t.Fatalf("decision = %v err = %v, want delegated ask", decision, err)
	}
	if inner.calls != 1 {
		t.Fatalf("inner approver calls = %d, want 1", inner.calls)
	}
}

// TestRememberCompoundShell covers per-step memory for a static
// composed script: remembering `go test ./... && git status` stores one
// package per step, later scripts combining ONLY remembered steps
// auto-approve, and any script containing an unremembered step still
// reaches the user.
func TestRememberCompoundShell(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	rule, ok := rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "go test ./... && git status"), "")
	if !ok {
		t.Fatal("a static compound shell must be persistable")
	}
	if rule.Label != "go test && git status" {
		t.Fatalf("label = %q, want 'go test && git status'", rule.Label)
	}
	if len(rule.Packages) != 2 {
		t.Fatalf("packages = %d, want 2 (one per step)", len(rule.Packages))
	}

	// A later script combining only remembered steps auto-approves.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "git status && go test ./pl/..."),
	})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("remembered combination = %v err=%v inner=%d, want allow without prompting", decision, err, inner.calls)
	}

	// A boundary-crossing step that is NOT remembered keeps the whole
	// script at an ask (git push needs network + shared-state, which no
	// remembered package covers).
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "go test ./... && git push"),
	})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 1 {
		t.Fatalf("partially-remembered script = %v err=%v inner=%d, want delegated ask", decision, err, inner.calls)
	}

	// A fully-covered script after remembering the push too auto-approves.
	if _, ok := rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "git push"), ""); !ok {
		t.Fatal("git push must be persistable")
	}
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: preparedRunCmd(t, "go test ./... && git push"),
	})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 1 {
		t.Fatalf("fully-remembered script = %v err=%v inner=%d, want allow", decision, err, inner.calls)
	}
}

func TestRememberRunCmdRejectsNonPersistable(t *testing.T) {
	rules := NewRuleApprover(nil, staticPolicy(permission.NewPackageSet()), nil)
	for name, raw := range map[string]json.RawMessage{
		// A DYNAMIC shell script (variables, substitutions) cannot prove
		// its steps and stays unpersistable.
		"dynamic shell":  argsJSON(t, "echo hi > $out"),
		"eval":           argsJSON(t, `python3 -c "print(1)"`),
		"heredoc python": argsJSON(t, "python3 <<'EOF'\nimport os\nEOF"),
		"other tool":     json.RawMessage(`{"path":"x.go"}`),
	} {
		toolName := "run_cmd"
		if name == "other tool" {
			toolName = "edit"
		}
		if _, ok := rules.RememberCall(toolName, domain.ToolSourceBuiltin, raw, ""); ok {
			t.Fatalf("%s must not be persistable", name)
		}
	}
}

// TestRememberRunCmdGrantDerivation covers the grant flavors a
// remembered package carries: needs_network remembers a sandboxed
// network grant; escalated calls can ONLY be remembered as explicit L2
// full trust — a lesser grant would never cover the next escalation.
func TestRememberRunCmdGrantDerivation(t *testing.T) {
	rules := NewRuleApprover(nil, staticPolicy(permission.NewPackageSet()), nil)

	needsNet := json.RawMessage(`{"command":"mycli query submit","needs_network":true}`)
	rule, ok := rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, needsNet, "")
	if !ok || !rule.Packages[0].Grant.NetworkFull || rule.Packages[0].Grant.Unsandboxed {
		t.Fatalf("needs_network remember = ok=%v packages=%+v, want sandboxed network grant", ok, rule.Packages)
	}

	needsGUI := json.RawMessage(`{"command":"open https://example.com","needs_gui_open":true}`)
	rule, ok = rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, needsGUI, "")
	if !ok || !rule.Packages[0].Grant.GUIOpen || rule.Packages[0].Grant.Unsandboxed {
		t.Fatalf("needs_gui_open remember = ok=%v packages=%+v, want sandboxed gui_open grant", ok, rule.Packages)
	}

	escalated := json.RawMessage(`{"command":"make deploy","sandbox_permissions":"require_escalated"}`)
	// An escalated call has NO minimal flavor: the model layer refuses
	// it outright (not just the UI), so no code path can write a
	// categorical standing approval that under-grants the escalation.
	if _, ok = rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, escalated, ""); ok {
		t.Fatal("escalated minimal flavor must not be rememberable")
	}

	rules2 := NewRuleApprover(nil, staticPolicy(permission.NewPackageSet()), nil)
	rule, ok = rules2.RememberCall("run_cmd", domain.ToolSourceBuiltin, escalated, TrustUnsandboxed)
	if !ok || !rule.Packages[0].Grant.Unsandboxed {
		t.Fatalf("escalated remember (trust flavor) = ok=%v packages=%+v, want unsandboxed", ok, rule.Packages)
	}
	// The trust flavor is honored only for escalated calls.
	rule, ok = rules2.RememberCall("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "go build ./..."), TrustUnsandboxed)
	if !ok || !rule.Packages[0].Grant.IsZero() {
		t.Fatalf("non-escalated trust flavor = ok=%v packages=%+v, want zero grant", ok, rule.Packages)
	}
}

// TestRememberIndicatedExact covers the indicator gate's memory shape:
// a call carrying danger indicators is remembered by its EXACT argv,
// never a categorical prefix — and a later identical call auto-approves
// while a variant still asks.
func TestRememberIndicatedExact(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	sudoArgs := json.RawMessage(`{"command":"sudo rm -rf /tmp/x","sandbox_permissions":"require_escalated"}`)
	rule, ok := rules.RememberCall("run_cmd", domain.ToolSourceBuiltin, sudoArgs, TrustUnsandboxed)
	if !ok {
		t.Fatal("an indicated single command must be exactly memorable")
	}
	if len(rule.Packages) != 1 || rule.Packages[0].Bind.Kind != permission.BindArgvExact {
		t.Fatalf("indicated memory = %+v, want one exact-argv package", rule.Packages)
	}

	// The identical call auto-approves.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "run_cmd", Arguments: sudoArgs}},
	})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("exact-remembered call = %v err=%v inner=%d", decision, err, inner.calls)
	}
	// A variant still asks.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{
		Call: domain.PreparedCall{Call: domain.ToolCall{
			ID: domain.NewToolCallID(), Name: "run_cmd",
			Arguments: json.RawMessage(`{"command":"sudo rm -rf /tmp/y","sandbox_permissions":"require_escalated"}`),
		}},
	})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 1 {
		t.Fatalf("variant call = %v inner=%d, want delegated ask", decision, inner.calls)
	}
}

func TestApprovalRulePreview(t *testing.T) {
	env := permission.DeriveEnv{}
	preview, grant, ok := ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "go vet ./..."), env)
	if !ok || preview != "go vet" {
		t.Fatalf("preview = %q ok = %v, want 'go vet'", preview, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("grant = %+v, want zero", grant)
	}
	// A static compound shell previews one prefix per step; a DYNAMIC
	// script (unprovable argv) has no preview.
	preview, _, ok = ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "go test ./... && git status"), env)
	if !ok || preview != "go test && git status" {
		t.Fatalf("compound shell preview = %q ok=%v, want 'go test && git status'", preview, ok)
	}
	if _, _, ok := ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "echo hi > $out"), env); ok {
		t.Fatal("dynamic shell must not have a rule preview")
	}
	preview, _, ok = ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "ls -la"), env)
	if !ok || preview != "ls" {
		t.Fatalf("simple shell preview = %q ok=%v, want 'ls'", preview, ok)
	}
	// Escalated calls have no minimal-capability preview (only the
	// unsandboxed trust option, surfaced by RunCmdTrustPreview).
	if _, _, ok := ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, json.RawMessage(`{"command":"make deploy","sandbox_permissions":"require_escalated"}`), env); ok {
		t.Fatal("escalated calls must not have a minimal rule preview")
	}
	if _, _, ok := ApprovalRulePreview("edit", domain.ToolSourceBuiltin, json.RawMessage(`{}`), env); ok {
		t.Fatal("non-run_cmd must not have a rule preview")
	}

	// needs_network previews carry the network grant.
	_, grant, ok = ApprovalRulePreview("run_cmd", domain.ToolSourceBuiltin, json.RawMessage(`{"command":"mycli","needs_network":true}`), env)
	if !ok || !grant.NetworkFull {
		t.Fatalf("needs_network preview grant = %+v ok=%v", grant, ok)
	}

	// web_fetch previews show the exact host.
	preview, _, ok = ApprovalRulePreview("web_fetch", domain.ToolSourceBuiltin, json.RawMessage(`{"url":"https://WWW.weather.com.cn/weather/1.shtml"}`), env)
	if !ok || preview != "www.weather.com.cn" {
		t.Fatalf("web_fetch preview = %q ok=%v, want www.weather.com.cn", preview, ok)
	}
	if _, _, ok := ApprovalRulePreview("web_fetch", domain.ToolSourceBuiltin, json.RawMessage(`{"url":"ftp://x/y"}`), env); ok {
		t.Fatal("non-http URLs must not have a rule preview")
	}
}

// TestRememberWebFetchDomain covers the host memory for web_fetch
// approvals: exact host, and auto-approval on later calls.
func TestRememberWebFetchDomain(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	rule, ok := rules.RememberCall("web_fetch", domain.ToolSourceBuiltin, json.RawMessage(`{"url":"https://www.weather.com.cn/a"}`), "")
	if !ok || rule.Label != "www.weather.com.cn" {
		t.Fatalf("remember = %+v ok=%v", rule, ok)
	}

	fetch := func(url string) domain.PreparedCall {
		raw, _ := json.Marshal(map[string]string{"url": url})
		return domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "web_fetch", Arguments: raw}}
	}
	// Same host (different path, case) auto-approves.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: fetch("https://WWW.weather.com.cn/other")})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("remembered host: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
	// A different host still prompts.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: fetch("https://api.weather.com.cn/")})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 1 {
		t.Fatalf("unremembered host: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
}

func TestRunCmdTrustPreview(t *testing.T) {
	env := permission.DeriveEnv{}
	escalated := json.RawMessage(`{"command":"make deploy","sandbox_permissions":"require_escalated"}`)
	if !RunCmdTrustPreview("run_cmd", domain.ToolSourceBuiltin, escalated, env) {
		t.Fatal("escalated run_cmd must offer the trust option")
	}
	if RunCmdTrustPreview("run_cmd", domain.ToolSourceBuiltin, argsJSON(t, "make build"), env) {
		t.Fatal("non-escalated calls must not offer the trust option")
	}
	// A static compound shell offers the trust option (one trusted
	// prefix per step); a dynamic one does not.
	if !RunCmdTrustPreview("run_cmd", domain.ToolSourceBuiltin, json.RawMessage(`{"command":"make deploy && echo done","sandbox_permissions":"require_escalated"}`), env) {
		t.Fatal("static compound shells must offer the trust option")
	}
	if RunCmdTrustPreview("run_cmd", domain.ToolSourceBuiltin, json.RawMessage(`{"command":"make $TARGET","sandbox_permissions":"require_escalated"}`), env) {
		t.Fatal("dynamic compound shells must not offer the trust option")
	}
	if RunCmdTrustPreview("edit", domain.ToolSourceBuiltin, json.RawMessage(`{}`), env) {
		t.Fatal("non-run_cmd must not offer the trust option")
	}
}

// TestRememberToolByName covers tool-name memory: eligible tools
// (generate_image) are remembered categorically and auto-approved on
// later calls; path/host-varying tools (edit) stay per-call.
func TestRememberToolByName(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	genArgs := json.RawMessage(`{"prompt":"a cat"}`)
	rule, ok := rules.RememberCall("generate_image", domain.ToolSourceBuiltin, genArgs, "")
	if !ok || rule.Label != "generate_image" {
		t.Fatalf("remember = %+v ok=%v, want tool generate_image", rule, ok)
	}

	genCall := domain.PreparedCall{Call: domain.ToolCall{ID: domain.NewToolCallID(), Name: "generate_image", Arguments: genArgs}}
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: genCall})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("remembered tool: decision=%v err=%v inner=%d, want allow without prompting", decision, err, inner.calls)
	}

	// Ineligible tools are never remembered and keep prompting.
	if _, ok := rules.RememberCall("edit", domain.ToolSourceBuiltin, json.RawMessage(`{"path":"x.go"}`), ""); ok {
		t.Fatal("edit must not be rememberable by tool name")
	}
	if _, ok := rules.RememberCall("view_image", domain.ToolSourceBuiltin, json.RawMessage(`{"path":"x.png"}`), ""); ok {
		t.Fatal("view_image must not be rememberable by tool name")
	}
}

// TestRememberWritePath covers path memory for boundary-crossing
// writes: the target's parent directory is remembered and later writes
// anywhere under it auto-approve; writes elsewhere keep prompting.
func TestRememberWritePath(t *testing.T) {
	inner := &recordingApprover{}
	rules := NewRuleApprover(inner, staticPolicy(permission.NewPackageSet()), nil)

	dir := t.TempDir()
	target := filepath.Join(dir, "notes", "a.txt")
	args := json.RawMessage(`{"path":` + strconv.Quote(target) + `,"content":"x"}`)
	rule, ok := rules.RememberCall("write", domain.ToolSourceBuiltin, args, "")
	wantDir := workspacepkg.Canonicalize(filepath.Join(dir, "notes"))
	if !ok || rule.Label != wantDir {
		t.Fatalf("remember = %+v ok=%v, want path %q", rule, ok, wantDir)
	}

	write := func(path string) domain.PreparedCall {
		raw, _ := json.Marshal(map[string]string{"path": path})
		return domain.PreparedCall{
			Call:         domain.ToolCall{ID: domain.NewToolCallID(), Name: "write", Arguments: raw},
			WriteRequest: &domain.WriteRequest{Path: workspacepkg.Canonicalize(path), OutsideRoots: true},
		}
	}
	// Under the remembered directory: auto-approve without prompting.
	decision, err := rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: write(filepath.Join(dir, "notes", "b.txt"))})
	if err != nil || decision != domain.DecisionAllow || inner.calls != 0 {
		t.Fatalf("remembered dir: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
	// Anywhere else still prompts.
	decision, err = rules.RequestApproval(context.Background(), domain.ApprovalRequest{Call: write(filepath.Join(t.TempDir(), "c.txt"))})
	if err != nil || decision != domain.DecisionAsk || inner.calls != 1 {
		t.Fatalf("unremembered dir: decision=%v err=%v inner=%d", decision, err, inner.calls)
	}
}

func TestApprovalRulePreviewWritePath(t *testing.T) {
	target := filepath.Join(t.TempDir(), "notes", "a.txt")
	preview, _, ok := ApprovalRulePreview("write", domain.ToolSourceBuiltin, json.RawMessage(`{"path":`+strconv.Quote(target)+`,"content":"x"}`), permission.DeriveEnv{})
	want := workspacepkg.Canonicalize(filepath.Dir(target))
	if !ok || preview != want {
		t.Fatalf("write preview = %q ok=%v, want %q", preview, ok, want)
	}
	// Workspace-relative paths are confined writes: no approval, no preview.
	if _, _, ok := ApprovalRulePreview("write", domain.ToolSourceBuiltin, json.RawMessage(`{"path":"x.go"}`), permission.DeriveEnv{}); ok {
		t.Fatal("relative write paths must not have a path rule preview")
	}
}

func TestApprovalRulePreviewTool(t *testing.T) {
	preview, grant, ok := ApprovalRulePreview("generate_image", domain.ToolSourceBuiltin, json.RawMessage(`{"prompt":"a cat"}`), permission.DeriveEnv{})
	if !ok || preview != "generate_image" {
		t.Fatalf("preview = %q ok=%v, want generate_image", preview, ok)
	}
	if !grant.IsZero() {
		t.Fatalf("grant = %+v, want zero", grant)
	}
	// The preview is the canonical (normalized) tool name.
	preview, _, ok = ApprovalRulePreview("Generate_Image", domain.ToolSourceBuiltin, json.RawMessage(`{"prompt":"a cat"}`), permission.DeriveEnv{})
	if !ok || preview != "generate_image" {
		t.Fatalf("preview = %q ok=%v, want canonical generate_image", preview, ok)
	}
	for _, name := range []string{"edit", "write", "view_image", "websearch"} {
		if _, _, ok := ApprovalRulePreview(name, domain.ToolSourceBuiltin, json.RawMessage(`{}`), permission.DeriveEnv{}); ok {
			t.Errorf("%s must not have a tool-name rule preview", name)
		}
	}
}
