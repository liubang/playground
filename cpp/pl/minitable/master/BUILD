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
# Created: 2026/06/23 23:47

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_proto_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

# =========================================================================
# Core libraries (header-only)
# =========================================================================

cc_library(
    name = "metadata",
    hdrs = ["metadata.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/minitable/proto:common_cc_proto",
        "//cpp/pl/thread:thread_pool",
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "unit_server_manager",
    hdrs = ["unit_server_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/thread:thread_pool",
        "//cpp/pl/utility",
    ],
)

# =========================================================================
# Master StateMachine
# =========================================================================

cc_library(
    name = "master_sm",
    srcs = ["master_sm.cpp"],
    hdrs = ["master_sm.h"],
    copts = COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":metadata",
        ":unit_server_manager",
        "@braft//:braft",
    ],
)

# =========================================================================
# brpc Service 实现 — 三个独立 service, 各自可绑定不同端口
# =========================================================================

cc_library(
    name = "master_service",
    srcs = ["master_service_impl.cpp"],
    hdrs = ["master_service_impl.h"],
    copts = COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":master_sm",
        ":metadata",
        "//cpp/pl/minitable/proto:common_cc_proto",
        "//cpp/pl/minitable/proto:master_cc_proto",
        "@brpc//:brpc",
    ],
)

cc_library(
    name = "admin_service",
    srcs = ["admin_service_impl.cpp"],
    hdrs = ["admin_service_impl.h"],
    copts = COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":master_sm",
        "//cpp/pl/minitable/proto:admin_cc_proto",
        "//cpp/pl/minitable/proto:common_cc_proto",
        "@brpc//:brpc",
    ],
)

cc_library(
    name = "unit_server_service",
    srcs = ["unit_server_service_impl.cpp"],
    hdrs = ["unit_server_service_impl.h"],
    copts = COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":master_sm",
        "//cpp/pl/minitable/proto:common_cc_proto",
        "//cpp/pl/minitable/proto:unit_server_cc_proto",
        "@brpc//:brpc",
    ],
)

# =========================================================================
# Master binary
# =========================================================================

cc_binary(
    name = "master",
    srcs = ["master_main.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":admin_service",
        ":master_service",
        ":master_sm",
        ":unit_server_service",
        "@brpc//:brpc",
        "@gflags",
    ],
)
