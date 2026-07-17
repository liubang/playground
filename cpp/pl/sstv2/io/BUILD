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
# Created: 2026/07/16

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "filesystem",
    hdrs = ["filesystem.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "local_filesystem",
    srcs = ["local_filesystem.cpp"],
    hdrs = ["local_filesystem.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":filesystem",
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
        "//cpp/pl/minidfs/common",
        "//cpp/pl/status",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)
