# Copyright (c) 2024 The Authors. All rights reserved.
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

load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "sstable",
    srcs = [
        "block.cpp",
        "block_builder.cpp",
        "bloom_filter_policy.cpp",
        "comparator.cpp",
        "filter_block_builder.cpp",
        "filter_block_reader.cpp",
        "filter_policy.cpp",
        "iterator.cpp",
        "options.cpp",
        "sstable_builder.cpp",
        "sstable_format.cpp",
        "table.cpp",
    ],
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/bloom:bloom_filter",
        "//cpp/misc/fs",
        "//cpp/tools",
    ],
)

cc_test(
    name = "encoding_test",
    srcs = [
        "encoding.h",
        "encoding_test.cpp",
    ],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "filter_block_test",
    srcs = ["filter_block_test.cpp"],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "comparator_test",
    srcs = ["comparator_test.cpp"],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_builder_test",
    srcs = ["block_builder_test.cpp"],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "sstable_builder_test",
    srcs = ["sstable_builder_test.cpp"],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

# cc_test(
#     name = "table_test",
#     srcs = ["table_test.cpp"],
#     copts = common_copts + [
#         "-std=c++20",
#     ],
#     linkopts = common_linkopts,
#     deps = [
#         ":sstable",
#         "@abseil-cpp//absl/cleanup",
#         "@googletest//:gtest_main",
#     ],
# )
