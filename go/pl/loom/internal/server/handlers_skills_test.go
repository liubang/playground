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
// Created: 2026/08/08

package server

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/agent"
	"github.com/liubang/playground/go/pl/loom/internal/app"
	"github.com/liubang/playground/go/pl/loom/internal/artifact"
	"github.com/liubang/playground/go/pl/loom/internal/config"
	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/fakes"
	"github.com/liubang/playground/go/pl/loom/internal/runtimeevent"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// newSkillsTestService mirrors newTestService but with skills discovery
// enabled and the storage base rooted in a temp dir.
func newSkillsTestService(t *testing.T) *app.SessionService {
	t.Helper()
	ctx := context.Background()
	store, err := session.OpenSQLiteStore(ctx, filepath.Join(t.TempDir(), "sessions.db"))
	if err != nil {
		t.Fatalf("OpenSQLiteStore: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	model := fakes.NewFakeModel()
	resolved := &config.ResolvedConfig{
		Providers: []config.ResolvedProvider{{
			Name:         "test",
			Model:        model,
			Models:       []config.Model{{Name: "model-a", ContextWindow: 128000}},
			DefaultModel: "model-a",
		}},
		Default: config.ProviderModelRef{Provider: "test", Model: "model-a"},
		Limits:  domain.DefaultLimits(),
		Skills:  config.ResolvedSkills{Enabled: true},
		Storage: config.ResolvedStorage{BaseDir: t.TempDir()},
	}
	broker := runtimeevent.NewBroker(runtimeevent.WithDurableQueue(4096))
	t.Cleanup(broker.Close)
	artStore, err := artifact.Open(filepath.Join(t.TempDir(), "artifacts"), resolved.Limits.MaxArtifactBytes)
	if err != nil {
		t.Fatalf("open artifact store: %v", err)
	}
	proc := &app.ProcessRuntime{
		Current:    resolved.Default,
		Store:      store,
		Artifact:   artStore,
		Questioner: domain.AutonomousQuestioner{},
	}
	proc.SwapResolved(resolved)
	svc := app.NewSingletonWorkspaceService(&app.Bootstrap{
		ProcessRuntime: proc,
		Registry:       agent.NewToolRegistry(),
	}, broker, app.SessionServiceConfig{})
	t.Cleanup(func() { _ = svc.Shutdown(context.Background()) })
	return svc
}

// writeSkillFile writes a minimal valid SKILL.md.
func writeSkillFile(t *testing.T, path, name, description string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("mkdir skill: %v", err)
	}
	body := "---\nname: " + name + "\ndescription: " + description + "\n---\n\nbody\n"
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatalf("write skill: %v", err)
	}
}

// groupByName finds one group in a /v1/skills response by its workspace_name.
func groupByName(t *testing.T, body map[string]any, name string) map[string]any {
	t.Helper()
	groups, ok := body["groups"].([]any)
	if !ok {
		t.Fatalf("groups missing in %v", body)
	}
	for _, g := range groups {
		gm := g.(map[string]any)
		if gm["workspace_name"] == name {
			return gm
		}
	}
	t.Fatalf("group %q not found in %v", name, body["groups"])
	return nil
}

func skillNames(t *testing.T, group map[string]any) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, s := range group["skills"].([]any) {
		sm := s.(map[string]any)
		out[sm["name"].(string)] = sm["scope"].(string)
	}
	return out
}

