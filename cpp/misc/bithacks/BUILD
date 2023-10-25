#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/10/25 19:29
# Last Modified: 2023/10/25 19:29
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "bithacks",
    hdrs = ["all.h"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)

cc_test(
    name = "test_bithacks",
    srcs = [
        "all.h",
        "test.cpp",
    ],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@googletest//:gtest_main",
    ],
)
