#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:38
# Last Modified: 2023/05/21 22:38
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_library(
    name = "bloom_filter",
    srcs = ["bloom_filter.cpp"],
    hdrs = ["bloom_filter.h"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/hash",
    ],
)

cc_test(
    name = "bloom_filter_test",
    srcs = [
        "bloom_filter_test.cpp",
    ],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/bloom:bloom_filter",
        "@googletest//:gtest_main",
    ],
)
