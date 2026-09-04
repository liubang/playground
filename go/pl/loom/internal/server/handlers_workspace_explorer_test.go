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
// Created: 2026/08/22

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/fakes"
)

// --- path confinement ---

func TestConfineWorkspacePath(t *testing.T) {
	root := t.TempDir()
	if resolved, err := filepath.EvalSymlinks(root); err == nil {
		root = resolved
	}
	// A symlink inside the workspace pointing outside must not be followed.
	outside := t.TempDir()
	if err := os.Symlink(outside, filepath.Join(root, "link")); err != nil {
		t.Fatal(err)
	}

	cases := []struct {
		name    string
		rel     string
		wantErr bool
	}{
		{"root itself", "", false},
		{"relative", "sub/dir", false},
		{"dot", ".", false},
		{"absolute rejected", "/etc/hosts", true},
		{"parent escape", "../outside", true},
		{"deep parent escape", "a/../../outside", true},
		{"symlink escape rejected", "link", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// Existing paths only: create the directory for the happy cases.
			if !tc.wantErr && tc.rel != "" && tc.rel != "." {
				if err := os.MkdirAll(filepath.Join(root, tc.rel), 0o755); err != nil {
					t.Fatal(err)
				}
			}
			_, err := confineWorkspacePath(root, tc.rel)
			if tc.wantErr && err == nil {
				t.Fatalf("confineWorkspacePath(%q) should fail", tc.rel)
			}
			if !tc.wantErr && err != nil {
				t.Fatalf("confineWorkspacePath(%q): %v", tc.rel, err)
			}
		})
	}
}

func TestConfineWorkspacePathLoose(t *testing.T) {
	root := t.TempDir()
	if resolved, err := filepath.EvalSymlinks(root); err == nil {
		root = resolved
	}
	// The loose variant must accept non-existent targets (deleted files) while
	// still rejecting escapes.
	abs, err := confineWorkspacePathLoose(root, "deleted/entry.go")
	if err != nil {
		t.Fatalf("non-existent in-workspace path should pass: %v", err)
	}
	if !strings.HasPrefix(abs, root) {
		t.Fatalf("resolved path %q left the workspace", abs)
	}
	if _, err := confineWorkspacePathLoose(root, "../outside"); err == nil {
		t.Fatal("escape must be rejected")
	}
}

// --- porcelain/numstat parsers ---

func TestParsePorcelainZ(t *testing.T) {
	raw := " M modified.go\x00?? untracked.txt\x00R  new-name.go\x00old-name.go\x00"
	got := parsePorcelainZ(raw)
	if len(got) != 3 {
		t.Fatalf("entries = %v, want 3", got)
	}
	if got[0] != [2]string{" M", "modified.go"} {
		t.Fatalf("modified entry = %v", got[0])
	}
	if got[1] != [2]string{"??", "untracked.txt"} {
		t.Fatalf("untracked entry = %v", got[1])
	}
	// The rename record consumes the following field (old path); the new name
	// is the entry's path.
	if got[2] != [2]string{"R ", "new-name.go"} {
		t.Fatalf("rename entry = %v", got[2])
	}
}

func TestParseNumstatZ(t *testing.T) {
	raw := "3\t2\tfile.go\x00-\t-\tbin.png\x001\t1\t\x00old.go\x00new.go\x00"
	stats := parseNumstatZ(raw)
	if st := stats["file.go"]; st.adds != 3 || st.dels != 2 || st.binary {
		t.Fatalf("file.go stat = %+v", st)
	}
	if st := stats["bin.png"]; !st.binary {
		t.Fatalf("bin.png should be binary: %+v", st)
	}
	// Rename records key by the new path.
	if st := stats["new.go"]; st.adds != 1 || st.dels != 1 {
		t.Fatalf("rename should key by new path, got %+v", stats)
	}
}

// --- handlers over a real httptest server ---

