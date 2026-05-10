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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

cc_library(
    name = "dfs_client",
    srcs = ["dfs_client.cpp"],
    hdrs = ["dfs_client.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    visibility = ["//cpp/pl/minidfs:__subpackages__"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "//cpp/pl/status",
        "@brpc//:brpc",
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

cc_binary(
    name = "datanode",
    srcs = ["datanode_main.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:block_reporter",
        "//cpp/pl/minidfs/datanode:data_transfer_service_impl",
        "//cpp/pl/minidfs/datanode:heartbeat_sender",
        "//cpp/pl/minidfs/datanode:local_block_store",
        "//cpp/pl/minidfs/datanode:replication_worker",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "@brpc//:brpc",
        "@fmt",
        "@folly//folly/logging",
        "@gflags",
    ],
)
