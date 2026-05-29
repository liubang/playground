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
    name = "connector",
    srcs = [
        "connector_registry.cpp",
        "connector_registry_init.cpp",
        "memory_source.cpp",
        "mysql_connection_pool.cpp",
        "mysql_source.cpp",
        "sql_builder.cpp",
        "sqlite_source.cpp",
    ],
    hdrs = [
        "connector_runtime.h",
        "connector_registry.h",
        "memory_source.h",
        "mysql_connection_pool.h",
        "mysql_source.h",
        "sql_builder.h",
        "sqlite_source.h",
        "table_source.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/flux/runtime:runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
        "@abseil-cpp//absl/time:time",
        "@boost.asio//:boost.asio",
        "@boost.mysql//:boost.mysql",
        "@sqlite3",
    ],
)

cc_test(
    name = "memory_source_unit_test",
    srcs = [
        "tests/memory_source_unit_test.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":connector",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "sqlite_source_unit_test",
    srcs = [
        "tests/sqlite_source_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":connector",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
        "@sqlite3",
    ],
)

cc_test(
    name = "mysql_source_unit_test",
    srcs = [
        "tests/mysql_source_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":connector",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "connector_runtime_conformance_unit_test",
    srcs = [
        "tests/connector_runtime_conformance_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":connector",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)
