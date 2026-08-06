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
# Created: 2026/08/02

load("@rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "memory_lib",
    srcs = [
        "store.go",
        "tools.go",
        "prompt.go",
        "extract.go",
        "consolidate.go",
        "jobs.go",
        "pipeline.go",
        "redact.go",
    ],
    importpath = "github.com/liubang/playground/go/pl/loom/internal/memory",
    visibility = ["//visibility:public"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
    ],
)

go_test(
    name = "memory_test",
    srcs = [
        "store_test.go",
        "tools_test.go",
        "prompt_test.go",
        "extract_test.go",
        "consolidate_test.go",
        "redact_test.go",
        "pipeline_test.go",
    ],
    embed = [":memory_lib"],
    deps = [
        "//go/pl/loom/internal/domain:domain_lib",
        "//go/pl/loom/internal/fakes:fakes_lib",
        "//go/pl/loom/internal/session:session_lib",
    ],
)
