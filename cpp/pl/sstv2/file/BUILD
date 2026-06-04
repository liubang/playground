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
# Created: 2026/06/04 15:23

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "locator",
    srcs = ["locator.cpp"],
    hdrs = ["locator.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
        "//cpp/pl/sstv2/types:data_type",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "tail",
    srcs = ["tail.cpp"],
    hdrs = ["tail.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:fixed",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "value_file_writer",
    srcs = ["value_file_writer.cpp"],
    hdrs = ["value_file_writer.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:fixed",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "value_file_reader",
    srcs = ["value_file_reader.cpp"],
    hdrs = ["value_file_reader.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":value_file_writer",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "sstable_builder",
    srcs = ["sstable_builder.cpp"],
    hdrs = ["sstable_builder.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":locator",
        ":tail",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
        "//cpp/pl/sstv2/types:data_type",
        "//cpp/pl/sstv2/types:table_schema",
        "//cpp/pl/sstv2/types:variant",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "sstable_reader",
    srcs = ["sstable_reader.cpp"],
    hdrs = ["sstable_reader.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":locator",
        ":tail",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
        "//cpp/pl/sstv2/types:data_type",
        "//cpp/pl/sstv2/types:variant",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