// newExplorerFixture builds a workspace with a small file tree and returns
// the running server plus the workspace id.
func newExplorerFixture(t *testing.T) (*httptest.Server, string) {
	t.Helper()
	dir := t.TempDir()
	write := func(rel, content string) {
		abs := filepath.Join(dir, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(abs), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(abs, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("alpha.go", "package main\n")
	write("sub/beta.go", "package sub\n")
	write(".hidden", "secret\n")

	ts, svc := newTestServer(t, fakes.NewFakeModel())
	ws, err := svc.RegisterWorkspace(t.Context(), dir, "explorer-test")
	if err != nil {
		t.Fatalf("RegisterWorkspace: %v", err)
	}
	return ts, ws.ID.String()
}

func getJSON(t *testing.T, ts *httptest.Server, path string) (int, map[string]any) {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, ts.URL+path, nil)
	if err != nil {
		t.Fatal(err)
	}
	authed(t, req)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var body map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&body)
	return resp.StatusCode, body
}

func TestWorkspaceFilesListing(t *testing.T) {
	ts, wsID := newExplorerFixture(t)
	code, body := getJSON(t, ts, "/v1/workspaces/"+wsID+"/files")
	if code != http.StatusOK {
		t.Fatalf("status = %d", code)
	}
	entries, _ := body["entries"].([]any)
	names := map[string]string{}
	for _, e := range entries {
		m := e.(map[string]any)
		names[m["name"].(string)] = m["kind"].(string)
	}
	if names["alpha.go"] != "file" || names["sub"] != "dir" {
		t.Fatalf("entries = %v", names)
	}
	if _, ok := names[".hidden"]; ok {
		t.Fatal("dotfiles must be hidden by default")
	}
}

func TestWorkspaceFileReadAndEscape(t *testing.T) {
	ts, wsID := newExplorerFixture(t)
	code, body := getJSON(t, ts, "/v1/workspaces/"+wsID+"/file?path=alpha.go")
	if code != http.StatusOK || body["content"] != "package main\n" {
		t.Fatalf("read = %d %v", code, body)
	}
	code, _ = getJSON(t, ts, "/v1/workspaces/"+wsID+"/file?path=../../../etc/hosts")
	if code != http.StatusBadRequest {
		t.Fatalf("escape status = %d, want 400", code)
	}
}

func TestWorkspaceFilesSearchRanking(t *testing.T) {
	ts, wsID := newExplorerFixture(t)
	code, body := getJSON(t, ts, "/v1/workspaces/"+wsID+"/files/search?q=beta")
	if code != http.StatusOK {
		t.Fatalf("status = %d", code)
	}
	matches, _ := body["matches"].([]any)
	if len(matches) != 1 {
		t.Fatalf("matches = %v, want exactly beta.go", matches)
	}
	if m := matches[0].(map[string]any); m["path"] != "sub/beta.go" {
		t.Fatalf("match = %v", m)
	}
}

func TestWorkspaceGitStatusNonRepo(t *testing.T) {
	ts, wsID := newExplorerFixture(t)
	code, body := getJSON(t, ts, "/v1/workspaces/"+wsID+"/git/status")
	if code != http.StatusOK || body["is_git"] != false {
		t.Fatalf("non-repo status = %d %v", code, body)
	}
}

func TestWorkspaceApprovalModeRoundtrip(t *testing.T) {
	ts, wsID := newExplorerFixture(t)
	get := func() string {
		_, body := getJSON(t, ts, "/v1/workspaces/"+wsID+"/approval-mode")
		mode, _ := body["mode"].(string)
		return mode
	}
	if got := get(); got != "on-request" {
		t.Fatalf("default mode = %q, want on-request", got)
	}
	req, err := http.NewRequest(http.MethodPost, ts.URL+"/v1/workspaces/"+wsID+"/approval-mode",
		strings.NewReader(`{"mode":"danger-only"}`))
	if err != nil {
		t.Fatal(err)
	}
	authed(t, req)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("post status = %d", resp.StatusCode)
	}
	// The live override must be readable back (the reload-consistency fix).
	if got := get(); got != "danger-only" {
		t.Fatalf("mode after switch = %q, want danger-only", got)
	}
}