// TestListSkillsAggregatesWorkspaces locks the aggregated listing: the
// shared user-scope group plus one repo-scope group per workspace.
func TestListSkillsAggregatesWorkspaces(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	writeSkillFile(t, filepath.Join(home, ".agents", "skills", "user-skill", "SKILL.md"), "user-skill", "user scope skill")

	wsRoot := t.TempDir()
	writeSkillFile(t, filepath.Join(wsRoot, ".loom", "skills", "repo-skill", "SKILL.md"), "repo-skill", "repo scope skill")
	// A broken skill (no description) surfaces as an issue, not a skill.
	broken := filepath.Join(wsRoot, ".loom", "skills", "broken", "SKILL.md")
	if err := os.MkdirAll(filepath.Dir(broken), 0o755); err != nil {
		t.Fatalf("mkdir broken skill: %v", err)
	}
	if err := os.WriteFile(broken, []byte("---\nname: broken\n---\n"), 0o644); err != nil {
		t.Fatalf("write broken skill: %v", err)
	}

	svc := newSkillsTestService(t)
	if _, err := svc.RegisterWorkspace(context.Background(), wsRoot, "ws1"); err != nil {
		t.Fatalf("RegisterWorkspace: %v", err)
	}
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)

	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	if body["enabled"] != true {
		t.Fatalf("enabled = %v, want true (reason %v)", body["enabled"], body["reason"])
	}

	shared := groupByName(t, body, "用户级（所有工作区共享）")
	if shared["shared"] != true {
		t.Fatalf("shared group not marked shared: %v", shared)
	}
	if names := skillNames(t, shared); names["user-skill"] != "user" {
		t.Fatalf("shared group skills = %v, want user-skill (user)", names)
	}

	ws := groupByName(t, body, "ws1")
	names := skillNames(t, ws)
	if names["repo-skill"] != "repo" {
		t.Fatalf("workspace group skills = %v, want repo-skill (repo)", names)
	}
	// The user-scope skill is reported only in the shared group, not
	// duplicated into the workspace group.
	if _, dup := names["user-skill"]; dup {
		t.Fatalf("workspace group duplicated the user-scope skill: %v", names)
	}
	issues, _ := ws["issues"].([]any)
	if len(issues) != 1 {
		t.Fatalf("workspace issues = %v, want the broken skill issue", ws["issues"])
	}
}

// skillEntry finds one skill in a group of a /v1/skills response.
func skillEntry(t *testing.T, group map[string]any, name string) map[string]any {
	t.Helper()
	for _, s := range group["skills"].([]any) {
		if sm := s.(map[string]any); sm["name"] == name {
			return sm
		}
	}
	t.Fatalf("skill %q not in group %v", name, group["skills"])
	return nil
}

// TestSetSkillDisabledPersistsAndHotApplies: the toggle endpoint writes the
// name into the config file's skills.disabled list, hot-applies it (the
// aggregated listing then marks the skill instead of dropping it), and a
// second toggle removes it again.
func TestSetSkillDisabledPersistsAndHotApplies(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	writeSkillFile(t, filepath.Join(home, ".agents", "skills", "user-skill", "SKILL.md"), "user-skill", "user scope skill")

	// The toggle persists via the config file, which must already hold a
	// valid configuration (JSON is a YAML subset; reuse the config fixture).
	cfgPath := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(cfgPath, []byte(validConfigBody), 0o600); err != nil {
		t.Fatalf("write config: %v", err)
	}
	svc := newSkillsTestService(t)
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc, ConfigPath: cfgPath})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)

	// Baseline: listed, not disabled.
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	entry := skillEntry(t, groupByName(t, body, "用户级（所有工作区共享）"), "user-skill")
	if entry["disabled"] == true {
		t.Fatalf("baseline disabled = %v, want absent/false", entry["disabled"])
	}

	// Disable: persisted to the file and hot-applied.
	status, resp := doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/skills/user-skill/disabled", `{"disabled": true}`)
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, resp)
	}
	if names, _ := resp["disabled"].([]any); len(names) != 1 || names[0] != "user-skill" {
		t.Fatalf("disabled = %v, want [user-skill]", resp["disabled"])
	}
	applied, _ := resp["applied"].(map[string]any)
	imm, _ := applied["immediate"].([]any)
	found := false
	for _, s := range imm {
		if s == "skills.disabled" {
			found = true
		}
	}
	if !found {
		t.Fatalf("applied.immediate = %v, want skills.disabled", applied["immediate"])
	}
	raw, err := os.ReadFile(cfgPath)
	if err != nil || !strings.Contains(string(raw), "user-skill") {
		t.Fatalf("config file = %q, %v; want skills.disabled with user-skill", raw, err)
	}

	// The listing still shows the skill, marked disabled.
	status, body = doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	entry = skillEntry(t, groupByName(t, body, "用户级（所有工作区共享）"), "user-skill")
	if entry["disabled"] != true {
		t.Fatalf("disabled = %v, want true", entry)
	}

	// Re-enable removes the name again.
	status, resp = doJSON(t, ts.Client(), http.MethodPut, ts.URL+"/v1/skills/user-skill/disabled", `{"disabled": false}`)
	if status != http.StatusOK {
		t.Fatalf("PUT status = %d (%v)", status, resp)
	}
	if names, _ := resp["disabled"].([]any); len(names) != 0 {
		t.Fatalf("disabled = %v, want empty", resp["disabled"])
	}
	status, body = doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	entry = skillEntry(t, groupByName(t, body, "用户级（所有工作区共享）"), "user-skill")
	if entry["disabled"] == true {
		t.Fatalf("disabled = %v after re-enable, want absent/false", entry["disabled"])
	}
}

