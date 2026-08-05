# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2026/08/05

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "web",
    srcs = ["web.go"],
    # The embedded SPA (docs/WEB_DESIGN.md §3.1): zero build chain, native
    # ES modules + vendored deps (VENDORED.md), served as-is.
    embedsrcs = glob(["static/**"]),
    importpath = "github.com/liubang/playground/go/pl/loom/internal/server/web",
    visibility = ["//go/pl/loom:__subpackages__"],
)

go_test(
    name = "web_test",
    srcs = ["web_test.go"],
    embed = [":web"],
)
