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
// Created: 2026/08/12

package permission

import (
	"context"
	"encoding/json"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	workspacepkg "github.com/liubang/playground/go/pl/loom/internal/workspace"
)

// writeCall builds a prepared write-tool call with the typed WriteRequest
// the producing tools sign during Prepare.
func writeCall(path string, outside bool) domain.PreparedCall {
	args, _ := json.Marshal(map[string]string{"path": path, "content": "x"})
	return domain.PreparedCall{
		Call:         domain.ToolCall{Name: "write", Arguments: args},
		Risk:         domain.R2,
		WritePaths:   []string{workspacepkg.Canonicalize(path)},
		WriteRequest: &domain.WriteRequest{Path: workspacepkg.Canonicalize(path), OutsideRoots: outside},
	}
}

func TestValidatePathRule(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)

	good := t.TempDir()
	r := PathRule{Path: good, Decision: "allow"}
	if err := validatePathRule(&r); err != nil {
		t.Fatalf("validatePathRule() error = %v", err)
	}
	if r.Path != workspacepkg.Canonicalize(good) {
		t.Fatalf("rule path = %q, want canonical %q", r.Path, workspacepkg.Canonicalize(good))
	}

	// ~ expands against $HOME.
	r = PathRule{Path: "~/notes", Decision: "allow"}
	if err := validatePathRule(&r); err != nil {
		t.Fatalf("validatePathRule(~) error = %v", err)
	}
	if want := workspacepkg.Canonicalize(filepath.Join(home, "notes")); r.Path != want {
		t.Fatalf("rule path = %q, want %q", r.Path, want)
	}

	for _, bad := range []PathRule{
		{Path: "relative/dir", Decision: "allow"},               // not absolute
		{Path: "/", Decision: "allow"},                          // filesystem root
		{Path: filepath.Join(home, ".ssh"), Decision: "allow"},  // sensitive subpath
		{Path: filepath.Join(home, ".netrc"), Decision: "deny"}, // sensitive literal
		{Path: filepath.Join(good, ".env"), Decision: "allow"},  // sensitive component
		{Path: good, Decision: "yolo"},                          // bad decision
	} {
		if err := validatePathRule(&bad); err == nil {
			t.Errorf("validatePathRule(%q, %q) must fail", bad.Path, bad.Decision)
		}
	}
}

func TestEvaluatePath(t *testing.T) {
	dir := t.TempDir()
	allow := workspacepkg.Canonicalize(dir)
	denySub := filepath.Join(allow, "secret")
	set := &RuleSet{paths: []PathRule{
		{Path: allow, Decision: "allow", Justification: "notes vault"},
		{Path: denySub, Decision: "deny", Justification: "never here"},
	}}

	// Subpath semantics: a directory rule covers everything beneath it.
	if d, _ := set.EvaluatePath(filepath.Join(dir, "a", "b.txt")); d != domain.DecisionAllow {
		t.Fatalf("EvaluatePath(subpath) = %s, want allow", d)
	}
	// Exact match on the rule path itself.
	if d, _ := set.EvaluatePath(dir); d != domain.DecisionAllow {
		t.Fatalf("EvaluatePath(exact) = %s, want allow", d)
	}
	// Strictest wins: the deny on the subdirectory beats the parent allow.
	if d, r := set.EvaluatePath(filepath.Join(denySub, "key.pem")); d != domain.DecisionDeny || r.Justification != "never here" {
		t.Fatalf("EvaluatePath(denied subdir) = %s (%q), want deny", d, r.Justification)
	}
	// No match outside every rule.
	if d, _ := set.EvaluatePath(filepath.Join(t.TempDir(), "x.txt")); d != "" {
		t.Fatalf("EvaluatePath(unrelated) = %s, want empty", d)
	}
}

// Project-layer path rules are tighten-only, same as argv and domain rules.
func TestLoadRuleSetsPathsTightenOnly(t *testing.T) {
	user := t.TempDir()
	project := t.TempDir()
	allowDir := t.TempDir()
	writeRulesFile(t, user, "u.json", `{"paths":[
		{"path": "`+filepath.ToSlash(allowDir)+`", "decision": "allow", "justification": "user allow"}
	]}`)
	writeRulesFile(t, project, "p.json", `{"paths":[
		{"path": "`+filepath.ToSlash(allowDir)+`", "decision": "allow"},
		{"path": "`+filepath.ToSlash(t.TempDir())+`", "decision": "deny", "justification": "project deny"}
	]}`)

	set, errs := LoadRuleSets(user, project, LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("errs = %v", errs)
	}
	paths := set.Paths()
	if len(paths) != 2 {
		t.Fatalf("loaded %d path rules, want 2 (project allow dropped): %+v", len(paths), paths)
	}
	if d, _ := set.EvaluatePath(filepath.Join(allowDir, "x.txt")); d != domain.DecisionAllow {
		t.Fatalf("user-layer allow must apply: %s", d)
	}
}

