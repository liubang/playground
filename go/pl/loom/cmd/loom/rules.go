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
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// loadCheckPolicy assembles the policy for the rules CLI: the full
// capability set plus the workspace derivation environment.
func loadCheckPolicy() (permission.Policy, error) {
	root, err := resolveWorkspace("")
	if err != nil {
		return permission.Policy{}, err
	}
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return permission.Policy{}, err
	}
	set := permission.NewPackageSet()
	permission.AttachPackages(context.Background(), set, root, resolved.Storage.RulesDir(), resolved.Rules.LoadOptions(), slog.Default())
	env := permission.DeriveEnv{
		Roots: append([]string{workspacepkg.Canonicalize(root)}, process.ExtraWritableDirs()...),
	}
	return permission.Policy{
		Packages: set,
		Env:      env,
		Mode:     resolved.Approval.Mode,
	}, nil
}

// listRules prints every effective package with its scope, so users can
// audit what the policy engine will do without running a command.
func listRules() error {
	policy, err := loadCheckPolicy()
	if err != nil {
		return err
	}
	pkgs := policy.Packages.Packages()
	if len(pkgs) == 0 {
		fmt.Println("no packages in effect (rules.enabled/rules.builtin may be disabled)")
		return nil
	}
	for _, p := range pkgs {
		just := ""
		if p.Justification != "" {
			just = " — " + p.Justification
		}
		grant := ""
		if g := p.Grant.ExecGrant(); !g.IsZero() {
			grant = " (" + g.Summary() + ")"
		}
		ceiling := ""
		if p.Decision == domain.DecisionAllow && p.MaxConsequence != permission.ConsequenceConfined {
			ceiling = " ≤" + p.MaxConsequence.String()
		}
		fmt.Printf("[%s] %-44s %s%s%s\n", p.Decision, bindingText(p.Bind)+grant+ceiling, p.Scope, sourceSuffix(p.Source), just)
	}
	return nil
}

// bindingText renders a binding for the list output.
func bindingText(b permission.Binding) string {
	switch b.Kind {
	case permission.BindArgv:
		return strings.Join(b.Argv, " ")
	case permission.BindArgvExact:
		return "exact:" + strings.Join(b.Argv, " ")
	case permission.BindHost:
		return "host:" + b.Host
	case permission.BindPath:
		return "path:" + b.Path
	case permission.BindTool:
		return "tool:" + b.Tool
	}
	return ""
}

// sourceSuffix renders the package source when it adds information
// beyond the scope (file paths, the remembered store).
func sourceSuffix(source string) string {
	if source == "" || source == "builtin" {
		return ""
	}
	return " <" + source + ">"
}

// checkRules is the dry-run inspector: it derives the effect of an
// invocation exactly like the policy path and prints the verdict,
// mirroring `codex execpolicy check`. Usage:
//
//	loom rules check [--escalated] [--needs-network] <program> [args...]
//	loom rules check --url https://example.com/x   (URL evaluation)
//	loom rules check --path /abs/path              (write evaluation)
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
	policy, err := loadCheckPolicy()
	if err != nil {
		return err
	}
	var call domain.PreparedCall
	switch {
	case fetchURL != "":
		argsJSON, _ := json.Marshal(map[string]string{"url": fetchURL})
		call = domain.PreparedCall{
			Call: domain.ToolCall{Name: "web_fetch", Arguments: argsJSON},
			Risk: domain.R3,
		}
	case writePath != "":
		argsJSON, _ := json.Marshal(map[string]string{"path": writePath})
		call = domain.PreparedCall{
			Call:         domain.ToolCall{Name: "write", Arguments: argsJSON},
			Risk:         domain.R2,
			WriteRequest: &domain.WriteRequest{Path: writePath, OutsideRoots: true},
		}
	default:
		if len(args) == 0 {
			return errors.New("usage: loom rules check [--escalated] [--needs-network] [--url URL] [--path PATH] <program> [args...]")
		}
		risk := domain.R2
		if escalated {
			risk = domain.R3
		}
		call = domain.PreparedCall{
			Call: domain.ToolCall{Name: "run_cmd"},
			Risk: risk,
			ExecRequest: &domain.ExecRequest{
				Argv:         args,
				Escalated:    escalated,
				NeedsNetwork: needsNetwork,
			},
		}
	}
	verdict := policy.Evaluate(call)
	d := policy.DeriveCall(call)
	fmt.Printf("decision: %s (source: %s)\n", verdict.Decision, verdict.Source)
	if !verdict.Grant.IsZero() {
		fmt.Printf("grant: %s\n", verdict.Grant.Summary())
	}
	if verdict.Reason != "" {
		fmt.Printf("reason: %s\n", verdict.Reason)
	}
	printEffect(d)
	if pkg, ok := policy.Packages.ExplainMatch(d); ok {
		fmt.Printf("matched package: %s -> %s (%s, %s)\n", bindingText(pkg.Bind), pkg.Decision, pkg.Scope, pkg.Source)
		if pkg.Justification != "" {
			fmt.Printf("justification: %s\n", pkg.Justification)
		}
	}
	return nil
}

