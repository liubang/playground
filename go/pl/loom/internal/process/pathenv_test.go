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
// Created: 2026/08/09

package process

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func dirSet(paths ...string) func(string) bool {
	set := make(map[string]bool, len(paths))
	for _, p := range paths {
		set[p] = true
	}
	return func(path string) bool { return set[path] }
}

// resolve is a test shortcut: no config extras, no glob expansion.
func resolve(base, home string, dirExists func(string) bool) (string, []PathDir) {
	return ResolveToolchainPATH(base, home, nil, dirExists, nil)
}

func dirStatus(dirs []PathDir, path string) DirStatus {
	for _, d := range dirs {
		if d.Path == path {
			return d.Status
		}
	}
	return ""
}

func TestResolvePrependsExistingToolchainDirs(t *testing.T) {
	got, dirs := resolve("/usr/bin:/bin", "/home/u",
		dirSet("/home/u/.local/share/mise/shims", "/opt/homebrew/bin"))
	want := "/home/u/.local/share/mise/shims:/opt/homebrew/bin:/usr/bin:/bin"
	if got != want {
		t.Fatalf("ResolveToolchainPATH = %q, want %q", got, want)
	}
	if s := dirStatus(dirs, "/home/u/.local/share/mise/shims"); s != DirStatusPrepended {
		t.Fatalf("mise shims status = %q, want prepended", s)
	}
}

func TestResolveSkipsMissingDirs(t *testing.T) {
	got, dirs := resolve("/usr/bin", "/home/u", dirSet())
	if got != "/usr/bin" {
		t.Fatalf("ResolveToolchainPATH = %q, want unchanged base", got)
	}
	if s := dirStatus(dirs, "/opt/homebrew/bin"); s != DirStatusMissing {
		t.Fatalf("missing dir status = %q, want missing", s)
	}
}

func TestResolveKeepsInheritedOrderUntouched(t *testing.T) {
	// A dir already on the inherited PATH keeps its position — loom never
	// reorders the user's explicit precedence, it only reports "existing".
	base := "/usr/bin:/home/u/.cargo/bin:/bin"
	got, dirs := resolve(base, "/home/u", dirSet("/home/u/.cargo/bin"))
	if got != base {
		t.Fatalf("ResolveToolchainPATH = %q, want unchanged %q", got, base)
	}
	if s := dirStatus(dirs, "/home/u/.cargo/bin"); s != DirStatusExisting {
		t.Fatalf("inherited dir status = %q, want existing", s)
	}
}

func TestResolveConfigExtraOutranksStatic(t *testing.T) {
	got, dirs := ResolveToolchainPATH("/usr/bin", "/home/u",
		[]string{"~/corp/bin"}, dirSet("/home/u/corp/bin", "/home/u/.local/share/mise/shims"), nil)
	want := "/home/u/corp/bin:/home/u/.local/share/mise/shims:/usr/bin"
	if got != want {
		t.Fatalf("ResolveToolchainPATH = %q, want %q", got, want)
	}
	if s := dirStatus(dirs, "/home/u/corp/bin"); s != DirStatusPrepended {
		t.Fatalf("config dir status = %q, want prepended", s)
	}
	// Config entries are reported with their source so the settings card
	// can tell user configuration apart from built-in candidates.
	if dirs[0].Source != DirSourceConfig {
		t.Fatalf("first dir source = %q, want config", dirs[0].Source)
	}
}

func TestResolveExpandsGlobsWithCap(t *testing.T) {
	globFn := func(pattern string) ([]string, error) {
		if pattern != "/home/u/.sdkman/candidates/*/current/bin" {
			return nil, nil
		}
		matches := make([]string, 0, maxGlobExpansions+3)
		for _, name := range []string{"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k"} {
			matches = append(matches, "/home/u/.sdkman/candidates/"+name+"/current/bin")
		}
		return matches, nil
	}
	got, dirs := ResolveToolchainPATH("/usr/bin", "/home/u", nil, dirSet(), globFn)
	// dirSet() reports nothing existing, so expansions are all "missing";
	// the cap still applies to how many are considered.
	globbed := 0
	for _, d := range dirs {
		if d.Source == DirSourceGlob {
			globbed++
			if d.Status != DirStatusMissing {
				t.Fatalf("glob dir %q status = %q, want missing", d.Path, d.Status)
			}
		}
	}
	if globbed != maxGlobExpansions {
		t.Fatalf("glob expansions = %d, want capped at %d", globbed, maxGlobExpansions)
	}
	if got != "/usr/bin" {
		t.Fatalf("ResolveToolchainPATH = %q, want unchanged base", got)
	}
}

