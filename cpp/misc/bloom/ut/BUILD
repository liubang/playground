#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:40
# Last Modified: 2023/05/21 22:40
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

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
