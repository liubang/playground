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
        "block_builder.cpp",
        "table_builder.cpp",
    ],
    hdrs = [
        "block_builder.h",
        "encoding.h",
        "filter_block_builder.h",
        "filter_policy.h",
        "table_builder.h",
    ],
    copts = ["-std=c++20"],
    visibility = ["//visibility:public"],
    deps = [
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
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)
