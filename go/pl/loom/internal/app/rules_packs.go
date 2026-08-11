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

// Rule pack management: list built-in packs with install state, install
// (write the standard rule file into the user rules dir), and uninstall
// (remove it). Every mutation reloads the in-memory policy of all
// assembled workspaces so the change applies immediately without a
// restart (docs/PERMISSION_DESIGN.md — rule packs).
package app

import (
	"context"
	"fmt"

	"github.com/liubang/playground/go/pl/loom/internal/permission"
)

// ListRulePacks returns every embedded pack with its installation state.
// A nil rulesDir yields the packs with installed=false.
func (s *SessionService) ListRulePacks(ctx context.Context) ([]permission.PackInfo, error) {
	packs, err := permission.LoadPacks()
	if err != nil {
		return nil, err
	}
	installed := permission.InstalledPackIDs(s.rulesDir)
	installedSet := make(map[string]bool, len(installed))
	for _, id := range installed {
		installedSet[id] = true
	}
	for i := range packs {
		if installedSet[packs[i].ID] {
			packs[i].Installed = true
			packs[i].Path = permission.InstalledPackPath(s.rulesDir, packs[i].ID)
		}
	}
	return packs, nil
}

// InstallRulePack enables a built-in pack: writes pack-<id>.json into the
// user rules dir (idempotent; existing files are never clobbered), then
// reloads every workspace's policy. Returns the pack info with state.
func (s *SessionService) InstallRulePack(ctx context.Context, id string) (*permission.PackInfo, error) {
	if s.rulesDir == "" {
		return nil, fmt.Errorf("rules directory is not configured")
	}
	info, err := permission.InstallPack(s.rulesDir, id)
	if err != nil {
		return nil, err
	}
	if err := s.reloadAllPolicies(ctx); err != nil {
		s.logger.Warn("rule pack installed but policy reload failed", "pack", id, "error", err)
	}
	return info, nil
}

// UninstallRulePack disables a pack by removing its rule file (idempotent),
// then reloads every workspace's policy.
func (s *SessionService) UninstallRulePack(ctx context.Context, id string) error {
	if s.rulesDir == "" {
		return fmt.Errorf("rules directory is not configured")
	}
	if err := permission.UninstallPack(s.rulesDir, id); err != nil {
		return err
	}
	if err := s.reloadAllPolicies(ctx); err != nil {
		s.logger.Warn("rule pack uninstalled but policy reload failed", "pack", id, "error", err)
	}
	return nil
}

// reloadAllPolicies re-reads rules for every assembled workspace so rule
// pack changes take effect without a restart. A workspace with no live
// policy (not yet assembled) picks the new rules at assembly time.
func (s *SessionService) reloadAllPolicies(ctx context.Context) error {
	var firstErr error
	for _, ws := range s.registry.Bootstraps() {
		if err := ws.ReloadPolicy(ctx); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}
