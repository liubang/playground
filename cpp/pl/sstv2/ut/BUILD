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
# Created: 2026/06/04 12:01

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "variant_test",
    srcs = ["types/variant_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/types:variant",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "schema_test",
    srcs = ["types/schema_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "varints_test",
    srcs = ["encode/varints_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/encode:varints",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "stream_vbyte_test",
    srcs = ["encode/stream_vbyte_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/encode:stream_vbyte",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bloom_test",
    srcs = ["bloom/bloom_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/bloom:bloom_builder",
        "//cpp/pl/sstv2/bloom:bloom_reader",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "index_tree_test",
    srcs = ["index/index_tree_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/index:index_block_writer",
        "//cpp/pl/sstv2/index:index_iterator",
        "//cpp/pl/sstv2/index:index_tree_builder",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "metadata_test",
    srcs = ["metadata/metadata_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/metadata:compatibility",
        "//cpp/pl/sstv2/metadata:configuration",
        "//cpp/pl/sstv2/metadata:metadata_section",
        "//cpp/pl/sstv2/metadata:schema_meta",
        "//cpp/pl/sstv2/metadata:statistics",
        "//cpp/pl/sstv2/metadata:user_defined",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "file_test",
    srcs = ["file/file_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/file:locator",
        "//cpp/pl/sstv2/file:sstable_builder",
        "//cpp/pl/sstv2/file:sstable_reader",
        "//cpp/pl/sstv2/file:tail",
        "//cpp/pl/sstv2/file:value_file_reader",
        "//cpp/pl/sstv2/file:value_file_writer",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pattern_test",
    srcs = ["pattern/pattern_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/pattern:pattern_constant",
        "//cpp/pl/sstv2/pattern:pattern_decoder",
        "//cpp/pl/sstv2/pattern:pattern_delta",
        "//cpp/pl/sstv2/pattern:pattern_dict",
        "//cpp/pl/sstv2/pattern:pattern_encoder",
        "//cpp/pl/sstv2/pattern:pattern_none",
        "//cpp/pl/sstv2/pattern:pattern_pfor",
        "//cpp/pl/sstv2/pattern:pattern_selector",
        "//cpp/pl/sstv2/pattern:pattern_stream_vbyte",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "compress_test",
    srcs = ["compress/compress_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/compress:block_compressor",
        "//cpp/pl/sstv2/compress:multi_prefix",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_test",
    srcs = ["block/block_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/block:block_reader",
        "//cpp/pl/sstv2/block:block_writer",
        "//cpp/pl/sstv2/block:column_store",
        "@googletest//:gtest_main",
    ],
)
