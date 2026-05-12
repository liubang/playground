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
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "scanner",
    srcs = [
        "scanner.cpp",
        "scanner_generated.cc",
    ],
    hdrs = [
        "scanner.h",
        "token.h",
    ],
    copts = COPTS,
    includes = ["."],
    linkopts = LINKOPTS,
)

cc_library(
    name = "parser",
    srcs = [
        "ast.cpp",
        "ast_debug.cpp",
        "parser.cpp",
        "strconv.cpp",
    ],
    hdrs = [
        "ast.h",
        "ast_debug.h",
        "parser.h",
        "strconv.h",
    ],
    copts = COPTS,
    includes = ["."],
    linkopts = LINKOPTS,
    deps = [
        ":scanner",
        "//cpp/pl/flux/common",
        "//cpp/pl/lang",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_binary(
    name = "scanner_test",
    srcs = [
        "tests/scanner_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":scanner",
    ],
)

cc_test(
    name = "scanner_unit_test",
    srcs = [
        "tests/scanner_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":scanner",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "parser_test",
    srcs = [
        "tests/parser_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":parser",
    ],
)

cc_test(
    name = "parser_unit_test",
    srcs = [
        "tests/parser_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":parser",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "strconv_unit_test",
    srcs = [
        "tests/strconv_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":parser",
        "@googletest//:gtest_main",
    ],
)
