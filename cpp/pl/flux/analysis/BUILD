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
# Created: 2026/05/24 11:04

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "builtin_metadata",
    srcs = [
        "builtin_metadata.cpp",
    ],
    hdrs = [
        "builtin_metadata.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "semantic_model",
    srcs = [
        "semantic_model.cpp",
    ],
    hdrs = [
        "semantic_model.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/flux/syntax:parser",
    ],
)

cc_library(
    name = "semantic_analyzer",
    srcs = [
        "semantic_analyzer.cpp",
    ],
    hdrs = [
        "semantic_analyzer.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":builtin_metadata",
        ":semantic_model",
        "//cpp/pl/flux/syntax:parser",
    ],
)

cc_test(
    name = "builtin_metadata_unit_test",
    srcs = [
        "tests/builtin_metadata_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":builtin_metadata",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "semantic_analyzer_unit_test",
    srcs = [
        "tests/semantic_analyzer_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":semantic_analyzer",
        "//cpp/pl/flux/syntax:parser",
        "@googletest//:gtest_main",
    ],
)
