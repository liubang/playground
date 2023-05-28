#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/28 21:00
# Last Modified: 2023/05/28 21:00
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_library(
    name = "json",
    hdrs = ["json.h"],
    copts = ["-std=c++20"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "json_test",
    srcs = ["json_test.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        ":json",
        "@googletest//:gtest_main",
    ],
)
