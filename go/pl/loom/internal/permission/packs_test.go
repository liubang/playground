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
// Created: 2026/08/11

package permission

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

func TestLoadPacksValidatesTemplates(t *testing.T) {
	packs, err := LoadPacks()
	if err != nil {
		t.Fatalf("LoadPacks() error = %v", err)
	}
	if len(packs) < 3 {
		t.Fatalf("packs = %d, want at least 3 (go-toolchain, cloud-cli, python-pip)", len(packs))
	}
	seen := map[string]bool{}
	for _, p := range packs {
		if seen[p.ID] {
			t.Fatalf("duplicate pack id %q", p.ID)
		}
		seen[p.ID] = true
		if p.Name == "" || p.Description == "" || p.Reason == "" {
			t.Fatalf("pack %s missing display fields", p.ID)
		}
		if p.Risk == "" || p.RuleCount == 0 {
			t.Fatalf("pack %s missing risk/rule_count", p.ID)
		}
		for _, cmd := range p.Commands {
			if strings.TrimSpace(cmd) == "" {
				t.Fatalf("pack %s has empty command entry", p.ID)
			}
		}
		// Every template rule must be allow+grant: a pack that ships a
		// deny/ask rule or a grant-less allow is a template bug.
		raw, err := packsFS.ReadFile("packs/" + p.ID + ".json")
		if err != nil {
			t.Fatalf("read template %s: %v", p.ID, err)
		}
		var f packFile
		if err := json.Unmarshal(raw, &f); err != nil {
			t.Fatalf("unmarshal template %s: %v", p.ID, err)
		}
		for _, r := range f.Rules {
			if r.Decision != string(domain.DecisionAllow) || r.Grant == nil || !r.Grant.Unsandboxed {
				t.Fatalf("pack %s rule %v must be allow with unsandboxed grant", p.ID, r.ArgvPrefix)
			}
		}
	}
	for _, want := range []string{"go-toolchain", "cloud-cli", "python-pip"} {
		if !seen[want] {
			t.Fatalf("missing expected pack %q (have %v)", want, seen)
		}
	}
}

func TestInstalledPackRoundTrip(t *testing.T) {
	rulesDir := t.TempDir()

	// Not installed yet.
	if ids := InstalledPackIDs(rulesDir); len(ids) != 0 {
		t.Fatalf("InstalledPackIDs before install = %v, want empty", ids)
	}

	info, err := InstallPack(rulesDir, "go-toolchain")
	if err != nil {
		t.Fatalf("InstallPack() error = %v", err)
	}
	if !info.Installed || info.Path == "" {
		t.Fatalf("installed info = %+v, want installed with path", info)
	}
	if _, err := os.Stat(info.Path); err != nil {
		t.Fatalf("installed file missing: %v", err)
	}
	if ids := InstalledPackIDs(rulesDir); len(ids) != 1 || ids[0] != "go-toolchain" {
		t.Fatalf("InstalledPackIDs = %v, want [go-toolchain]", ids)
	}

	// Idempotent install: second call succeeds and leaves the file alone.
	info2, err := InstallPack(rulesDir, "go-toolchain")
	if err != nil {
		t.Fatalf("second InstallPack() error = %v", err)
	}
	if !info2.Installed {
		t.Fatalf("second install not reported installed")
	}

	// The installed file must load as an ordinary user rule file with the
	// unsandboxed grant intact (LoadRuleSets is what AttachRules uses).
	set, errs := LoadRuleSets(rulesDir, "", LoadOptions{})
	if len(errs) != 0 {
		t.Fatalf("LoadRuleSets errors: %v", errs)
	}
	d, rule := set.Evaluate([]string{"go", "mod", "download", "github.com/x/y"})
	if d != domain.DecisionAllow || rule.Grant == nil || !rule.Grant.Unsandboxed {
		t.Fatalf("installed rule must allow with unsandboxed grant, got %s %+v", d, rule.Grant)
	}

	// Unknown pack id is rejected.
	if _, err := InstallPack(rulesDir, "no-such-pack"); err == nil {
		t.Fatal("InstallPack(no-such-pack) must fail")
	}

	// Uninstall removes the file and is idempotent.
	if err := UninstallPack(rulesDir, "go-toolchain"); err != nil {
		t.Fatalf("UninstallPack() error = %v", err)
	}
	if _, err := os.Stat(info.Path); !os.IsNotExist(err) {
		t.Fatalf("installed file still present after uninstall")
	}
	if err := UninstallPack(rulesDir, "go-toolchain"); err != nil {
		t.Fatalf("second UninstallPack() error = %v", err)
	}
	if ids := InstalledPackIDs(rulesDir); len(ids) != 0 {
		t.Fatalf("InstalledPackIDs after uninstall = %v, want empty", ids)
	}
}

func TestInstallPackNeverClobbersUserEdits(t *testing.T) {
	rulesDir := t.TempDir()
	if _, err := InstallPack(rulesDir, "go-toolchain"); err != nil {
		t.Fatal(err)
	}
	path := InstalledPackPath(rulesDir, "go-toolchain")
	// Simulate a user edit after install.
	edited := `{"rules":[{"argv_prefix":["go","mod","download"],"decision":"deny","justification":"user overrode"}]}`
	if err := os.WriteFile(path, []byte(edited), 0o644); err != nil {
		t.Fatal(err)
	}
	// Re-install must not clobber.
	if _, err := InstallPack(rulesDir, "go-toolchain"); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != edited {
		t.Fatalf("re-install clobbered user edits:\n%s", got)
	}
}

func TestPackFileNaming(t *testing.T) {
	if got := PackFileName("go-toolchain"); got != "pack-go-toolchain.json" {
		t.Fatalf("PackFileName = %q", got)
	}
	for name, wantID := range map[string]string{
		"pack-go-toolchain.json": "go-toolchain",
		"pack-cloud-cli.json":    "cloud-cli",
		"rules.json":             "",
		"pack-.json":             "",
	} {
		id, ok := PackIDFromFileName(name)
		if wantID == "" {
			if ok {
				t.Fatalf("PackIDFromFileName(%q) = %q, want not-ok", name, id)
			}
			continue
		}
		if !ok || id != wantID {
			t.Fatalf("PackIDFromFileName(%q) = %q, %v; want %q", name, id, ok, wantID)
		}
	}
}

// TestPackInstalledFileCarriesJustifications locks the auditability
// contract: an installed pack file stays a plain, human-readable rule
// file (not a binary blob), so `loom rules check` and manual editing work.
func TestPackInstalledFileCarriesJustifications(t *testing.T) {
	rulesDir := t.TempDir()
	if _, err := InstallPack(rulesDir, "python-pip"); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(InstalledPackPath(rulesDir, "python-pip"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(got), "justification") {
		t.Fatalf("installed pack file must keep justifications for auditability:\n%s", got)
	}
	if !filepath.IsAbs(InstalledPackPath(rulesDir, "go-toolchain")) {
		t.Fatal("InstalledPackPath must return an absolute path")
	}
}
