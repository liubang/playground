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
# Created: 2026/05/10 22:46

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "mock_metadata_store",
    hdrs = ["mock_metadata_store.h"],
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/metadata:metadata_store",
        "//cpp/pl/status",
    ],
)

cc_test(
    name = "namespace_manager_test",
    srcs = ["namespace_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:namespace_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "lease_manager_test",
    srcs = ["lease_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:lease_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "datanode_manager_test",
    srcs = ["datanode_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:datanode_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "placement_manager_test",
    srcs = ["placement_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:datanode_manager",
        "//cpp/pl/minidfs/namenode:placement_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_manager_test",
    srcs = ["block_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:block_manager",
        "//cpp/pl/minidfs/namenode:datanode_manager",
        "//cpp/pl/minidfs/namenode:placement_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "replication_manager_test",
    srcs = ["replication_manager_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:datanode_manager",
        "//cpp/pl/minidfs/namenode:placement_manager",
        "//cpp/pl/minidfs/namenode:replication_manager",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "regression_test",
    srcs = ["regression_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":mock_metadata_store",
        "//cpp/pl/minidfs/namenode:block_manager",
        "//cpp/pl/minidfs/namenode:datanode_manager",
        "//cpp/pl/minidfs/namenode:lease_manager",
        "//cpp/pl/minidfs/namenode:namespace_manager",
        "//cpp/pl/minidfs/namenode:placement_manager",
        "@googletest//:gtest_main",
    ],
)
