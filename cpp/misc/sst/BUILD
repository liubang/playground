#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/26 16:34
# Last Modified: 2023/05/26 16:34
#
#======================================================================
load("//cpp:opts.bzl", "common_copts", "common_linkopts")

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
        "options.cpp",
        "sstable_builder.cpp",
        "sstable_format.cpp",
    ],
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/bloom:bloom_filter",
        "//cpp/misc/fs",
        "//cpp/tools",
        "@zlib",
    ],
)

cc_test(
    name = "encoding_test",
    srcs = [
        "encoding.h",
        "encoding_test.cpp",
    ],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "filter_block_test",
    srcs = ["filter_block_test.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "comparator_test",
    srcs = ["comparator_test.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        ":sstable",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "block_builder_test",
    srcs = ["block_builder_test.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        ":sstable",
        "@abseil-cpp//absl/cleanup",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "sstable_builder_test",
    srcs = ["sstable_builder_test.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        ":sstable",
        "@abseil-cpp//absl/cleanup",
        "@googletest//:gtest_main",
    ],
)
