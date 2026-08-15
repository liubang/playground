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
// identity. The version is stamped at BUILD time from the workspace
// status (tools/workspace_status.sh) as "<yyyymmdd>.<git-short-hash>"
// (e.g. "20260815.82f4e2a53") — no hand-maintained release numbers.
//
// Consumers:
//   - Go binaries (cmd/loom, cmd/loom-desktop) carry Version via bazel
//     x_defs on their go_binary targets ({STABLE_LOOM_VERSION}).
//   - macOS packaging renders cmd/loom-desktop/macos/Info.plist.tmpl
//     from the same workspace status keys (cmd/loom-desktop/BUILD).
//
// Unstamped builds — plain `go build`, `go test` — report "dev".
package version

import "strings"

// Version is the full version string, e.g. "20260815.82f4e2a"; "dev"
// when the build was not stamped.
var Version = "dev"

// Release is the numeric-only prefix of Version ("20260815"), suitable
// for CFBundleShortVersionString, which forbids non-numeric parts.
var Release = releaseOf(Version)

// releaseOf strips the git-hash segment: everything before the first
// "." of a stamped "<date>.<hash>"; a hashless value stays as is.
func releaseOf(v string) string {
	if i := strings.IndexByte(v, '.'); i >= 0 {
		return v[:i]
	}
	return v
}
