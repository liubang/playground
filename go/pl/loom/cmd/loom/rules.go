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
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/permission"
	"github.com/liubang/playground/go/pl/loom/internal/process"
)

// listRules prints every effective rule with its layer, so users can audit
// what the policy engine will do without running a command.
func listRules() error {
	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	policy := permission.AttachRules(context.Background(), permission.DefaultPolicy(), root, resolved.Storage.RulesDir(), resolved.Rules.LoadOptions(), slog.Default())
	rules := policy.Rules.Rules()
	domains := policy.Rules.Domains()
	tools := policy.Rules.Tools()
	paths := policy.Rules.Paths()
	if len(rules) == 0 && len(domains) == 0 && len(tools) == 0 && len(paths) == 0 {
		fmt.Println("no rules in effect (rules.enabled/rules.builtin may be disabled)")
		return nil
	}
	for _, r := range rules {
		just := ""
		if r.Justification != "" {
			just = " — " + r.Justification
		}
		grant := ""
		if g := r.Grant.ExecGrant(); !g.IsZero() {
			grant = " (" + g.Summary() + ")"
		}
		fmt.Printf("[%s] %-40s %s%s\n", r.Decision, strings.Join(r.ArgvPrefix, " ")+grant, r.Source, just)
	}
	for _, d := range domains {
		just := ""
		if d.Justification != "" {
			just = " — " + d.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", d.Decision, "host:"+d.Host, d.Source, just)
	}
	for _, t := range tools {
		just := ""
		if t.Justification != "" {
			just = " — " + t.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", t.Decision, "tool:"+t.Name, t.Source, just)
	}
	for _, p := range paths {
		just := ""
		if p.Justification != "" {
			just = " — " + p.Justification
		}
		fmt.Printf("[%s] %-40s %s%s\n", p.Decision, "path:"+p.Path, p.Source, just)
	}
	return nil
}

// checkRules is the dry-run inspector for the declarative rule engine: it
// evaluates an argv exactly like the run_cmd policy path and prints the
// verdict with the matching rule (if any), mirroring `codex execpolicy
// check`. Usage:
//
//	loom rules check [--escalated] [--needs-network] <program> [args...]
//	loom rules check --url https://example.com/x   (web_fetch evaluation)
func checkRules(argv []string) error {
	var (
		escalated    bool
		needsNetwork bool
		fetchURL     string
		writePath    string
		args         []string
	)
	for i := 0; i < len(argv); i++ {
		switch argv[i] {
		case "--escalated":
			escalated = true
		case "--needs-network":
			needsNetwork = true
		case "--url":
			if i+1 >= len(argv) {
				return errors.New("--url requires a value")
			}
			i++
			fetchURL = argv[i]
		case "--path":
			if i+1 >= len(argv) {
				return errors.New("--path requires a value")
			}
			i++
			writePath = argv[i]
		default:
			args = append(args, argv[i])
		}
	}
	root, err := resolveWorkspace("")
	if err != nil {
		return err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	policy := permission.AttachRules(context.Background(), permission.DefaultPolicy(), root, resolved.Storage.RulesDir(), resolved.Rules.LoadOptions(), slog.Default())
	if fetchURL != "" {
		return checkFetchURL(policy, resolved.Approval.Mode, fetchURL)
	}
	if writePath != "" {
		return checkWritePath(policy, resolved.Approval.Mode, writePath)
	}
	if len(args) == 0 {
		return errors.New("usage: loom rules check [--escalated] [--needs-network] [--url URL] [--path PATH] <program> [args...]")
	}
	argv = args
	callArgs := map[string]any{"program": argv[0], "args": argv[1:]}
	risk := domain.R2
	if escalated {
		callArgs["sandbox_permissions"] = "require_escalated"
		callArgs["justification"] = "dry run"
		risk = domain.R3
	}
	if needsNetwork {
		callArgs["needs_network"] = true
	}
	argsJSON, _ := json.Marshal(callArgs)
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "run_cmd", Arguments: argsJSON},
		Risk: risk,
	}
	decider := policy.Decider(resolved.Approval.Mode)
	verdict := decider.Evaluate(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if !verdict.Grant.IsZero() {
		fmt.Printf("grant: %s\n", verdict.Grant.Summary())
	}
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	if process.IsShellProgram(argv[0]) {
		fmt.Println("note: shell scripts are evaluated per subcommand via AST analysis (pipes/redirects included)")
	}
	if ruleArgv, ok := permission.RunCmdArgv(argsJSON); ok {
		rule := permission.MatchRule(policy.Rules, ruleArgv)
		via := ""
		if rule.Source == "" {
			if norm, ok := permission.NormalizeTrustedPath(ruleArgv); ok {
				rule = permission.MatchRule(policy.Rules, norm)
				via = " (via trusted basename " + norm[0] + ")"
			}
		}
		if rule.Source != "" {
			source := rule.Source
			if source == "builtin" {
				source = "builtin (embedded read-only set)"
			}
			fmt.Printf("matched rule: %v -> %s (%s)%s\n", rule.ArgvPrefix, rule.Decision, source, via)
			if rule.Justification != "" {
				fmt.Printf("justification: %s\n", rule.Justification)
			}
		}
	}
	return nil
}

// checkFetchURL evaluates a web_fetch call against the domain rules and
// prints the verdict — the web_fetch counterpart of checkRules.
func checkFetchURL(policy permission.Policy, mode permission.ApprovalMode, rawURL string) error {
	argsJSON, _ := json.Marshal(map[string]string{"url": rawURL})
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "web_fetch", Arguments: argsJSON},
		Risk: domain.R3,
	}
	verdict := policy.Decider(mode).Evaluate(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	if host, ok := permission.ParseWebFetchHost(argsJSON); ok {
		if _, rule := policy.Rules.EvaluateDomain(host); rule.Source != "" {
			fmt.Printf("matched rule: host:%s -> %s (%s)\n", rule.Host, rule.Decision, rule.Source)
			if rule.Justification != "" {
				fmt.Printf("justification: %s\n", rule.Justification)
			}
		}
	}
	return nil
}

// checkWritePath evaluates a boundary-crossing file write against the
// writable-path rules and prints the verdict — the write counterpart of
// checkFetchURL.
func checkWritePath(policy permission.Policy, mode permission.ApprovalMode, path string) error {
	argsJSON, _ := json.Marshal(map[string]string{"path": path})
	call := domain.PreparedCall{
		Call: domain.ToolCall{Name: "write", Arguments: argsJSON},
		Risk: domain.R2,
	}
	verdict := policy.Decider(mode).Evaluate(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	if _, rule := policy.Rules.EvaluatePath(path); rule.Source != "" {
		fmt.Printf("matched rule: path:%s -> %s (%s)\n", rule.Path, rule.Decision, rule.Source)
		if rule.Justification != "" {
			fmt.Printf("justification: %s\n", rule.Justification)
		}
	}
	return nil
}

// forgetRules removes a remembered approval from the SQLite store.
// Usage: loom rules forget [--domain host | --tool name | --path dir] <program> [args...]
func forgetRules(argv []string) error {
	if len(argv) == 0 {
		return errors.New("usage: loom rules forget [--domain host | --tool name | --path dir] <program> [args...]")
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	store, err := permission.OpenRememberedStore(context.Background(), permission.RememberedDBPath(resolved.Storage.RulesDir()))
	if err != nil {
		return fmt.Errorf("open remembered store: %w", err)
	}
	defer store.Close()
	if argv[0] == "--domain" || argv[0] == "--tool" || argv[0] == "--path" {
		if len(argv) < 2 || strings.HasPrefix(argv[1], "--") {
			return fmt.Errorf("%s requires a value\nusage: loom rules forget [--domain host | --tool name | --path dir] <program> [args...]", argv[0])
		}
		if argv[0] == "--domain" {
			host := argv[1]
			ok, err := store.ForgetDomain(context.Background(), host)
			if err != nil {
				return err
			}
			if !ok {
				fmt.Printf("no remembered domain for %q\n", host)
			} else {
				fmt.Printf("forgot domain %q\n", host)
			}
			return nil
		}
		if argv[0] == "--path" {
			dir := argv[1]
			ok, err := store.ForgetPath(context.Background(), dir)
			if err != nil {
				return err
			}
			if !ok {
				fmt.Printf("no remembered path rule for %q\n", dir)
			} else {
				fmt.Printf("forgot path rule %q\n", dir)
			}
			return nil
		}
		name := argv[1]
		ok, err := store.ForgetTool(context.Background(), name)
		if err != nil {
			return err
		}
		if !ok {
			fmt.Printf("no remembered tool rule for %q\n", name)
		} else {
			fmt.Printf("forgot tool rule %q\n", name)
		}
		return nil
	}
	// argv prefix rule
	ok, err := store.ForgetRule(context.Background(), argv)
	if err != nil {
		return err
	}
	if !ok {
		fmt.Printf("no remembered rule for %v\n", argv)
	} else {
		fmt.Printf("forgot rule %v\n", argv)
	}
	return nil
}

// importRules imports a declarative rule file (the user-layer JSON schema)
// into the remembered store. Existing store entries win; the file itself is
// left untouched (rename it aside manually to complete a migration).
// Usage: loom rules import <file.json>
func importRules(path string) error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	ctx := context.Background()
	store, err := permission.OpenRememberedStore(ctx, permission.RememberedDBPath(resolved.Storage.RulesDir()))
	if err != nil {
		return fmt.Errorf("open remembered store: %w", err)
	}
	defer store.Close()
	if err := store.ImportRuleFile(ctx, path); err != nil {
		return fmt.Errorf("import %s: %w", path, err)
	}
	fmt.Printf("imported allow rules from %s into %s (existing entries kept)\n", path, store.Path())
	return nil
}