func TestWriteInfoOf(t *testing.T) {
	outside := filepath.Join(t.TempDir(), "out.txt")

	// Typed contract: the OutsideRoots flag is authoritative.
	if _, ok := WriteInfoOf(writeCall(outside, true)); !ok {
		t.Fatal("typed outside-roots write must resolve")
	}
	if _, ok := WriteInfoOf(writeCall("/tmp/scratch.txt", false)); ok {
		t.Fatal("confined write (scratch) must NOT resolve as boundary-crossing")
	}

	// No typed contract means NOT a boundary write — deciders never guess
	// from raw arguments (a read tool's absolute path would misclassify).
	args, _ := json.Marshal(map[string]string{"path": outside})
	raw := domain.PreparedCall{Call: domain.ToolCall{Name: "write", Arguments: args}}
	if _, ok := WriteInfoOf(raw); ok {
		t.Fatal("untyped call must not resolve as boundary-crossing")
	}
}

// WriteRequestFromRawArgs powers the approval-UI boundary: an absolute
// "path" argument synthesizes the boundary contract; a relative one
// never does.
func TestWriteRequestFromRawArgs(t *testing.T) {
	outside := filepath.Join(t.TempDir(), "out.txt")
	wr := WriteRequestFromRawArgs(json.RawMessage(`{"path":` + strconv.Quote(outside) + `,"content":"x"}`))
	if wr == nil || !wr.OutsideRoots || wr.Path != workspacepkg.Canonicalize(outside) {
		t.Fatalf("WriteRequestFromRawArgs = %+v", wr)
	}
	if wr := WriteRequestFromRawArgs(json.RawMessage(`{"path":"relative/file.go"}`)); wr != nil {
		t.Fatalf("relative path must not synthesize a request, got %+v", wr)
	}
	if wr := WriteRequestFromRawArgs(json.RawMessage(`{}`)); wr != nil {
		t.Fatalf("missing path must not synthesize a request, got %+v", wr)
	}
}

// Regression: read tools accept absolute paths outside the workspace
// (read alignment) and carry no WriteRequest — they must never be
// classified as boundary-crossing writes. Before the typed-only
// WriteInfoOf, an external read_file prompted in interactive modes and
// was DENIED in never mode.
func TestReadToolNeverClassifiedAsBoundaryWrite(t *testing.T) {
	args, _ := json.Marshal(map[string]string{"path": "/etc/hosts"})
	read := domain.PreparedCall{
		Call: domain.ToolCall{Name: "read_file", Arguments: args},
		Risk: domain.R1,
	}
	if _, ok := WriteInfoOf(read); ok {
		t.Fatal("read_file must never resolve as a boundary write")
	}
	for _, mode := range []ApprovalMode{ModeOnRequest, ModeUnlessDangerous, ModeNever} {
		v := BaselineDecider{Mode: mode}.Evaluate(read)
		if v.Decision != domain.DecisionAllow {
			t.Fatalf("%s baseline for external read = %s, want allow", mode, v.Decision)
		}
	}
}

