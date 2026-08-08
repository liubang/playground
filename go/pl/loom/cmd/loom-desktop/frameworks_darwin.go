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
// Created: 2026/08/08

package main

// Upstream gap: the wails v2.13 darwin frontend references UTType
// (UniformTypeIdentifiers) without declaring the framework in its own cgo
// LDFLAGS. Declaring it here keeps the link self-contained for both
// go build and Bazel (no CGO_LDFLAGS env, no per-rule clinkopts).
// NOTE: the comment group attached to import "C" is the cgo preamble and
// must contain only the directive below.

// #cgo LDFLAGS: -framework UniformTypeIdentifiers
import "C"

// Force real cgo codegen: rules_go collects cgo LDFLAGS only for packages
// that actually reference C (plain `go build` honors the directive either
// way; the blank C reference keeps both toolchains consistent).
var _ C.int
