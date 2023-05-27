#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 01:37
# Last Modified: 2023/05/21 01:37
#
#======================================================================
load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_library(
    name = "arena",
    srcs = [
        "arena.cpp",
    ],
    hdrs = [
        "arena.h",
    ],
    copts = [
        "-std=c++20",
    ],
    visibility = ["//visibility:public"],
)

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
