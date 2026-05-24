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

load("@rules_cc//cc:defs.bzl", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "local_block_store_test",
    srcs = ["local_block_store_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:local_block_store",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_reporter_test",
    srcs = ["block_reporter_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:block_reporter",
        "//cpp/pl/minidfs/datanode:local_block_store",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "heartbeat_sender_test",
    srcs = ["heartbeat_sender_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:heartbeat_sender",
        "//cpp/pl/minidfs/datanode:local_block_store",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pipeline_receiver_test",
    srcs = ["pipeline_receiver_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/common",
        "//cpp/pl/minidfs/datanode:local_block_store",
        "//cpp/pl/minidfs/datanode:pipeline_receiver",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "replication_worker_test",
    srcs = ["replication_worker_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:local_block_store",
        "//cpp/pl/minidfs/datanode:replication_worker",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "regression_test",
    srcs = ["regression_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/datanode:local_block_store",
        "@googletest//:gtest_main",
    ],
)
