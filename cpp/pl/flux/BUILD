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
# Created: 2023/11/01 20:29

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

# load("@rules_ragel//ragel:ragel.bzl", "ragel")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS_WITH_ASAN",
    "LINKOPTS_WITH_ASAN",
)

package(default_visibility = ["//visibility:public"])

# ragel(
#     name = "scanner_generated",
#     src = "syntax/scanner.rl",
#     data = [
#         "syntax/unicode.rl",
#     ],
#     language = "c++",
#     ragel_options = ["-I ."],
# )

cc_library(
    name = "common",
    hdrs = [
        "common/compat.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
)

cc_library(
    name = "scanner",
    srcs = [
        "syntax/scanner.cpp",
        "syntax/scanner_generated.cc",
    ],
    hdrs = [
        "syntax/scanner.h",
        "syntax/token.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
)

cc_binary(
    name = "scanner_test",
    srcs = [
        "syntax/tests/scanner_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":scanner",
    ],
)

cc_test(
    name = "scanner_unit_test",
    srcs = [
        "syntax/tests/scanner_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":scanner",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "parser",
    srcs = [
        "syntax/ast.cpp",
        "syntax/ast_debug.cpp",
        "syntax/parser.cpp",
        "syntax/strconv.cpp",
    ],
    hdrs = [
        "syntax/ast.h",
        "syntax/ast_debug.h",
        "syntax/parser.h",
        "syntax/strconv.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
        ":scanner",
        "//cpp/pl/lang",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_library(
    name = "plan",
    hdrs = [
        "plan/physical_plan.h",
        "plan/plan_node.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
)

cc_library(
    name = "optimizer",
    srcs = [
        "optimizer/cbo.cpp",
        "optimizer/explain.cpp",
        "optimizer/rbo.cpp",
    ],
    hdrs = [
        "optimizer/cbo.h",
        "optimizer/explain.h",
        "optimizer/rbo.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":connector",
        ":plan",
        ":runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_test(
    name = "optimizer_cbo_unit_test",
    srcs = [
        "optimizer/tests/cbo_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = [
        "examples/cross_source/metrics.db",
    ],
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":optimizer",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "optimizer_rbo_unit_test",
    srcs = [
        "optimizer/tests/rbo_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = [
        "examples/cross_source/metrics.db",
    ],
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":optimizer",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "execution",
    srcs = [
        "execution/materializer.cpp",
        "execution/physical_executor.cpp",
    ],
    hdrs = [
        "execution/materializer.h",
        "execution/physical_executor.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":optimizer",
        ":runtime_builtin_aggregate_helpers",
        ":runtime_builtin_table_helpers",
        ":runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "runtime_value",
    srcs = [
        "runtime/runtime_value.cpp",
    ],
    hdrs = [
        "runtime/runtime_value.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
        ":plan",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "runtime_env",
    srcs = [
        "runtime/runtime_env.cpp",
    ],
    hdrs = [
        "runtime/runtime_env.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "connector",
    srcs = [
        "connector/connector_registry.cpp",
        "connector/connector_registry_init.cpp",
        "connector/memory_source.cpp",
        "connector/mysql_source.cpp",
        "connector/sql_builder.cpp",
        "connector/sqlite_source.cpp",
    ],
    hdrs = [
        "connector/connector_registry.h",
        "connector/memory_source.h",
        "connector/mysql_source.h",
        "connector/sql_builder.h",
        "connector/sqlite_source.h",
        "connector/table_source.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
        "@abseil-cpp//absl/time:time",
        "@boost.mysql//:boost.mysql",
        "@boost.asio//:boost.asio",
        "@sqlite3",
    ],
)

cc_test(
    name = "connector_memory_source_unit_test",
    srcs = [
        "connector/tests/memory_source_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":connector",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "connector_sqlite_source_unit_test",
    srcs = [
        "connector/tests/sqlite_source_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = [
        "examples/cross_source/metrics.db",
    ],
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":connector",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "connector_mysql_source_unit_test",
    srcs = [
        "connector/tests/mysql_source_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = [
        "examples/cross_source/mysql_metrics.sql",
    ],
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":connector",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "runtime_builtin",
    srcs = [
        "runtime/runtime_builtin.cpp",
        "runtime/runtime_builtin_mysql.cpp",
        "runtime/runtime_builtin_package.cpp",
        "runtime/runtime_builtin_scalar.cpp",
        "runtime/runtime_builtin_sqlite.cpp",
        "runtime/runtime_builtin_table.cpp",
        "runtime/runtime_builtin_universe_aggregate.cpp",
        "runtime/runtime_builtin_universe_core.cpp",
        "runtime/runtime_builtin_universe_inspect.cpp",
        "runtime/runtime_builtin_universe_join.cpp",
        "runtime/runtime_builtin_universe_transform.cpp",
        "runtime/runtime_builtin_universe_window.cpp",
    ],
    hdrs = [
        "runtime/runtime_builtin.h",
        "runtime/runtime_builtin_aggregate_helpers.h",
        "runtime/runtime_builtin_package.h",
        "runtime/runtime_builtin_table_helpers.h",
        "runtime/runtime_builtin_time_helpers.h",
        "runtime/runtime_builtin_universe.h",
        "runtime/runtime_builtin_window_helpers.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
        ":connector",
        ":execution",
        ":optimizer",
        ":parser",
        ":runtime_env",
        ":runtime_eval",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/time:time",
        "@abseil-cpp//absl/time/internal/cctz:civil_time",
        "@abseil-cpp//absl/time/internal/cctz:time_zone",
        "@simdjson",
    ],
)

cc_library(
    name = "runtime_eval",
    srcs = [
        "runtime/runtime_eval.cpp",
    ],
    hdrs = [
        "runtime/runtime_eval.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
        ":parser",
        ":runtime_env",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "runtime_builtin_table_helpers",
    hdrs = [
        "runtime/runtime_builtin_table_helpers.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
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
        "runtime/runtime_builtin_aggregate_helpers.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_builtin_table_helpers",
    ],
)

cc_library(
    name = "runtime_exec",
    srcs = [
        "runtime/runtime_exec.cpp",
    ],
    hdrs = [
        "runtime/runtime_exec.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
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
        "cli/flux_cli.cpp",
    ],
    hdrs = [
        "cli/flux_cli.h",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":common",
        ":execution",
        ":parser",
        ":runtime_builtin",
        ":runtime_exec",
        "//cpp/pl/ascii_table:pretty",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@simdjson",
    ],
)

cc_binary(
    name = "flux",
    srcs = [
        "cli/flux.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":flux_cli",
    ],
)

cc_binary(
    name = "parser_test",
    srcs = [
        "syntax/tests/parser_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
    ],
)

cc_test(
    name = "parser_unit_test",
    srcs = [
        "syntax/tests/parser_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "strconv_unit_test",
    srcs = [
        "syntax/tests/strconv_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":parser",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_value_unit_test",
    srcs = [
        "runtime/tests/runtime_value_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":runtime_value",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_env_unit_test",
    srcs = [
        "runtime/tests/runtime_env_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
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
        "runtime/tests/runtime_eval_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
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
        "runtime/tests/runtime_exec_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = [
        "examples/cross_source/metrics.db",
    ],
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":execution",
        ":runtime_exec",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "flux_cli_unit_test",
    srcs = [
        "cli/tests/flux_cli_unit_test.cpp",
    ],
    copts = COPTS_WITH_ASAN,
    data = glob([
        "examples/**/*",
    ]),
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":flux_cli",
        "@googletest//:gtest_main",
    ],
)

sh_test(
    name = "flux_cli_smoke_test",
    srcs = [
        "cli/tests/flux_cli_smoke_test.sh",
    ],
    data = [
        ":flux",
    ] + glob([
        "examples/**/*",
    ]),
)

sh_test(
    name = "stdlib_conformance_test",
    srcs = [
        "cli/tests/stdlib_conformance_test.sh",
    ],
    data = [
        ":flux",
        "examples/cross_source/metrics.db",
    ] + glob([
        "examples/stdlib_conformance/**/*",
    ]),
)
