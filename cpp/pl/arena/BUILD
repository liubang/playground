#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 01:37
# Last Modified: 2023/05/21 01:37
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "arena",
    srcs = [
        "arena.cpp",
    ],
    hdrs = [
        "arena.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)

cc_test(
    name = "arena_test",
    srcs = [
        "arena_test.cpp",
    ],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/pl/arena",
        "@googletest//:gtest_main",
    ],
)