// The decider chain: path rule → session memory → per-mode baseline.
func TestWriteDeciderChain(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "notes", "a.txt")

	// Baseline: interactive modes ask, never mode denies, confined writes
	// keep the silent R2 allow.
	ask := BaselineDecider{Mode: ModeOnRequest}.Evaluate(writeCall(target, true))
	if ask.Decision != domain.DecisionAsk || ask.Source != SourceBaseline {
		t.Fatalf("on-request baseline = %s (%s), want ask", ask.Decision, ask.Source)
	}
	ask = BaselineDecider{Mode: ModeUnlessDangerous}.Evaluate(writeCall(target, true))
	if ask.Decision != domain.DecisionAsk {
		t.Fatalf("unless-dangerous baseline = %s, want ask", ask.Decision)
	}
	deny := BaselineDecider{Mode: ModeNever}.Evaluate(writeCall(target, true))
	if deny.Decision != domain.DecisionDeny {
		t.Fatalf("never baseline = %s, want deny", deny.Decision)
	}
	allow := BaselineDecider{Mode: ModeOnRequest}.Evaluate(writeCall(target, false))
	if allow.Decision != domain.DecisionAllow {
		t.Fatalf("confined write baseline = %s, want allow", allow.Decision)
	}

	// A path rule decides before the baseline, in every mode.
	set := &RuleSet{paths: []PathRule{{
		Path: workspacepkg.Canonicalize(filepath.Dir(target)), Decision: "allow", Justification: "notes vault", Source: "test",
	}}}
	rule := RuleDecider{Rules: set}.Evaluate(writeCall(target, true))
	if rule.Decision != domain.DecisionAllow || rule.Source != SourceRule || rule.Reason != "notes vault" {
		t.Fatalf("rule verdict = %+v, want allow from rule", rule)
	}
	if v := (RuleDecider{Rules: set}).Evaluate(writeCall(filepath.Join(t.TempDir(), "b.txt"), true)); v != nil {
		t.Fatalf("unmatched path must yield no rule opinion, got %+v", v)
	}

	// Session memory auto-allows writes under a remembered directory.
	session := NewSessionRules()
	if _, ok := session.RememberPath(filepath.Dir(target)); !ok {
		t.Fatal("RememberPath failed")
	}
	v := (SessionDecider{Session: session}).Evaluate(writeCall(target, true))
	if v == nil || v.Decision != domain.DecisionAllow || v.Source != SourceSession {
		t.Fatalf("session verdict = %+v, want allow from session", v)
	}
	if v := (SessionDecider{Session: session}).Evaluate(writeCall(filepath.Join(t.TempDir(), "c.txt"), true)); v != nil {
		t.Fatalf("unremembered path must yield no session opinion, got %+v", v)
	}
}

func TestSessionPathMemory(t *testing.T) {
	dir := t.TempDir()
	s := NewSessionRules()
	canonical, ok := s.RememberPath(dir)
	if !ok || canonical != workspacepkg.Canonicalize(dir) {
		t.Fatalf("RememberPath = %q, %v", canonical, ok)
	}
	if !s.MatchPath(filepath.Join(dir, "sub", "f.txt")) {
		t.Error("MatchPath under remembered dir = false, want true")
	}
	if !s.MatchPath(dir) {
		t.Error("MatchPath exact dir = false, want true")
	}
	if s.MatchPath(filepath.Join(dir+"-sibling", "f.txt")) {
		t.Error("MatchPath sibling prefix = true, want false")
	}
	if got := s.SessionPaths(); len(got) != 1 || got[0] != canonical {
		t.Errorf("SessionPaths = %v", got)
	}
	if !s.ForgetPath(dir) || s.MatchPath(filepath.Join(dir, "f.txt")) {
		t.Error("ForgetPath must remove the memory")
	}

	// Sensitive locations can never be remembered.
	home := t.TempDir()
	t.Setenv("HOME", home)
	if _, ok := s.RememberPath(filepath.Join(home, ".ssh")); ok {
		t.Error("RememberPath(~/.ssh) must fail")
	}
}

func TestRememberedStorePaths(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	vault := t.TempDir()
	if err := store.RememberPath(ctx, vault); err != nil {
		t.Fatal(err)
	}
	// Idempotent re-remember.
	if err := store.RememberPath(ctx, vault); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	set, err := LoadRememberedRules(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	if d, rule := set.EvaluatePath(filepath.Join(vault, "a.txt")); d != domain.DecisionAllow || rule.Source != RememberedSource {
		t.Fatalf("remembered path not loaded: decision=%s rule=%+v", d, rule)
	}
	if !set.HasAny() {
		t.Fatal("HasAny must count path-only stores")
	}

	store, err = OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if ok, err := store.ForgetPath(ctx, vault); !ok || err != nil {
		t.Fatalf("ForgetPath = %v, %v", ok, err)
	}

	// Sensitive locations are refused at the store boundary too.
	home := t.TempDir()
	t.Setenv("HOME", home)
	if err := store.RememberPath(ctx, filepath.Join(home, ".aws")); err == nil {
		t.Fatal("RememberPath(~/.aws) must fail")
	}
}

// A legacy database without the remembered_paths table degrades to "no
// path rules" instead of failing the whole load.
func TestRememberedStorePathsLegacyDatabase(t *testing.T) {
	dir := t.TempDir()
	ctx := context.Background()
	store, err := OpenRememberedStore(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.db.ExecContext(ctx, "DROP TABLE remembered_paths"); err != nil {
		t.Fatal(err)
	}
	if err := store.RememberDomain(ctx, "www.weather.com.cn"); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	set, err := LoadRememberedRules(ctx, RememberedDBPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	if d, _ := set.EvaluateDomain("www.weather.com.cn"); d != domain.DecisionAllow {
		t.Fatal("domain rules must still load without the paths table")
	}
	if len(set.Paths()) != 0 {
		t.Fatal("legacy database must yield no path rules")
	}
}