// printEffect renders the derived effect for `loom rules check`.
func printEffect(d permission.Derivation) {
	e := d.Effect
	fmt.Printf("effect: consequence=%s proven=%v", e.Consequence, e.Proven)
	if !e.Proven && e.Reason != "" {
		fmt.Printf(" (%s)", e.Reason)
	}
	fmt.Println()
	if !e.Network.IsZero() {
		if e.Network.Any {
			fmt.Println("  network: any (underivable)")
		} else {
			fmt.Println("  network: " + strings.Join(e.Network.Hosts, ", "))
		}
	}
	if !e.Writes.IsZero() {
		if e.Writes.Any {
			fmt.Println("  writes: any (underivable)")
		} else {
			fmt.Println("  writes: " + strings.Join(e.Writes.Paths, ", "))
		}
	}
	if e.Unsandboxed {
		fmt.Println("  unsandboxed: true")
	}
	if e.GUIOpen {
		fmt.Println("  gui_open: true")
	}
	for _, ind := range e.Indicators {
		fmt.Println("  indicator: " + ind)
	}
}

// forgetRules removes a remembered approval from the SQLite store.
// Usage: loom rules forget [--host h | --tool name | --path dir | --exact] <program> [args...]
func forgetRules(argv []string) error {
	if len(argv) == 0 {
		return errors.New("usage: loom rules forget [--host h | --tool name | --path dir | --exact] <program> [args...]")
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
	var bind permission.Binding
	switch argv[0] {
	case "--host", "--domain":
		if len(argv) < 2 {
			return errors.New("--host requires a value")
		}
		bind = permission.Binding{Kind: permission.BindHost, Host: argv[1]}
	case "--tool":
		if len(argv) < 2 {
			return errors.New("--tool requires a value")
		}
		bind = permission.Binding{Kind: permission.BindTool, Tool: argv[1]}
	case "--path":
		if len(argv) < 2 {
			return errors.New("--path requires a value")
		}
		bind = permission.Binding{Kind: permission.BindPath, Path: argv[1]}
	case "--exact":
		if len(argv) < 2 {
			return errors.New("--exact requires an argv")
		}
		bind = permission.Binding{Kind: permission.BindArgvExact, Argv: argv[1:]}
	default:
		bind = permission.Binding{Kind: permission.BindArgv, Argv: argv}
	}
	ok, err := store.Forget(context.Background(), bind)
	if err != nil {
		return err
	}
	if !ok {
		fmt.Printf("no remembered package for %s\n", bindingText(bind))
	} else {
		fmt.Printf("forgot %s\n", bindingText(bind))
	}
	return nil
}

// migrateRules performs the one-time v2→v3 migration of the user rules
// directory (rule files, remembered.db, legacy remembered.json).
// Usage: loom rules migrate
func migrateRules() error {
	resolved, err := loadConfig(false, slog.Default())
	if err != nil {
		return err
	}
	report, err := permission.MigrateUserRules(context.Background(), resolved.Storage.RulesDir())
	if err != nil {
		return err
	}
	fmt.Println("migration complete:", report)
	return nil
}
