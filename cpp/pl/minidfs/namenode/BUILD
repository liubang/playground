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

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//cpp/pl/minidfs:__subpackages__"])

cc_library(
    name = "datanode_manager",
    srcs = ["datanode_manager.cpp"],
    hdrs = ["datanode_manager.h"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
    ],
)

cc_library(
    name = "placement_manager",
    srcs = ["placement_manager.cpp"],
    hdrs = ["placement_manager.h"],
    deps = [
        ":datanode_manager",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
    ],
)

cc_library(
    name = "block_manager",
    srcs = ["block_manager.cpp"],
    hdrs = ["block_manager.h"],
    deps = [
        ":placement_manager",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
        "@fmt",
    ],
)

cc_library(
    name = "lease_manager",
    srcs = ["lease_manager.cpp"],
    hdrs = ["lease_manager.h"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
    ],
)

cc_library(
    name = "namespace_manager",
    srcs = ["namespace_manager.cpp"],
    hdrs = ["namespace_manager.h"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
        "@fmt",
    ],
)

cc_library(
    name = "replication_manager",
    srcs = ["replication_manager.cpp"],
    hdrs = ["replication_manager.h"],
    deps = [
        ":placement_manager",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
    ],
)

cc_library(
    name = "namenode_service_impl",
    srcs = ["namenode_service_impl.cpp"],
    hdrs = ["namenode_service_impl.h"],
    deps = [
        ":block_manager",
        ":datanode_manager",
        ":lease_manager",
        ":namespace_manager",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "//cpp/pl/status",
        "@brpc//:brpc",
    ],
)

cc_library(
    name = "admin_service_impl",
    srcs = ["admin_service_impl.cpp"],
    hdrs = ["admin_service_impl.h"],
    deps = [
        ":datanode_manager",
        ":namespace_manager",
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/minidfs/protocol:minidfs_cc_proto",
        "//cpp/pl/status",
        "@brpc//:brpc",
    ],
)
