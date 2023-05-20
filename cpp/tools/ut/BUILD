#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:36
# Last Modified: 2023/05/21 00:36
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "ip_test",
    srcs = [
        "ip_test.cpp",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/tools",
        "@googletest//:gtest_main",
    ],
)
