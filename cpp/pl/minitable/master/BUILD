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
    name = "bounded_task_queue",
    hdrs = ["bounded_task_queue.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "route_table",
    hdrs = ["route_table.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "schema_service",
    hdrs = ["schema_service.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":bounded_task_queue",
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "region_service",
    hdrs = ["region_service.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":bounded_task_queue",
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "heartbeat_monitor",
    hdrs = ["heartbeat_monitor.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/utility",
    ],
)

cc_library(
    name = "snapshot_manager",
    hdrs = ["snapshot_manager.h"],
    visibility = ["//visibility:public"],
    deps = [
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
        ":heartbeat_monitor",
        ":region_service",
        ":route_table",
        ":schema_service",
        ":snapshot_manager",
        "//cpp/pl/utility",
        "@braft//:braft",
    ],
)

# =========================================================================
# Master brpc Service
# =========================================================================

cc_library(
    name = "master_service",
    srcs = ["master.cpp"],
    hdrs = ["master.h"],
    copts = COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":master_sm",
        ":route_table",
        "//cpp/pl/minitable/proto:admin_cc_proto",
        "//cpp/pl/minitable/proto:master_cc_proto",
        "//cpp/pl/minitable/proto:unit_cc_proto",
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
        ":master_service",
        ":master_sm",
        "@brpc//:brpc",
        "@gflags",
    ],
)
