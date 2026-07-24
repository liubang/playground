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
# Created: 2026/07/24

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "prompt",
    srcs = ["prompt.go"],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/prompt",
    visibility = ["//go/pl/loom:__subpackages__"],
    deps = ["//go/pl/loom/internal/domain:domain_lib"],
)

go_test(
    name = "prompt_test",
    srcs = ["prompt_test.go"],
    embed = [":prompt"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "@com_github_stretchr_testify//assert",
        "@com_github_stretchr_testify//require",
    ],
)
