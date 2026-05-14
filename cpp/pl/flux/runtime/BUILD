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

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "runtime_value",
    srcs = [
        "runtime_value.cpp",
    ],
    hdrs = [
        "runtime_page.h",
        "runtime_value.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/flux/common",
        "//cpp/pl/flux/plan",
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
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_value",
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
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_env",
        "//cpp/pl/flux/common",
        "//cpp/pl/flux/syntax:parser",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "runtime_builtin_table_helpers",
    hdrs = [
        "runtime_builtin_table_helpers.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_env",
        ":runtime_eval",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "runtime_builtin_aggregate_helpers",
    hdrs = [
        "runtime_builtin_aggregate_helpers.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_builtin_table_helpers",
        "//cpp/pl/flux/common",
    ],
)

cc_library(
    name = "runtime_builtin",
    srcs = [
        "runtime_builtin.cpp",
        "runtime_builtin_mysql.cpp",
        "runtime_builtin_package.cpp",
        "runtime_builtin_scalar.cpp",
        "runtime_builtin_sqlite.cpp",
        "runtime_builtin_table.cpp",
        "runtime_builtin_universe_aggregate.cpp",
        "runtime_builtin_universe_core.cpp",
        "runtime_builtin_universe_inspect.cpp",
        "runtime_builtin_universe_join.cpp",
        "runtime_builtin_universe_transform.cpp",
        "runtime_builtin_universe_window.cpp",
    ],
    hdrs = [
        "runtime_builtin.h",
        "runtime_builtin_aggregate_helpers.h",
        "runtime_builtin_package.h",
        "runtime_builtin_table_helpers.h",
        "runtime_builtin_time_helpers.h",
        "runtime_builtin_universe.h",
        "runtime_builtin_window_helpers.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_env",
        ":runtime_eval",
        "//cpp/pl/flux/common",
        "//cpp/pl/flux/connector",
        "//cpp/pl/flux/execution",
        "//cpp/pl/flux/optimizer",
        "//cpp/pl/flux/syntax:parser",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/time:time",
        "@abseil-cpp//absl/time/internal/cctz:civil_time",
        "@abseil-cpp//absl/time/internal/cctz:time_zone",
        "@simdjson",
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
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_builtin",
        ":runtime_eval",
        "//cpp/pl/flux/common",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_test(
    name = "runtime_value_unit_test",
    srcs = [
        "tests/runtime_value_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_value",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_page_unit_test",
    srcs = [
        "tests/runtime_page_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_value",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_env_unit_test",
    srcs = [
        "tests/runtime_env_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":runtime_env",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_eval_unit_test",
    srcs = [
        "tests/runtime_eval_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
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
        "tests/runtime_exec_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":runtime_exec",
        "//cpp/pl/flux/execution",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)
