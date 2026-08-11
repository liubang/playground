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

// Rule packs are curated, user-opt-in rule templates for known-good but
// sandbox-incompatible commands (Go/Python TLS via Security framework,
// etc.). Each pack ships embedded in the binary (packs/*.json) with
// human-readable metadata; enabling a pack writes its rules to the user
// rules directory as a STANDARD rule file (pack-<id>.json), so:
//
//   - loading needs zero special-casing: LoadRuleSets already reads every
//     top-level *.json in the user layer, and the file carries ordinary
//     argv rules with grants;
//   - the user can audit, edit, or delete the file exactly like any other
//     rule file (`loom rules check` sees it);
//   - disabling is just deleting the file (idempotent).
//
// Builtin packs never widen the boundary by themselves: unsandboxed
// grants are L2 trust, so installation is an explicit user action gated
// by the WebUI confirmation, never a silent default.
package permission

import (
	"embed"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

//go:embed packs/*.json
var packsFS embed.FS

// packFile is the on-disk template format: metadata plus the standard
// rule file sections. Only the rule sections are written to the user
// rules directory on install; the metadata is display-only.
type packFile struct {
	Pack    PackMeta     `json:"pack"`
	Rules   []Rule       `json:"rules"`
	Domains []DomainRule `json:"domains,omitempty"`
}

// PackMeta describes one rule pack for the settings UI.
type PackMeta struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Description string   `json:"description"`
	Risk        string   `json:"risk"`   // low | medium | high
	Reason      string   `json:"reason"` // why the grant is needed + trust boundary
	Commands    []string `json:"commands"`
}

// PackInfo is the API view of a pack: metadata plus installation state.
type PackInfo struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Description string   `json:"description"`
	Risk        string   `json:"risk"`
	Reason      string   `json:"reason"`
	Commands    []string `json:"commands"`
	RuleCount   int      `json:"rule_count"`
	Installed   bool     `json:"installed"`
	// Path is the installed rule file path (empty when not installed).
	Path string `json:"path,omitempty"`
}

// packPrefix marks rule files installed by a pack. LoadRuleSets picks up
// every top-level *.json in the user layer, so pack files participate in
// the ordinary rule pipeline; this prefix is only used to list/remove
// pack-managed files.
const packPrefix = "pack-"

// PackFileName returns the user-layer rule file name for a pack id.
func PackFileName(id string) string {
	return packPrefix + id + ".json"
}

// PackIDFromFileName reverses PackFileName; ok=false for non-pack files
// or an empty id.
func PackIDFromFileName(name string) (string, bool) {
	if !strings.HasPrefix(name, packPrefix) || !strings.HasSuffix(name, ".json") {
		return "", false
	}
	id := strings.TrimSuffix(strings.TrimPrefix(name, packPrefix), ".json")
	if id == "" {
		return "", false
	}
	return id, true
}

