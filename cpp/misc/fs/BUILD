#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/26 16:42
# Last Modified: 2023/05/26 16:42
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_library(
    name = "fs",
    srcs = [
        "fs.cpp",
        "posix_fs.cpp",
    ],
    hdrs = ["fs.h"],
    copts = ["-std=c++20"],
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
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        ":fs",
        "@googletest//:gtest_main",
    ],
)