func TestResolveEmptyBaseStartsFromDefault(t *testing.T) {
	got, _ := resolve("", "/home/u", dirSet("/usr/local/bin"))
	want := "/usr/local/bin:" + defaultPATH
	if got != want {
		t.Fatalf("ResolveToolchainPATH(\"\") = %q, want %q", got, want)
	}
}

func TestResolveSkipsTildeEntriesWithoutHome(t *testing.T) {
	got, _ := resolve("/usr/bin", "", dirSet("/opt/homebrew/bin"))
	want := "/opt/homebrew/bin:/usr/bin"
	if got != want {
		t.Fatalf("ResolveToolchainPATH = %q, want %q", got, want)
	}
}

func TestResolveIsIdempotent(t *testing.T) {
	exists := dirSet("/opt/homebrew/bin", "/home/u/.cargo/bin")
	once, _ := resolve("/usr/bin", "/home/u", exists)
	twice, dirs := resolve(once, "/home/u", exists)
	if once != twice {
		t.Fatalf("not idempotent: once=%q twice=%q", once, twice)
	}
	if strings.Count(twice, "/opt/homebrew/bin") != 1 {
		t.Fatalf("duplicate prepended: %q", twice)
	}
	// Second pass sees loom's own additions as "existing" base entries.
	if s := dirStatus(dirs, "/opt/homebrew/bin"); s != DirStatusExisting {
		t.Fatalf("second-pass status = %q, want existing", s)
	}
}

func TestAugmentProcessPATHHotApplyRebuilds(t *testing.T) {
	extraDir := t.TempDir()
	staticDir := t.TempDir() // stands in for a well-known dir via config below
	t.Setenv("PATH", "/usr/bin:/bin")

	added := AugmentProcessPATH([]string{extraDir, staticDir})
	// Config dirs lead the prepended set (they outrank the built-in
	// candidates, which follow on machines where they exist).
	if len(added) < 2 || added[0] != extraDir || added[1] != staticDir {
		t.Fatalf("added = %v, want config dirs leading", added)
	}
	got := os.Getenv("PATH")
	if !strings.HasPrefix(got, extraDir+":"+staticDir+":") {
		t.Fatalf("PATH after augment = %q", got)
	}
	if !strings.HasSuffix(got, "/usr/bin:/bin") {
		t.Fatalf("base entries must keep their tail position: %q", got)
	}
	report := CurrentToolchainReport()
	if report == nil || report.EffectivePATH != got {
		t.Fatalf("report not cached: %+v", report)
	}

	// Hot-apply with one entry removed: the rebuild starts from the
	// pre-augmentation base, so the dropped entry leaves the PATH.
	added = AugmentProcessPATH([]string{extraDir})
	if len(added) == 0 || added[0] != extraDir {
		t.Fatalf("added after re-augment = %v, want %s leading", added, extraDir)
	}
	got = os.Getenv("PATH")
	if strings.Contains(got, staticDir) {
		t.Fatalf("removed config dir still on PATH: %q", got)
	}
	if !strings.HasPrefix(got, extraDir+":") || !strings.HasSuffix(got, "/usr/bin:/bin") {
		t.Fatalf("PATH after re-augment = %q", got)
	}
}

func TestLookPathOn(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "faketool")
	if err := os.WriteFile(exe, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	plain := filepath.Join(dir, "notexec")
	if err := os.WriteFile(plain, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if got := lookPathOn("/elsewhere:"+dir, "faketool"); got != exe {
		t.Fatalf("lookPathOn(faketool) = %q, want %q", got, exe)
	}
	if got := lookPathOn(dir, "notexec"); got != "" {
		t.Fatalf("lookPathOn(notexec) = %q, want empty (not executable)", got)
	}
	if got := lookPathOn(dir, "missing"); got != "" {
		t.Fatalf("lookPathOn(missing) = %q, want empty", got)
	}
}

func TestSubtractPathList(t *testing.T) {
	got := subtractPathList("/a:/b:/c", []string{"/b"})
	if got != "/a:/c" {
		t.Fatalf("subtractPathList = %q, want /a:/c", got)
	}
	if got := subtractPathList("/a:/b", nil); got != "/a:/b" {
		t.Fatalf("subtractPathList(nil) = %q, want unchanged", got)
	}
}