// LoadPacks parses and validates every embedded pack template. A broken
// template is a build-time bug, so the error is fatal to the caller
// (mirrors LoadBuiltinRules).
func LoadPacks() ([]PackInfo, error) {
	entries, err := packsFS.ReadDir("packs")
	if err != nil {
		return nil, fmt.Errorf("read embedded packs: %w", err)
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	packs := make([]PackInfo, 0, len(names))
	for _, name := range names {
		data, err := packsFS.ReadFile(filepath.Join("packs", name))
		if err != nil {
			return nil, fmt.Errorf("read embedded pack %s: %w", name, err)
		}
		var f packFile
		if err := json.Unmarshal(data, &f); err != nil {
			return nil, fmt.Errorf("parse embedded pack %s: %w", name, err)
		}
		if f.Pack.ID == "" {
			return nil, fmt.Errorf("embedded pack %s: missing pack.id", name)
		}
		// Validate the rule sections exactly as a user rule file would be,
		// so a pack can never install rules that fail loading.
		for i := range f.Rules {
			f.Rules[i].Source = builtinSource
			if err := validateRule(&f.Rules[i]); err != nil {
				return nil, fmt.Errorf("embedded pack %s: %w", name, err)
			}
			// Packs exist to widen capabilities; only allow-with-grant
			// rules make sense. Anything else is a template bug.
			if f.Rules[i].Decision != string(domain.DecisionAllow) || f.Rules[i].Grant == nil {
				return nil, fmt.Errorf("embedded pack %s: rule %v must be allow with a grant", name, f.Rules[i].ArgvPrefix)
			}
		}
		for i := range f.Domains {
			if err := validateDomainRule(&f.Domains[i]); err != nil {
				return nil, fmt.Errorf("embedded pack %s: %w", name, err)
			}
		}
		packs = append(packs, PackInfo{
			ID:          f.Pack.ID,
			Name:        f.Pack.Name,
			Description: f.Pack.Description,
			Risk:        f.Pack.Risk,
			Reason:      f.Pack.Reason,
			Commands:    append([]string(nil), f.Pack.Commands...),
			RuleCount:   len(f.Rules) + len(f.Domains),
		})
	}
	return packs, nil
}

// PackByID returns the named pack, or nil.
func PackByID(packs []PackInfo, id string) *PackInfo {
	for i := range packs {
		if packs[i].ID == id {
			return &packs[i]
		}
	}
	return nil
}

// InstalledPackIDs lists pack-managed rule files under rulesDir. The
// result is deterministic (sorted).
func InstalledPackIDs(rulesDir string) []string {
	entries, err := os.ReadDir(rulesDir)
	if err != nil {
		return nil
	}
	var ids []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if id, ok := PackIDFromFileName(e.Name()); ok {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	return ids
}

// InstalledPackPath returns the installed rule file path for a pack id
// (rulesDir/pack-<id>.json).
func InstalledPackPath(rulesDir, id string) string {
	return filepath.Join(rulesDir, PackFileName(id))
}

// InstallPack writes the pack's rule sections to rulesDir/pack-<id>.json
// as a standard rule file (idempotent: an existing file is left
// untouched so user edits are never clobbered — presence means enabled).
// id must be one of the embedded packs.
func InstallPack(rulesDir, id string) (*PackInfo, error) {
	packs, err := LoadPacks()
	if err != nil {
		return nil, err
	}
	pack := PackByID(packs, id)
	if pack == nil {
		return nil, fmt.Errorf("rule pack %q not found", id)
	}
	path := InstalledPackPath(rulesDir, id)
	if _, err := os.Stat(path); err == nil {
		pack.Installed = true
		pack.Path = path
		return pack, nil // already installed (user may have edited it)
	} else if !os.IsNotExist(err) {
		return nil, fmt.Errorf("stat installed pack: %w", err)
	}
	raw, err := packsFS.ReadFile(filepath.Join("packs", id+".json"))
	if err != nil {
		return nil, fmt.Errorf("read embedded pack %s: %w", id, err)
	}
	var f packFile
	if err := json.Unmarshal(raw, &f); err != nil {
		return nil, fmt.Errorf("parse embedded pack %s: %w", id, err)
	}
	out := ruleFile{Rules: f.Rules, Domains: f.Domains}
	body, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return nil, fmt.Errorf("encode pack %s rules: %w", id, err)
	}
	body = append(body, '\n')
	if err := os.MkdirAll(rulesDir, 0o700); err != nil {
		return nil, fmt.Errorf("create rules dir: %w", err)
	}
	if err := os.WriteFile(path, body, 0o644); err != nil {
		return nil, fmt.Errorf("write pack %s: %w", id, err)
	}
	pack.Installed = true
	pack.Path = path
	return pack, nil
}

// UninstallPack removes the pack's rule file from rulesDir (idempotent:
// a missing file is not an error). id must be one of the embedded packs.
func UninstallPack(rulesDir, id string) error {
	packs, err := LoadPacks()
	if err != nil {
		return err
	}
	if PackByID(packs, id) == nil {
		return fmt.Errorf("rule pack %q not found", id)
	}
	path := InstalledPackPath(rulesDir, id)
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("remove pack %s: %w", id, err)
	}
	return nil
}
