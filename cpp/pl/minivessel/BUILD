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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS", "TEST_COPTS")

package(default_visibility = ["//cpp/pl:__subpackages__"])

cc_library(
    name = "types",
    hdrs = ["types.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "active_log_storage",
    hdrs = ["active_log_storage.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":types",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "shared_wal",
    srcs = ["shared_wal.cpp"],
    hdrs = ["shared_wal.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":active_log_storage",
        ":types",
        "//cpp/pl/sstv2/codec:fixed",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "replica_runtime",
    srcs = ["replica_runtime.cpp"],
    hdrs = ["replica_runtime.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":shared_wal",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "filesystem",
    hdrs = ["filesystem.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/sstv2/io:filesystem"],
)

cc_library(
    name = "local_filesystem",
    srcs = ["local_filesystem.cpp"],
    hdrs = ["local_filesystem.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":active_log_storage",
        ":filesystem",
        "//cpp/pl/sstv2/io:local_filesystem",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "minidfs_filesystem",
    srcs = ["minidfs_filesystem.cpp"],
    hdrs = ["minidfs_filesystem.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":filesystem",
        "//cpp/pl/minidfs/client:dfs_client",
        "//cpp/pl/sstv2/io:minidfs_filesystem",
    ],
)

cc_library(
    name = "minidfs_object_store",
    srcs = ["minidfs_object_store.cpp"],
    hdrs = ["minidfs_object_store.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minidfs/client:dfs_client",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_binary(
    name = "minidfs_e2e",
    srcs = ["minidfs_e2e.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":minidfs_filesystem",
        "//cpp/pl/minidfs/client:dfs_client",
        "//cpp/pl/sstv2/io:filesystem",
        "@gflags",
    ],
)

cc_library(
    name = "role_state",
    srcs = ["role_state.cpp"],
    hdrs = ["role_state.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["@abseil-cpp//absl/status"],
)

cc_test(
    name = "replica_runtime_test",
    srcs = ["replica_runtime_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":replica_runtime",
        ":shared_wal",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shared_wal_test",
    srcs = ["shared_wal_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":local_filesystem",
        ":shared_wal",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "local_filesystem_test",
    srcs = ["local_filesystem_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":local_filesystem",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "minidfs_filesystem_test",
    srcs = ["minidfs_filesystem_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":active_log_storage",
        ":minidfs_filesystem",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "role_state_test",
    srcs = ["role_state_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":role_state",
        "@googletest//:gtest_main",
    ],
)
