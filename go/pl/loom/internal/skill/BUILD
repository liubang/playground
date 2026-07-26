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
# Created: 2026/07/26

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "skill_lib",
    srcs = [
        "discover.go",
        "parse.go",
        "prompt.go",
        "render.go",
        "skill.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/skill",
    visibility = ["//visibility:public"],
    deps = [
        "@in_gopkg_yaml_v3//:yaml_v3",
    ],
)

go_test(
    name = "skill_test",
    srcs = [
        "discover_test.go",
        "parse_test.go",
        "render_test.go",
        "skill_test.go",
    ],
    embed = [":skill_lib"],
)
