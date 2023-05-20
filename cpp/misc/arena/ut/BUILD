#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 01:39
# Last Modified: 2023/05/21 01:39
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "arena_test",
    srcs = [
        "arena_test.cpp",
    ],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/arena",
        "@googletest//:gtest_main",
    ],
)
