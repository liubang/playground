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
# Created: 2026/05/06 20:04

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

cc_library(
    name = "dfs_client",
    srcs = [
        "dfs_client.cpp",
        "dfs_output_stream.cpp",
    ],
    hdrs = [
        "dfs_client.h",
        "dfs_input_stream.h",
        "dfs_output_stream.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    visibility = ["//cpp/pl/minidfs:__subpackages__"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "//cpp/pl/status",
        "@brpc",
        "@fmt",
        "@folly//folly/logging",
    ],
)

cc_binary(
    name = "minidfs",
    srcs = ["cli_main.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":dfs_client",
        "//cpp/pl/ascii_table:pretty",
        "@fmt",
        "@gflags",
    ],
)

cc_test(
    name = "dfs_output_stream_test",
    srcs = ["dfs_output_stream_test.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    tags = ["no-sandbox"],
    deps = [
        ":dfs_client",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "@brpc",
        "@googletest//:gtest_main",
    ],
)
