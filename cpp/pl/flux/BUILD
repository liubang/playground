# Copyright (c) 2023 The Authors. All rights reserved.
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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
# load("@rules_ragel//ragel:ragel.bzl", "ragel")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS_WITH_ASAN",
    "LINKOPTS_WITH_ASAN",
)

package(default_visibility = ["//visibility:public"])

# ragel(
#     name = "scanner_generated",
#     src = "scanner.rl",
#     data = [
#         "unicode.rl",
#     ],
#     language = "c++",
#     ragel_options = ["-I ."],
# )

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
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
)

cc_binary(
    name = "scanner_test",
    srcs = [
        "scanner_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":scanner",
    ],
)

cc_test(
    name = "scanner_unit_test",
    srcs = [
        "scanner_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":scanner",
        "@googletest//:gtest_main",
    ],
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
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":scanner",
        "//cpp/pl/lang",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_library(
    name = "runtime_value",
    srcs = [
        "runtime_value.cpp",
    ],
    hdrs = [
        "runtime_value.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "runtime_env",
    srcs = [
        "runtime_env.cpp",
    ],
    hdrs = [
        "runtime_env.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "runtime_builtin",
    srcs = [
        "runtime_builtin.cpp",
    ],
    hdrs = [
        "runtime_builtin.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_eval",
        ":runtime_env",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "runtime_eval",
    srcs = [
        "runtime_eval.cpp",
    ],
    hdrs = [
        "runtime_eval.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
        ":runtime_env",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "runtime_exec",
    srcs = [
        "runtime_exec.cpp",
    ],
    hdrs = [
        "runtime_exec.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_builtin",
        ":runtime_eval",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "flux_cli",
    srcs = [
        "flux_cli.cpp",
    ],
    hdrs = [
        "flux_cli.h",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        "//cpp/pl/ascii_table:pretty",
        ":parser",
        ":runtime_builtin",
        ":runtime_exec",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@simdjson//:simdjson",
    ],
)

cc_binary(
    name = "flux",
    srcs = [
        "flux.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":flux_cli",
    ],
)

cc_binary(
    name = "parser_test",
    srcs = [
        "parser_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
    ],
)

cc_test(
    name = "parser_unit_test",
    srcs = [
        "parser_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_value_unit_test",
    srcs = [
        "runtime_value_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_value",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_env_unit_test",
    srcs = [
        "runtime_env_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_env",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_eval_unit_test",
    srcs = [
        "runtime_eval_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_builtin",
        ":runtime_eval",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_exec_unit_test",
    srcs = [
        "runtime_exec_unit_test.cpp",
    ],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_exec",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "flux_cli_unit_test",
    srcs = [
        "flux_cli_unit_test.cpp",
    ],
    data = glob([
        "examples/**/*",
    ]),
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":flux_cli",
        "@googletest//:gtest_main",
    ],
)
