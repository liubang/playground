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
# Created: 2026/05/10 23:13

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "flux_cli",
    srcs = [
        "flux_cli.cpp",
    ],
    hdrs = [
        "flux_cli.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/ascii_table:pretty",
        "//cpp/pl/flux/common",
        "//cpp/pl/flux/execution",
        "//cpp/pl/flux/runtime:runtime_builtin",
        "//cpp/pl/flux/runtime:runtime_exec",
        "//cpp/pl/flux/syntax:parser",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@simdjson",
    ],
)

cc_binary(
    name = "flux",
    srcs = [
        "flux.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":flux_cli",
    ],
)

cc_test(
    name = "flux_cli_unit_test",
    srcs = [
        "tests/flux_cli_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:all_examples",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":flux_cli",
        "@googletest//:gtest_main",
    ],
)

sh_test(
    name = "flux_cli_smoke_test",
    srcs = [
        "tests/flux_cli_smoke_test.sh",
    ],
    data = [
        ":flux",
        "//cpp/pl/flux/examples:all_examples",
    ],
)

sh_test(
    name = "stdlib_conformance_test",
    srcs = [
        "tests/stdlib_conformance_test.sh",
    ],
    data = [
        ":flux",
        "//cpp/pl/flux/examples:all_examples",
        "//cpp/pl/flux/examples:cross_source_data",
    ],
)
