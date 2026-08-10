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

//go:build unix

package process

// wellKnownToolchainDirs are the conventional install locations for user
// toolchains, in PREPEND priority order (highest first): version-manager
// shims beat personal bins, which beat system-level package managers, so a
// GUI-launched loom resolves the same tools the user's login shell would
// (e.g. mise's python wins over the /usr/bin stub). Directories already on
// the inherited PATH keep their position — the user's explicit order is
// never reordered. They matter when loom is launched from a GUI context
// (Finder/launchd): the process then inherits a sparse default PATH
// ("/usr/bin:/bin:...") without any user toolchain, and sandboxed commands
// inherit it verbatim — "go: command not found" style failures push the
// model toward pointless escalations. "~/" expands against the user's
// home. The mise shims directory alone covers every mise-managed tool (go,
// bazel, node, ...), so versioned install dirs never need listing here.
var wellKnownToolchainDirs = []string{
	// Version-manager shims: the user's chosen tool versions win.
	"~/.local/share/mise/shims",         // mise (go, bazel, node, python, ...)
	"~/.asdf/shims",                     // asdf
	"~/.pyenv/shims",                    // pyenv
	"~/.volta/bin",                      // Volta (node)
	"~/.nix-profile/bin",                // Nix user profile
	"/nix/var/nix/profiles/default/bin", // Nix system profile
	// Personal bin dirs.
	"~/bin",        // classic personal bin
	"~/.local/bin", // pip --user, pipx, uv tools, misc user installs
	"~/go/bin",     // GOPATH binaries (go install)
	"~/.cargo/bin", // rustup / cargo
	"~/.deno/bin",  // deno
	"~/.bun/bin",   // Bun
	"~/.pixi/bin",  // pixi
	// Language/platform toolchains.
	"~/.rd/bin",                            // Rancher Desktop (docker, kubectl, ...)
	"~/Library/Android/sdk/platform-tools", // Android SDK (adb, fastboot)
	// System-level package managers.
	"/opt/homebrew/bin",          // Homebrew (Apple Silicon)
	"/opt/homebrew/sbin",         // Homebrew (Apple Silicon)
	"/usr/local/bin",             // Homebrew (Intel) / manual installs
	"/usr/local/go/bin",          // official Go installer
	"/opt/local/bin",             // MacPorts
	"/snap/bin",                  // snap (Ubuntu)
	"/opt/homebrew/opt/llvm/bin", // keg-only LLVM (clang, clang-format, ...)
}

// wellKnownToolchainGlobs are single-level patterns covering versioned
// install layouts that a static list can never enumerate. Each pattern
// expands to at most maxGlobExpansions directories (filepath.Glob order,
// i.e. lexical). Homebrew keg-only (/opt/homebrew/opt/*\/bin) is
// deliberately NOT globbed: it would prepend dozens of dirs of
// near-duplicate tools.
var wellKnownToolchainGlobs = []string{
	"~/.sdkman/candidates/*/current/bin", // SDKMAN (java, maven, gradle, ...)
	"~/Library/Python/3.*/bin",           // pip --user with the Apple/CLT Python
}

// maxGlobExpansions caps one pattern's expansion so a sprawling versioned
// layout cannot flood the PATH.
const maxGlobExpansions = 8
