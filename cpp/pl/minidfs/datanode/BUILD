# Copyright (c) 2025 The Authors. All rights reserved.
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
# Created: 2025/06/30 00:47

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/minidfs:__subpackages__"])

cc_library(
    name = "block_format",
    hdrs = ["block_format.h"],
    deps = [
        "//cpp/pl/minidfs/common",
    ],
)

cc_library(
    name = "local_block_store",
    srcs = ["local_block_store.cpp"],
    hdrs = ["local_block_store.h"],
    deps = [
        ":block_format",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
        "@fmt",
    ],
)

cc_library(
    name = "pipeline_receiver",
    srcs = ["pipeline_receiver.cpp"],
    hdrs = ["pipeline_receiver.h"],
    deps = [
        ":local_block_store",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
        "@fmt",
    ],
)

cc_library(
    name = "heartbeat_sender",
    srcs = ["heartbeat_sender.cpp"],
    hdrs = ["heartbeat_sender.h"],
    deps = [
        ":local_block_store",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
        "@brpc",
    ],
)

cc_library(
    name = "block_reporter",
    srcs = ["block_reporter.cpp"],
    hdrs = ["block_reporter.h"],
    deps = [
        ":local_block_store",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
        "@brpc",
    ],
)

cc_library(
    name = "replication_worker",
    srcs = ["replication_worker.cpp"],
    hdrs = ["replication_worker.h"],
    deps = [
        ":local_block_store",
        "//cpp/pl/status",
        "@brpc",
    ],
)

cc_library(
    name = "data_transfer_service_impl",
    srcs = ["data_transfer_service_impl.cpp"],
    hdrs = ["data_transfer_service_impl.h"],
    deps = [
        ":block_reporter",
        ":local_block_store",
        ":pipeline_receiver",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "//cpp/pl/status",
        "@brpc",
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
        "@brpc",
        "@fmt",
        "@gflags",
    ],
)
