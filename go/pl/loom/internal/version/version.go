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

// Package version is the single source of truth for the Loom version
// identity. The VERSION file in this directory holds the canonical string;
// every consumer derives from it:
//
//   - Go binaries (cmd/loom, cmd/loom-desktop) read Version/Release below.
//   - macOS packaging renders cmd/loom-desktop/macos/Info.plist.tmpl from
//     the same VERSION file at build time (see cmd/loom-desktop/BUILD).
//
// To cut a new version, edit VERSION and nothing else.
package version

import (
	_ "embed"
	"strings"
)

//go:embed VERSION
var rawVersion string

var (
	// Version is the full version string, e.g. "0.2.0-dev".
	Version = strings.TrimSpace(rawVersion)
	// Release is Version without any pre-release suffix, e.g. "0.2.0".
	// It is suitable for CFBundleShortVersionString, which forbids suffixes.
	Release = strings.SplitN(Version, "-", 2)[0]
)
