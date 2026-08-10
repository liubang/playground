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

//go:build !unix

package process

// No well-known toolchain directories on non-unix platforms yet: the GUI
// sparse-PATH problem this list solves is macOS/Linux-specific (Windows
// GUI processes inherit the full user environment from the registry).
// When Windows support lands, define its list here (mise shims, cargo,
// scoop, ...) — the empty list makes the augmentation an explicit no-op
// rather than silently inheriting unix paths.
var wellKnownToolchainDirs []string

var wellKnownToolchainGlobs []string

const maxGlobExpansions = 8
