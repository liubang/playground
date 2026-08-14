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
// Created: 2026/07/26

package config

import (
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
)

// LoadOptions controls loading behavior.
type LoadOptions struct {
	// RequireProviders demands at least one provider (chat/run/resume).
	// Offline commands (sessions/inspect/gc/rules) set false: they only
	// need storage/rules and must work before any provider is configured.
	RequireProviders bool
	// Logger receives deprecation and permission warnings; nil discards.
	Logger *slog.Logger
}

// ErrConfigNotFound reports that the config file does not exist. Agent
// entry points use it to trigger first-run bootstrap (auto-create the
// starter template at the default path); offline commands treat a missing
// file as empty defaults and never see it.
var ErrConfigNotFound = errors.New("config file not found")

// Load reads, validates, and resolves the YAML config at
// <home>/config.yaml. It is the single configuration entry point for
// every loom command (§3.1): no env overlay, no silent defaults — any
// problem is a hard error whose message names the file, field, and
// cause. home is the loom home (data root): LOOM_HOME is the only
// locator, and the file never names its own home.
func Load(home string, opts LoadOptions, lookup EnvLookup) (*ResolvedConfig, error) {
	if lookup == nil {
		lookup = os.LookupEnv
	}
	logger := opts.Logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	baseDir, err := filepath.Abs(home)
	if err != nil {
		return nil, fmt.Errorf("config: resolve loom home: %w", err)
	}
	path := ConfigPathForHome(baseDir)
	raw, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			if !opts.RequireProviders {
				return resolve(&File{}, baseDir, lookup)
			}
			return nil, fmt.Errorf("%w: %s\n\ncreate one (or run `loom config init`):\n\n%s", ErrConfigNotFound, path, minimalExample)
		}
		return nil, fmt.Errorf("read config: %w", err)
	}

	// ParseFile treats an empty (or comment-only) file as "no content" so
	// offline commands keep working with defaults; agent entries still
	// fail below on the missing providers. Unknown keys are typos —
	// fail fast, never ignore.
	f, err := ParseFile(raw)
	if err != nil {
		return nil, fmt.Errorf("parse %s: %w", path, err)
	}
	if opts.RequireProviders && len(f.Providers) == 0 {
		return nil, fmt.Errorf("%s: at least one provider is required\n\nminimal example:\n\n%s", path, minimalExample)
	}
	warnPlaintextKeyPermissions(path, *f, logger)
	return resolve(f, baseDir, lookup)
}

// warnPlaintextKeyPermissions nudges the user toward 0600 when the file
// carries inline secrets and is readable by group/other. Advisory only —
// inline keys are a supported configuration form (§5), not an error.
func warnPlaintextKeyPermissions(path string, f File, logger *slog.Logger) {
	hasInlineSecret := f.Tracing.PublicKey != "" || f.Tracing.SecretKey != ""
	for _, p := range f.Providers {
		if p.APIKey != "" {
			hasInlineSecret = true
		}
	}
	if !hasInlineSecret {
		return
	}
	info, err := os.Stat(path)
	if err != nil {
		return
	}
	if info.Mode().Perm()&0o077 != 0 {
		logger.Warn("config file contains plaintext keys and is readable by group/other users; consider chmod 600",
			"path", path)
	}
}
