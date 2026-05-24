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
# Created: 2026/05/18 00:33

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "json_util",
    hdrs = [
        "json_util.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "jsonrpc",
    srcs = [
        "jsonrpc.cpp",
    ],
    hdrs = [
        "jsonrpc.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":json_util",
        "@simdjson",
    ],
)

cc_library(
    name = "transport",
    srcs = [
        "transport.cpp",
    ],
    hdrs = [
        "transport.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "formatter",
    srcs = [
        "formatter.cpp",
    ],
    hdrs = [
        "formatter.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/flux/syntax:parser",
    ],
)

cc_library(
    name = "server",
    srcs = [
        "server.cpp",
    ],
    hdrs = [
        "server.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":formatter",
        ":json_util",
        ":jsonrpc",
        ":transport",
        "//cpp/pl/flux/analysis:builtin_metadata",
        "//cpp/pl/flux/analysis:semantic_analyzer",
        "//cpp/pl/flux/syntax:parser",
        "@simdjson",
    ],
)

cc_binary(
    name = "flux-ls",
    srcs = [
        "main.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":server",
    ],
)

# ==================== Tests ====================

cc_test(
    name = "json_util_unit_test",
    srcs = [
        "tests/json_util_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":json_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "jsonrpc_unit_test",
    srcs = [
        "tests/jsonrpc_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":jsonrpc",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "transport_unit_test",
    srcs = [
        "tests/transport_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":transport",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "formatter_unit_test",
    srcs = [
        "tests/formatter_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":formatter",
        "//cpp/pl/flux/syntax:parser",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "server_unit_test",
    srcs = [
        "tests/server_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":server",
        "@googletest//:gtest_main",
    ],
)
