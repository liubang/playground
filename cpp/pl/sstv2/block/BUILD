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
# Created: 2026/06/04 13:06

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "block_header",
    srcs = ["block_header.cpp"],
    hdrs = ["block_header.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:fixed",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "data_table",
    srcs = ["data_table.cpp"],
    hdrs = ["data_table.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "column_store",
    srcs = ["column_store.cpp"],
    hdrs = ["column_store.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/pattern:pattern_decoder",
        "//cpp/pl/sstv2/pattern:pattern_selector",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "offset_table",
    srcs = ["offset_table.cpp"],
    hdrs = ["offset_table.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "rowkey_bitmap",
    srcs = ["rowkey_bitmap.cpp"],
    hdrs = ["rowkey_bitmap.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "block_writer",
    srcs = ["block_writer.cpp"],
    hdrs = ["block_writer.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":block_header",
        ":column_store",
        ":data_table",
        ":offset_table",
        ":rowkey_bitmap",
        "//cpp/pl/sstv2/compress:block_compressor",
        "//cpp/pl/sstv2/encode:fixed",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "block_reader",
    srcs = ["block_reader.cpp"],
    hdrs = ["block_reader.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":block_header",
        ":column_store",
        ":data_table",
        ":offset_table",
        ":rowkey_bitmap",
        "//cpp/pl/sstv2/compress:block_compressor",
        "//cpp/pl/sstv2/encode:fixed",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status:statusor",
    ],
)
