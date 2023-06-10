#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/26 16:42
# Last Modified: 2023/05/26 16:42
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "fs",
    srcs = [
        "fs.cpp",
        "posix_fs.cpp",
    ],
    hdrs = ["fs.h"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/tools",
    ],
)

cc_test(
    name = "fs_test",
    srcs = [
        "fs_test.cpp",
    ],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":fs",
        "@googletest//:gtest_main",
    ],
)
