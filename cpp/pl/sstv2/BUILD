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
# Created: 2026/06/05 00:23

load("@rules_cc//cc:defs.bzl", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "varint_test",
    srcs = ["ut/codec/varint_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/codec:varint",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "comparable_test",
    srcs = ["ut/codec/comparable_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/codec:scalar_comparable",
        "//cpp/pl/sstv2/codec:value_comparable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "value_test",
    srcs = ["ut/types/value_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/types:value",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "schema_test",
    srcs = ["ut/types/schema_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/types:internal_schema",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "key_test",
    srcs = ["ut/types/key_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/codec:value_comparable",
        "//cpp/pl/sstv2/types:key",
        "//cpp/pl/sstv2/types:key_comparator",
        "//cpp/pl/sstv2/types:key_factory",
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "column_flag_test",
    srcs = ["ut/types/column_flag_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/types:column_flag",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pattern_test",
    srcs = ["ut/pattern/pattern_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/codec:endian",
        "//cpp/pl/sstv2/pattern:compound",
        "//cpp/pl/sstv2/pattern:raw",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "compress_test",
    srcs = ["ut/compress/compress_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/compress",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_test",
    srcs = ["ut/block/block_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/block",
        "//cpp/pl/sstv2/types:column_flag",
        "//cpp/pl/sstv2/types:internal_row",
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bloom_test",
    srcs = ["ut/bloom/bloom_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/bloom",
        "//cpp/pl/sstv2/types:internal_row",
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "index_tree_test",
    srcs = ["ut/index/index_tree_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/block",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/index",
        "//cpp/pl/sstv2/types:key",
        "//cpp/pl/sstv2/types:key_comparator",
        "//cpp/pl/sstv2/types:key_factory",
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "format_test",
    srcs = ["ut/format/format_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/format:metadata",
        "//cpp/pl/sstv2/format:section",
        "//cpp/pl/sstv2/format:tail",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "sstable_test",
    srcs = ["ut/file/sstable_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/block",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/file:sstable",
        "//cpp/pl/sstv2/format:section",
        "//cpp/pl/sstv2/format:tail",
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)