// TestDeleteSkillRemovesFromDisk: the delete endpoint removes the skill
// directory — but only for paths the discovery roots would actually load.
func TestDeleteSkillRemovesFromDisk(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	wsRoot := t.TempDir()
	skillPath := filepath.Join(wsRoot, ".loom", "skills", "repo-skill", "SKILL.md")
	writeSkillFile(t, skillPath, "repo-skill", "repo scope skill")

	svc := newSkillsTestService(t)
	if _, err := svc.RegisterWorkspace(context.Background(), wsRoot, "ws1"); err != nil {
		t.Fatalf("RegisterWorkspace: %v", err)
	}
	srv, err := New(Config{Token: testToken, Version: "test", Service: svc})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)

	// A loadable SKILL.md outside every discovery root → 404, untouched.
	outside := filepath.Join(t.TempDir(), "x", "SKILL.md")
	writeSkillFile(t, outside, "x", "d")
	status, body := doJSON(t, ts.Client(), http.MethodDelete, ts.URL+"/v1/skills", `{"path": "`+outside+`"}`)
	if status != http.StatusNotFound || body["error"].(map[string]any)["code"] != "skill_not_found" {
		t.Fatalf("DELETE outside-root status = %d (%v), want 404 skill_not_found", status, body)
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatalf("outside-root skill removed: %v", err)
	}

	// A path not naming SKILL.md → 400.
	status, _ = doJSON(t, ts.Client(), http.MethodDelete, ts.URL+"/v1/skills", `{"path": "/etc/hosts"}`)
	if status != http.StatusBadRequest {
		t.Fatalf("DELETE non-SKILL.md status = %d, want 400", status)
	}

	// The real delete: 204 (no body), the directory is gone, and the next
	// listing no longer shows the skill.
	req, err := http.NewRequest(http.MethodDelete, ts.URL+"/v1/skills", strings.NewReader(`{"path": "`+skillPath+`"}`))
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	authed(t, req)
	req.Header.Set("Content-Type", "application/json")
	resp, err := ts.Client().Do(req)
	if err != nil {
		t.Fatalf("DELETE: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Fatalf("DELETE status = %d, want 204", resp.StatusCode)
	}
	if _, err := os.Stat(filepath.Dir(skillPath)); !os.IsNotExist(err) {
		t.Fatalf("skill dir still exists: %v", err)
	}
	status, body = doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	if names := skillNames(t, groupByName(t, body, "ws1")); len(names) != 0 {
		t.Fatalf("workspace skills = %v after delete, want empty", names)
	}
}

// TestListSkillsDisabled: with skills turned off the endpoint reports the
// disabled state instead of scanning.
func TestListSkillsDisabled(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	ts := newConfigTestServer(t, filepath.Join(t.TempDir(), "config.yaml"))
	status, body := doJSON(t, ts.Client(), http.MethodGet, ts.URL+"/v1/skills", "")
	if status != http.StatusOK {
		t.Fatalf("GET status = %d (%v)", status, body)
	}
	if body["enabled"] != false {
		t.Fatalf("enabled = %v, want false", body["enabled"])
	}
	if body["reason"] == "" || body["reason"] == nil {
		t.Fatalf("reason missing: %v", body)
	}
	if groups := body["groups"].([]any); len(groups) != 0 {
		t.Fatalf("groups = %v, want empty", groups)
	}
}
